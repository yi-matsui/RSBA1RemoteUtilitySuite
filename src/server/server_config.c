/*
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, Yi Matsui
 */
#include "server_config.h"

#include <string.h>

#include "cfg_load.h"
#include "ctl_proto.h"
#include "rs_error.h"
#include "usb_device.h"

#define MODULE "config"

static const char *const k_root[] = {
    "listen", "auth", "session", "radio", "audio", "usb_reset", "logging", NULL
};
static const char *const k_listen[] = {
    "address", "control_port", "civ_port", "audio_port", NULL
};
static const char *const k_auth[] = {
    "username", "password", "pbkdf2_iterations", "max_failures", "failure_window_seconds",
    "ban_seconds", NULL
};
static const char *const k_session[] = {
    "keepalive_interval_ms", "keepalive_timeout_ms", "challenge_timeout_ms", NULL
};
static const char *const k_radio[] = {
    "model", "civ_address", "controller_address", "serial_device", "serial_baud",
    "audio_capture_device", "audio_playback_device", NULL
};
static const char *const k_usb[] = {
    "enabled", "vid_pid", "serial", "device_id", "reset_parent", "method",
    "serial_timeout_threshold", "rx_garbage_threshold", "recovery_timeout_ms", "poll_interval_ms",
    "min_interval_seconds", NULL
};
static const char *const k_logging[] = {
    "level", "file", "max_file_bytes", "max_backups", "to_stderr", "usb_reset_file", NULL
};

static const char *empty_to_null(const char *s)
{
    return (s != NULL && s[0] != '\0') ? s : NULL;
}

int server_config_apply(server_app_config_t *cfg, rs_log_config_t *log, const json_value_t *root,
                        const char *source, int *warnings)
{
    cfg_reader_t r;
    const char *s;

    cfg_reader_init(&r, root, source);
    if (root == NULL || root->type != JSON_OBJECT) {
        cfg_error(&r, "(root)", root, "the top level must be an object");
        if (warnings != NULL)
            *warnings = r.warnings;
        return r.errors;
    }

    cfg_known_keys(&r, "", k_root);
    cfg_known_keys(&r, "listen", k_listen);
    cfg_known_keys(&r, "auth", k_auth);
    cfg_known_keys(&r, "session", k_session);
    cfg_known_keys(&r, "radio", k_radio);
    cfg_known_keys(&r, "usb_reset", k_usb);
    cfg_known_keys(&r, "logging", k_logging);

    /* listen */
    cfg->listen_addr = empty_to_null(cfg_string(&r, "listen.address", cfg->listen_addr));
    cfg->ctl_port = (uint16_t)cfg_uint(&r, "listen.control_port", cfg->ctl_port, 1, 65535);
    cfg->civ_port = (uint16_t)cfg_uint(&r, "listen.civ_port", cfg->civ_port, 1, 65535);
    cfg->audio_port = (uint16_t)cfg_uint(&r, "listen.audio_port", cfg->audio_port, 1, 65535);

    /* auth */
    cfg->username = cfg_string(&r, "auth.username", cfg->username);
    cfg->password = cfg_string(&r, "auth.password", cfg->password);
    cfg->pbkdf2_iterations = cfg_uint(&r, "auth.pbkdf2_iterations", cfg->pbkdf2_iterations, 1, 10000000);
    cfg->max_failures = (int)cfg_uint(&r, "auth.max_failures", (uint32_t)cfg->max_failures, 0, 1000);
    cfg->failure_window_ms = cfg_uint(&r, "auth.failure_window_seconds",
                                      cfg->failure_window_ms / 1000u, 1, 86400) * 1000u;
    cfg->ban_ms = cfg_uint(&r, "auth.ban_seconds", cfg->ban_ms / 1000u, 1, 604800) * 1000u;

    /* session */
    cfg->keepalive_interval_ms = cfg_uint(&r, "session.keepalive_interval_ms",
                                          cfg->keepalive_interval_ms, 100, 60000);
    cfg->keepalive_timeout_ms = cfg_uint(&r, "session.keepalive_timeout_ms",
                                         cfg->keepalive_timeout_ms, 500, 600000);
    cfg->challenge_timeout_ms = cfg_uint(&r, "session.challenge_timeout_ms",
                                         cfg->challenge_timeout_ms, 500, 60000);

    /* radio */
    s = cfg_string(&r, "radio.model", NULL);
    if (s != NULL && strcmp(s, "IC-9100") != 0)
        cfg_warn(&r, "radio.model", json_get_path(root, "radio.model"),
                 "\"%s\": only IC-9100 has been considered", s);
    cfg->radio_addr = cfg_byte(&r, "radio.civ_address", cfg->radio_addr);
    cfg->ctrl_addr = cfg_byte(&r, "radio.controller_address", cfg->ctrl_addr);
    cfg->civ_device = empty_to_null(cfg_string(&r, "radio.serial_device", cfg->civ_device));
    cfg->civ_baud = cfg_uint(&r, "radio.serial_baud", cfg->civ_baud, 1200, 115200);
    cfg->audio_dev.capture = cfg_device_name(
        cfg_string(&r, "radio.audio_capture_device", cfg->audio_dev.capture));
    cfg->audio_dev.playback = cfg_device_name(
        cfg_string(&r, "radio.audio_playback_device", cfg->audio_dev.playback));

    /* audio（形式・ジッター・backend） */
    cfg_read_audio(&r, "audio", &cfg->audio_dev, &cfg->audio_link, &cfg->audio_enabled);

    /* usb_reset */
    cfg->usb_watch = cfg_bool(&r, "usb_reset.enabled", cfg->usb_watch);
    s = cfg_string(&r, "usb_reset.vid_pid", NULL);
    if (s != NULL && s[0] != '\0' &&
        usb_parse_vid_pid(s, &cfg->usb.match.vid, &cfg->usb.match.pid) != RS_OK)
        cfg_error(&r, "usb_reset.vid_pid", json_get_path(root, "usb_reset.vid_pid"),
                  "\"%s\" is not VID:PID (e.g. \"10C4:EA60\")", s);
    cfg->usb.match.serial = empty_to_null(cfg_string(&r, "usb_reset.serial", cfg->usb.match.serial));
    cfg->usb.match.device_id = empty_to_null(cfg_string(&r, "usb_reset.device_id",
                                                        cfg->usb.match.device_id));
    cfg->usb.reset_parent = cfg_bool(&r, "usb_reset.reset_parent", cfg->usb.reset_parent);
    s = cfg_string(&r, "usb_reset.method", NULL);
    if (s != NULL && usb_reset_method_from_string(s, &cfg->usb.method) != RS_OK)
        cfg_error(&r, "usb_reset.method", json_get_path(root, "usb_reset.method"),
                  "\"%s\" is not one of auto / usbdevfs / authorized / devnode", s);
    cfg->usb.serial_timeout_threshold = (int)cfg_uint(&r, "usb_reset.serial_timeout_threshold",
                                                      (uint32_t)cfg->usb.serial_timeout_threshold, 0, 1000);
    cfg->usb.rx_garbage_threshold = cfg_uint(&r, "usb_reset.rx_garbage_threshold",
                                             (uint32_t)cfg->usb.rx_garbage_threshold, 0, 1048576);
    cfg->usb.recovery_timeout_ms = cfg_uint(&r, "usb_reset.recovery_timeout_ms",
                                            cfg->usb.recovery_timeout_ms, 1000, 120000);
    cfg->usb.poll_interval_ms = cfg_uint(&r, "usb_reset.poll_interval_ms",
                                         cfg->usb.poll_interval_ms, 10, 5000);
    cfg->usb.min_interval_ms = cfg_uint(&r, "usb_reset.min_interval_seconds",
                                        cfg->usb.min_interval_ms / 1000u, 0, 86400) * 1000u;

    /* logging */
    cfg_read_logging(&r, "logging", log);
    cfg->usb_log_path = empty_to_null(cfg_string(&r, "logging.usb_reset_file", cfg->usb_log_path));
    if (cfg->usb_log_path == NULL)
        cfg->usb_log_path = "usb_reset.log";

    if (warnings != NULL)
        *warnings = r.warnings;
    return r.errors;
}

int server_config_validate(const server_app_config_t *cfg)
{
    int errors = 0;
    size_t ulen = cfg->username != NULL ? strlen(cfg->username) : 0;

    if (ulen == 0 || ulen > CTL_USERNAME_MAX) {
        RS_LOG_ERROR(MODULE, "auth.username (or --user) is required (1..%d characters)",
                     CTL_USERNAME_MAX);
        errors++;
    }
    if (cfg->password == NULL || cfg->password[0] == '\0') {
        RS_LOG_ERROR(MODULE, "password is required: set auth.password or environment variable "
                     "RSBA_PASSWORD");
        errors++;
    } else if (strcmp(cfg->password, "CHANGE_ME") == 0) {
        RS_LOG_ERROR(MODULE, "auth.password is still the example value CHANGE_ME");
        errors++;
    } else if (strlen(cfg->password) < 8) {
        RS_LOG_WARN(MODULE, "password is shorter than 8 characters");
    }
    if (cfg->ctl_port == cfg->civ_port || cfg->ctl_port == cfg->audio_port ||
        cfg->civ_port == cfg->audio_port) {
        RS_LOG_ERROR(MODULE, "control / CI-V / audio ports must differ (%u / %u / %u)",
                     (unsigned)cfg->ctl_port, (unsigned)cfg->civ_port, (unsigned)cfg->audio_port);
        errors++;
    }
    if (cfg->keepalive_timeout_ms < cfg->keepalive_interval_ms * 2) {
        RS_LOG_ERROR(MODULE, "session.keepalive_timeout_ms (%u) must be at least twice "
                     "keepalive_interval_ms (%u)", (unsigned)cfg->keepalive_timeout_ms,
                     (unsigned)cfg->keepalive_interval_ms);
        errors++;
    }
    if (cfg->audio_link.jitter_min_ms > cfg->audio_link.jitter_max_ms) {
        RS_LOG_ERROR(MODULE, "jitter min (%u ms) exceeds max (%u ms)",
                     (unsigned)cfg->audio_link.jitter_min_ms, (unsigned)cfg->audio_link.jitter_max_ms);
        errors++;
    }
    if (cfg->usb_watch && cfg->usb.match.vid == 0 && cfg->usb.match.pid == 0 &&
        cfg->usb.match.serial == NULL && cfg->usb.match.device_id == NULL) {
        RS_LOG_ERROR(MODULE, "usb_reset.enabled requires usb_reset.vid_pid (or --usb-watch VID:PID)");
        errors++;
    }
    if (cfg->pbkdf2_iterations < 10000)
        RS_LOG_WARN(MODULE, "pbkdf2_iterations %u is low; 100000 or more is recommended",
                    (unsigned)cfg->pbkdf2_iterations);
    if (cfg->civ_device == NULL)
        RS_LOG_WARN(MODULE, "no CI-V serial device configured: CI-V relay and PTT release "
                    "fail-safe are disabled");
    return errors;
}
