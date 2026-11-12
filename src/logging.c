/** @file logging.c
 * @brief Leveled logging to stderr; see logging.h for the contract.
 */

#include "logging.h"

#include <stdarg.h>
#include <stdio.h>

// The threshold; only messages at or above it reach stderr.
static enum log_level threshold = LOG_INFO;

// Per-level line prefix, uniform "LEVEL: " so lines stay greppable.
static const char *level_prefix(enum log_level lvl)
{
    switch (lvl)
    {
    case LOG_TRACE:
        return "TRACE: ";
    case LOG_DEBUG:
        return "DEBUG: ";
    case LOG_INFO:
        return "INFO: ";
    case LOG_WARN:
        return "WARN: ";
    case LOG_ERROR:
        return "ERROR: ";
    default:
        return "";
    }
}

// The one filtered emitter; the five log_* wrappers differ only in level.
static void vlog(enum log_level lvl, const char *fmt, va_list ap)
{
    if (lvl < threshold)
        return;
    (void)fputs(level_prefix(lvl), stderr);
    (void)vfprintf(stderr, fmt, ap);
}

void log_set_level(enum log_level level) { threshold = level; }

// The five wrappers differ only in level (see logging.h for the contract).
#define LOG_FN(name, lvl)                                                                          \
    void name(const char *fmt, ...)                                                                \
    {                                                                                              \
        va_list ap;                                                                                \
        va_start(ap, fmt);                                                                         \
        vlog(lvl, fmt, ap);                                                                        \
        va_end(ap);                                                                                \
    }

LOG_FN(log_trace, LOG_TRACE)
LOG_FN(log_debug, LOG_DEBUG)
LOG_FN(log_info, LOG_INFO)
LOG_FN(log_warn, LOG_WARN)
LOG_FN(log_error, LOG_ERROR)
