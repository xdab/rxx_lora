/**
 * @file logging.h
 * @brief Leveled logging to stderr; stdout carries the structured per-frame
 *        records (output.h). Every call prints nothing below the threshold.
 */

#ifndef LOGGING_H
#define LOGGING_H

#include <stddef.h>
#include <stdint.h>

// Severity ladder; a message prints when its level >= the threshold.
enum log_level
{
    LOG_TRACE = 0, // per-window/per-symbol internals, can spam
    LOG_DEBUG = 1, // stage handovers and decisions
    LOG_INFO = 2,  // routine progress, the default
    LOG_WARN = 3,  // handled, execution continues
    LOG_ERROR = 4, // infrastructure or internal failure
    LOG_OFF = 5,   // silences everything
};

/** Set the threshold; only messages at or above it are printed. */
void log_set_level(enum log_level level);

void log_trace(const char *fmt, ...);
void log_debug(const char *fmt, ...);
void log_info(const char *fmt, ...);
void log_warn(const char *fmt, ...);
void log_error(const char *fmt, ...);

#endif
