/**
 * @file logging.h
 * @brief Leveled logging to stderr; stdout carries the structured per-frame
 *        records (output.h).
 */

#ifndef LOGGING_H
#define LOGGING_H

enum log_level
{
    LOG_TRACE = 0, // per-window/per-symbol internals, can spam
    LOG_DEBUG = 1, // stage handovers and decisions
    LOG_INFO = 2,  // routine progress, the default
    LOG_WARN = 3,  // handled, execution continues
    LOG_ERROR = 4, // infrastructure or internal failure
    LOG_OFF = 5,   // silences everything
};

void log_set_level(enum log_level level);

void log_trace(const char *fmt, ...);
void log_debug(const char *fmt, ...);
void log_info(const char *fmt, ...);
void log_warn(const char *fmt, ...);
void log_error(const char *fmt, ...);

#endif
