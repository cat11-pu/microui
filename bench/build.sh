#!/bin/sh
# Builds the baseline (HEAD microui.c) and optimized (src/microui.c) benches.
set -e
cd "$(dirname "$0")"
git show HEAD:src/microui.c > microui_base.c
CFLAGS="-O2 -std=gnu99 -Wall -Wextra -I../src -I../demo"
cc $CFLAGS -DTAG='"base"' bench.c microui_base.c -o bench_base
cc $CFLAGS -DTAG='"opt"'  bench.c ../src/microui.c -o bench_opt
echo "built bench_base bench_opt"
