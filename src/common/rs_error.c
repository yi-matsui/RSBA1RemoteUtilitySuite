/*
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, Yi Matsui
 */
#include "rs_error.h"

const char *rs_strerror(int status)
{
    switch (status) {
    case RS_OK:              return "success";
    case RS_ERR_INVALID_ARG: return "invalid argument";
    case RS_ERR_NO_MEMORY:   return "out of memory";
    case RS_ERR_SYSTEM:      return "system call failed";
    case RS_ERR_TIMEOUT:     return "timed out";
    case RS_ERR_TOO_LARGE:   return "data too large";
    case RS_ERR_TRUNCATED:   return "output truncated";
    case RS_ERR_ADDRESS:     return "address resolution failed";
    case RS_ERR_IO:          return "I/O error";
    case RS_ERR_NOT_FOUND:   return "not found";
    case RS_ERR_PERMISSION:  return "permission denied";
    case RS_ERR_BUSY:        return "busy or rate limited";
    case RS_ERR_UNSUPPORTED: return "operation not supported";
    default:                 return "unknown error";
    }
}
