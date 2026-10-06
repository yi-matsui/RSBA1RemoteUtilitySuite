/*
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, Yi Matsui
 *
 * rs_log のテスト。カレントディレクトリにログファイルを作成する。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rs_error.h"
#include "rs_log.h"
#include "test_util.h"

#define LOG_PATH "test_rs_log.log"

static char *read_file(const char *path, long *size_out)
{
    FILE *fp = fopen(path, "rb");
    char *buf;
    long size;

    if (fp == NULL)
        return NULL;
    fseek(fp, 0, SEEK_END);
    size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    buf = malloc((size_t)size + 1);
    if (buf != NULL) {
        size = (long)fread(buf, 1, (size_t)size, fp);
        buf[size] = '\0';
    }
    fclose(fp);
    if (size_out != NULL)
        *size_out = size;
    return buf;
}

static int file_exists(const char *path)
{
    FILE *fp = fopen(path, "rb");
    if (fp == NULL)
        return 0;
    fclose(fp);
    return 1;
}

static void remove_all(void)
{
    remove(LOG_PATH);
    remove(LOG_PATH ".1");
    remove(LOG_PATH ".2");
    remove(LOG_PATH ".3");
}

static void test_levels_and_format(void)
{
    rs_log_config_t cfg;
    rs_log_t *log = NULL;
    char *text;

    remove_all();
    rs_log_config_default(&cfg);
    cfg.file_path = LOG_PATH;
    cfg.to_stderr = 0;
    cfg.max_file_bytes = 0;
    CHECK_RC(rs_log_open(&log, &cfg), RS_OK);

    rs_log_write(log, RS_LOG_LEVEL_DEBUG, "test", "hidden-%d", 1);
    rs_log_write(log, RS_LOG_LEVEL_INFO, "test", "visible-%d", 2);
    rs_log_set_level(log, RS_LOG_LEVEL_DEBUG);
    rs_log_write(log, RS_LOG_LEVEL_DEBUG, "test", "debug-%d", 3);

    /* 既定ロガー経由 */
    rs_log_set_default(log);
    RS_LOG_ERROR("dflt", "via-default");
    rs_log_close(log); /* 既定ロガーも自動解除される */
    CHECK(rs_log_get_default() == NULL);

    text = read_file(LOG_PATH, NULL);
    CHECK(text != NULL);
    if (text != NULL) {
        CHECK(strstr(text, "hidden-1") == NULL);
        CHECK(strstr(text, " INFO  [test] visible-2\n") != NULL);
        CHECK(strstr(text, " DEBUG [test] debug-3\n") != NULL);
        CHECK(strstr(text, " ERROR [dflt] via-default\n") != NULL);
        /* 先頭は UTC タイムスタンプ: YYYY-MM-DDThh:mm:ss.mmmZ */
        CHECK(strlen(text) > 24 && text[4] == '-' && text[10] == 'T' && text[23] == 'Z');
        free(text);
    }
}

static void test_truncation(void)
{
    rs_log_config_t cfg;
    rs_log_t *log = NULL;
    char *big = malloc(RS_LOG_LINE_MAX * 2);
    char *text;
    long size = 0;

    remove_all();
    rs_log_config_default(&cfg);
    cfg.file_path = LOG_PATH;
    cfg.to_stderr = 0;
    cfg.max_file_bytes = 0;
    CHECK_RC(rs_log_open(&log, &cfg), RS_OK);

    CHECK(big != NULL);
    if (big != NULL) {
        memset(big, 'x', RS_LOG_LINE_MAX * 2 - 1);
        big[RS_LOG_LINE_MAX * 2 - 1] = '\0';
        rs_log_write(log, RS_LOG_LEVEL_INFO, "test", "%s", big);
    }
    rs_log_close(log);

    text = read_file(LOG_PATH, &size);
    CHECK(text != NULL);
    if (text != NULL) {
        CHECK(size == RS_LOG_LINE_MAX - 1);
        CHECK(size >= 4 && memcmp(text + size - 4, "...\n", 4) == 0);
        free(text);
    }
    free(big);
}

static void test_rotation(void)
{
    rs_log_config_t cfg;
    rs_log_t *log = NULL;
    long size = 0;
    char *text;
    int i;

    remove_all();
    rs_log_config_default(&cfg);
    cfg.file_path = LOG_PATH;
    cfg.to_stderr = 0;
    cfg.max_file_bytes = 512;
    cfg.max_backups = 2;
    CHECK_RC(rs_log_open(&log, &cfg), RS_OK);

    for (i = 0; i < 60; i++)
        rs_log_write(log, RS_LOG_LEVEL_INFO, "rot", "line %03d", i);
    rs_log_close(log);

    CHECK(file_exists(LOG_PATH));
    CHECK(file_exists(LOG_PATH ".1"));
    CHECK(file_exists(LOG_PATH ".2"));
    CHECK(!file_exists(LOG_PATH ".3"));

    text = read_file(LOG_PATH, &size);
    CHECK(text != NULL);
    if (text != NULL) {
        CHECK(size > 0 && size <= 512);
        CHECK(strstr(text, "line 059") != NULL);
        free(text);
    }
}

static void test_level_parse(void)
{
    rs_log_level_t lv = RS_LOG_LEVEL_INFO;

    CHECK_RC(rs_log_level_from_string("warn", &lv), RS_OK);
    CHECK(lv == RS_LOG_LEVEL_WARN);
    CHECK_RC(rs_log_level_from_string("TRACE", &lv), RS_OK);
    CHECK(lv == RS_LOG_LEVEL_TRACE);
    CHECK_RC(rs_log_level_from_string("Off", &lv), RS_OK);
    CHECK(lv == RS_LOG_LEVEL_OFF);
    CHECK_RC(rs_log_level_from_string("bogus", &lv), RS_ERR_INVALID_ARG);
    CHECK_RC(rs_log_level_from_string("inf", &lv), RS_ERR_INVALID_ARG);
    CHECK(strcmp(rs_log_level_name(RS_LOG_LEVEL_ERROR), "ERROR") == 0);
}

int main(void)
{
    rs_log_t *log = NULL;

    test_levels_and_format();
    test_truncation();
    test_rotation();
    test_level_parse();

    CHECK_RC(rs_log_open(&log, NULL), RS_ERR_INVALID_ARG);
    remove_all();
    return TEST_RESULT();
}
