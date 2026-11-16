#!/bin/sh
# Writes the C file that builds the given files into prism (cli/serve.c's
# embedded[]): embed.sh <out.c> <file>..., paths as the page asks for them.
set -e
out=$1; shift
{
    echo '/* made by scripts/embed.sh: do not edit */'
    echo '#include <stddef.h>'
    echo 'typedef struct { const char *path; const unsigned char *data; size_t len; } embedded_t;'
    i=0
    for f; do
        echo "static const unsigned char f$i[] = {"
        od -An -v -tx1 "$f" | sed 's/ \([0-9a-f][0-9a-f]\)/0x\1,/g'
        echo "};"
        i=$((i + 1))
    done
    echo 'const embedded_t embedded[] = {'
    i=0
    for f; do
        echo "    { \"$f\", f$i, sizeof f$i },"
        i=$((i + 1))
    done
    echo '    { 0, 0, 0 },'
    echo '};'
} > "$out.tmp"
mv "$out.tmp" "$out"
