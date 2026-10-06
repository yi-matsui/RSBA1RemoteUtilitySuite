/*
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, Yi Matsui
 */
#include "serial_port.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rs_error.h"
#include "rs_platform.h"

static RS_THREAD_LOCAL int g_last_os_error;

int serial_last_os_error(void)
{
    return g_last_os_error;
}

int serial_read(serial_port_t *port, void *buf, size_t cap, size_t *got)
{
    if (!serial_is_open(port) || buf == NULL || got == NULL)
        return RS_ERR_INVALID_ARG;
    *got = 0;
    return port->ops->read(port->st, buf, cap, got);
}

int serial_write(serial_port_t *port, const void *buf, size_t len)
{
    if (!serial_is_open(port) || (buf == NULL && len > 0))
        return RS_ERR_INVALID_ARG;
    if (len == 0)
        return RS_OK;
    return port->ops->write(port->st, buf, len);
}

void serial_close(serial_port_t *port)
{
    if (!serial_is_open(port))
        return;
    port->ops->close(port->st);
    port->ops = NULL;
    port->st = NULL;
}

#ifdef _WIN32
/* ======================================================================== */
#include <windows.h>

typedef struct win_serial {
    HANDLE h;
} win_serial_t;

static int win_error(DWORD err)
{
    g_last_os_error = (int)err;
    switch (err) {
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:     return RS_ERR_NOT_FOUND;
    case ERROR_ACCESS_DENIED:
    case ERROR_SHARING_VIOLATION:  return RS_ERR_BUSY;  /* COM ポートは排他オープン */
    default:                       return RS_ERR_SYSTEM;
    }
}

static int win_read(void *st, void *buf, size_t cap, size_t *got)
{
    win_serial_t *s = st;
    DWORD n = 0;
    DWORD want = cap > 0x7fffffffu ? 0x7fffffffu : (DWORD)cap;

    if (!ReadFile(s->h, buf, want, &n, NULL)) {
        g_last_os_error = (int)GetLastError();
        return RS_ERR_IO;
    }
    *got = n;
    return RS_OK;
}

static int win_write(void *st, const void *buf, size_t len)
{
    win_serial_t *s = st;
    DWORD n = 0;

    if (len > 0x7fffffffu)
        return RS_ERR_TOO_LARGE;
    if (!WriteFile(s->h, buf, (DWORD)len, &n, NULL)) {
        g_last_os_error = (int)GetLastError();
        return RS_ERR_IO;
    }
    return n == len ? RS_OK : RS_ERR_TIMEOUT;
}

static void win_close(void *st)
{
    win_serial_t *s = st;
    CloseHandle(s->h);
    free(s);
}

static const serial_ops_t k_win_ops = { win_read, win_write, win_close };

int serial_open(serial_port_t *port, const char *name, uint32_t baud)
{
    char path[64];
    win_serial_t *s;
    HANDLE h;
    DCB dcb;
    COMMTIMEOUTS to;

    if (port == NULL || name == NULL || name[0] == '\0' || baud == 0)
        return RS_ERR_INVALID_ARG;
    port->ops = NULL;
    port->st = NULL;

    /* COM10 以上は \\.\ 接頭辞が必要。全ポートに付けて統一する */
    if (strncmp(name, "\\\\.\\", 4) == 0)
        snprintf(path, sizeof(path), "%s", name);
    else
        snprintf(path, sizeof(path), "\\\\.\\%s", name);

    h = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return win_error(GetLastError());

    memset(&dcb, 0, sizeof(dcb));
    dcb.DCBlength = sizeof(dcb);
    if (!GetCommState(h, &dcb)) {
        int rc = win_error(GetLastError());
        CloseHandle(h);
        return rc;
    }
    dcb.BaudRate = baud;
    dcb.ByteSize = 8;
    dcb.Parity = NOPARITY;
    dcb.StopBits = ONESTOPBIT;
    dcb.fBinary = TRUE;
    dcb.fParity = FALSE;
    dcb.fOutxCtsFlow = FALSE;
    dcb.fOutxDsrFlow = FALSE;
    dcb.fDsrSensitivity = FALSE;
    dcb.fDtrControl = DTR_CONTROL_DISABLE;
    dcb.fRtsControl = RTS_CONTROL_DISABLE;
    dcb.fOutX = FALSE;
    dcb.fInX = FALSE;
    dcb.fNull = FALSE;
    dcb.fAbortOnError = FALSE;
    if (!SetCommState(h, &dcb)) {
        DWORD err = GetLastError();
        CloseHandle(h);
        g_last_os_error = (int)err;
        return err == ERROR_INVALID_PARAMETER ? RS_ERR_INVALID_ARG : RS_ERR_SYSTEM;
    }

    /* ReadIntervalTimeout = MAXDWORD かつ他が 0: 受信済みの分だけ即時に返す */
    memset(&to, 0, sizeof(to));
    to.ReadIntervalTimeout = MAXDWORD;
    to.WriteTotalTimeoutConstant = SERIAL_WRITE_TIMEOUT_MS;
    if (!SetCommTimeouts(h, &to)) {
        int rc = win_error(GetLastError());
        CloseHandle(h);
        return rc;
    }
    EscapeCommFunction(h, CLRDTR);
    EscapeCommFunction(h, CLRRTS);
    PurgeComm(h, PURGE_RXCLEAR | PURGE_TXCLEAR | PURGE_RXABORT | PURGE_TXABORT);

    s = malloc(sizeof(*s));
    if (s == NULL) {
        CloseHandle(h);
        return RS_ERR_NO_MEMORY;
    }
    s->h = h;
    port->ops = &k_win_ops;
    port->st = s;
    return RS_OK;
}

#else
/* ======================================================================== */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#include "rs_time.h"

typedef struct posix_serial {
    int fd;
} posix_serial_t;

static int errno_status(int err)
{
    g_last_os_error = err;
    switch (err) {
    case ENOENT:
    case ENODEV:
    case ENXIO:  return RS_ERR_NOT_FOUND;
    case EACCES:
    case EPERM:  return RS_ERR_PERMISSION;
    case EBUSY:  return RS_ERR_BUSY;
    default:     return RS_ERR_SYSTEM;
    }
}

static int baud_to_speed(uint32_t baud, speed_t *out)
{
    switch (baud) {
    case 1200:   *out = B1200;   return 1;
    case 2400:   *out = B2400;   return 1;
    case 4800:   *out = B4800;   return 1;
    case 9600:   *out = B9600;   return 1;
    case 19200:  *out = B19200;  return 1;
    case 38400:  *out = B38400;  return 1;
    case 57600:  *out = B57600;  return 1;
    case 115200: *out = B115200; return 1;
    default:     return 0;
    }
}

static int posix_read(void *st, void *buf, size_t cap, size_t *got)
{
    posix_serial_t *s = st;

    for (;;) {
        ssize_t n = read(s->fd, buf, cap);
        if (n >= 0) {
            *got = (size_t)n;
            return RS_OK;
        }
        if (errno == EINTR)
            continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            *got = 0;
            return RS_OK;
        }
        g_last_os_error = errno;
        return RS_ERR_IO;
    }
}

static int posix_write(void *st, const void *buf, size_t len)
{
    posix_serial_t *s = st;
    const uint8_t *p = buf;
    uint64_t deadline = rs_time_monotonic_ms() + SERIAL_WRITE_TIMEOUT_MS;

    while (len > 0) {
        ssize_t n = write(s->fd, p, len);
        if (n > 0) {
            p += n;
            len -= (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
            g_last_os_error = errno;
            return RS_ERR_IO;
        }
        {
            uint64_t now = rs_time_monotonic_ms();
            struct pollfd pfd;
            if (now >= deadline)
                return RS_ERR_TIMEOUT;
            pfd.fd = s->fd;
            pfd.events = POLLOUT;
            pfd.revents = 0;
            if (poll(&pfd, 1, (int)(deadline - now)) < 0 && errno != EINTR) {
                g_last_os_error = errno;
                return RS_ERR_IO;
            }
            if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
                g_last_os_error = EIO;
                return RS_ERR_IO;
            }
        }
    }
    return RS_OK;
}

static void posix_close(void *st)
{
    posix_serial_t *s = st;
    close(s->fd);
    free(s);
}

static const serial_ops_t k_posix_ops = { posix_read, posix_write, posix_close };

int serial_open(serial_port_t *port, const char *name, uint32_t baud)
{
    posix_serial_t *s;
    struct termios tio;
    speed_t speed;
    int fd;
    int bits = TIOCM_DTR | TIOCM_RTS;

    if (port == NULL || name == NULL || name[0] == '\0')
        return RS_ERR_INVALID_ARG;
    port->ops = NULL;
    port->st = NULL;
    if (!baud_to_speed(baud, &speed))
        return RS_ERR_INVALID_ARG;

    fd = open(name, O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0)
        return errno_status(errno);

    /* オープン時に OS が DTR/RTS をアサートするため、直ちにネゲートする */
    (void)ioctl(fd, TIOCMBIC, &bits);

    if (tcgetattr(fd, &tio) != 0) {
        int rc = errno_status(errno);
        close(fd);
        return rc;
    }
    cfmakeraw(&tio);
    tio.c_cflag |= CLOCAL | CREAD;
    tio.c_cflag &= ~(CRTSCTS | CSTOPB | PARENB | HUPCL);
    tio.c_iflag &= ~(IXON | IXOFF | IXANY);
    tio.c_cc[VMIN] = 0;
    tio.c_cc[VTIME] = 0;
    cfsetispeed(&tio, speed);
    cfsetospeed(&tio, speed);
    if (tcsetattr(fd, TCSANOW, &tio) != 0) {
        int rc = errno_status(errno);
        close(fd);
        return rc;
    }
    tcflush(fd, TCIOFLUSH);

    s = malloc(sizeof(*s));
    if (s == NULL) {
        close(fd);
        return RS_ERR_NO_MEMORY;
    }
    s->fd = fd;
    port->ops = &k_posix_ops;
    port->st = s;
    return RS_OK;
}
#endif
