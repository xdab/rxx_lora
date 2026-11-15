#!/bin/sh
# probe_lib.sh HEADER LIB... - print "yes" when HEADER compiles and links
# against LIB..., nothing otherwise. Used by the Makefile to decide whether
# the SDR backends can be built; not part of normal compilation.
set -e

header=$1
shift

printf '#include <%s>\nint main(void){return 0;}\n' "$header" |
    ${CC:-cc} -x c - -o /dev/null "$@" 2>/dev/null && echo yes
