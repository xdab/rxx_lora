#ifndef MAIN_H
#define MAIN_H

#include <getopt.h>
#include <stdlib.h>

#include "logging.h"

// true if s is a nonempty run of decimal digits
static inline int digits_only(const char *s)
{
    if (!*s)
        return 0;
    for (; *s; s++)
        if (*s < '0' || *s > '9')
            return 0;
    return 1;
}

// -v/-v1/-v 1 and -q/-q1/-q 1: the shared tail of both binaries' option
// loops, with the optional level argument taken from the argv tail
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters) // (opt, argc, argv) is getopt's shape
static inline void log_verbosity_opt(int opt, int argc, char **argv)
{
    int n = 1;
    if (optarg)
        n = (int)strtol(optarg, NULL, 10);
    else if (optind < argc && digits_only(argv[optind]))
        n = (int)strtol(argv[optind++], NULL, 10);
    if (n < 1)
        n = 1;
    log_set_level(opt == 'v' ? LOG_INFO - (n > 2 ? 2 : n) : LOG_INFO + (n > 3 ? 3 : n));
}

#endif
