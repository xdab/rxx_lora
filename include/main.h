#ifndef MAIN_H
#define MAIN_H

// true if s is a nonempty run of decimal digits
static int digits_only(const char *s)
{
    if (!*s)
        return 0;
    for (; *s; s++)
        if (*s < '0' || *s > '9')
            return 0;
    return 1;
}

#endif