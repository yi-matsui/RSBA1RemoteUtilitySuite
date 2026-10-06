/*
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, Yi Matsui
 */
#include "client_config.h"

#include <string.h>

#include "cfg_load.h"
#include "ctl_proto.h"

#define MODULE "config"

static const char *const k_root[] = {
    "server", "local", "auth", "session", "civ", "audio", "logging", NULL
};
static const char *const k_server[] = { "host", "control_port", "civ_port", "audio_port", NULL };
static const char *const k_local[] = { "bind_address", NULL };
static const char *const k_auth[] = { "username", "password", NULL };
static const char *const k_session[] = {
    "handshake_timeout_ms", "auto_reconnect", "reconnect_interval_ms", NULL
};
static const char *const k_civ[] = { "serial_device", "serial_baud", NULL };
static const char *const k_logging[] = {
    "level", "file", "max_file_bytes", "max_backups", "to_stderr", NULL
};

static const char *empty_to_null(const char *s)
{
    return (s != NULL && s[0] != '\0') ? s : NULL;
}

int client_config_apply(client_app_config_t *cfg, rs_log_config_t *log, const json_value_t *root,
                        const char *source, int *warnings)
{
    cfg_reader_t r;

    cfg_reader_init(&r, root, source);
    if (root == NULL || root->type != JSON_OBJECT) {
        cfg_error(&r, "(root)", root, "the top level must be an object");
        if (warnings != NULL)
            *warnings = r.warnings;
        return r.errors;
    }

    cfg_known_keys(&r, "", k_root);
    cfg_known_keys(&r, "server", k_server);
    cfg_known_keys(&r, "local", k_local);
    cfg_known_keys(&r, "auth", k_auth);
    cfg_known_keys(&r, "session", k_session);
    cfg_known_keys(&r, "civ", k_civ);
    cfg_known_keys(&r, "logging", k_logging);

    cfg->server_host = empty_to_null(cfg_string(&r, "server.host", cfg->server_host));
    cfg->ctl_port = (uint16_t)cfg_uint(&r, "server.control_port", cfg->ctl_port, 1, 65535);
    cfg->civ_port = (uint16_t)cfg_uint(&r, "server.civ_port", cfg->civ_port, 1, 65535);
    cfg->audio_port = (uint16_t)cfg_uint(&r, "server.audio_port", cfg->audio_port, 1, 65535);
    cfg->bind_addr = empty_to_null(cfg_string(&r, "local.bind_address", cfg->bind_addr));

    cfg->username = cfg_string(&r, "auth.username", cfg->username);
    cfg->password = cfg_string(&r, "auth.password", cfg->password);

    cfg->handshake_timeout_ms = cfg_uint(&r, "session.handshake_timeout_ms",
                                         cfg->handshake_timeout_ms, 500, 60000);
    cfg->auto_reconnect = cfg_bool(&r, "session.auto_reconnect", cfg->auto_reconnect);
    cfg->reconnect_interval_ms = cfg_uint(&r, "session.reconnect_interval_ms",
                                          cfg->reconnect_interval_ms, 100, 600000);

    cfg->civ_device = empty_to_null(cfg_string(&r, "civ.serial_device", cfg->civ_device));
    cfg->civ_baud = cfg_uint(&r, "civ.serial_baud", cfg->civ_baud, 1200, 115200);

    cfg_read_audio(&r, "audio", &cfg->audio_dev, &cfg->audio_link, &cfg->audio_enabled);
    cfg_read_logging(&r, "logging", log);

    if (warnings != NULL)
        *warnings = r.warnings;
    return r.errors;
}

int client_config_validate(const client_app_config_t *cfg)
{
    int errors = 0;
    size_t ulen = cfg->username != NULL ? strlen(cfg->username) : 0;

    if (cfg->server_host == NULL) {
        RS_LOG_ERROR(MODULE, "server.host (or --server) is required");
        errors++;
    }
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
    } else if (strlen(cfg->password) > 256) {
        RS_LOG_ERROR(MODULE, "password is longer than 256 characters");
        errors++;
    }
    if (cfg->ctl_port == cfg->civ_port || cfg->ctl_port == cfg->audio_port ||
        cfg->civ_port == cfg->audio_port) {
        RS_LOG_ERROR(MODULE, "control / CI-V / audio ports must differ (%u / %u / %u)",
                     (unsigned)cfg->ctl_port, (unsigned)cfg->civ_port, (unsigned)cfg->audio_port);
        errors++;
    }
    if (cfg->audio_link.jitter_min_ms > cfg->audio_link.jitter_max_ms) {
        RS_LOG_ERROR(MODULE, "jitter min (%u ms) exceeds max (%u ms)",
                     (unsigned)cfg->audio_link.jitter_min_ms, (unsigned)cfg->audio_link.jitter_max_ms);
        errors++;
    }
    return errors;
}
