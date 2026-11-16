#!/bin/sh
# Builds baseline + optimized benches, runs the three widget-count tiers,
# verifies pixel-identity frame by frame and prints the timing table.
set -e
cd "$(dirname "$0")"
./build.sh
for r in 40 150 400; do
  ./bench_base $r 320 > res_base_$r.txt
  ./bench_opt  $r 320 > res_opt_$r.txt
  if diff -q out_base_$r.hashes out_opt_$r.hashes > /dev/null \
     && cmp -s out_base_$r.rgba out_opt_$r.rgba; then
    echo "rows=$r: 340/340 frames pixel-identical"
  else
    echo "rows=$r: PIXEL MISMATCH" >&2
    exit 1
  fi
done
echo
for r in 40 150 400; do
  b=$(grep build_avg res_base_$r.txt | cut -d= -f2 | cut -d' ' -f1)
  o=$(grep build_avg res_opt_$r.txt  | cut -d= -f2 | cut -d' ' -f1)
  w=$(grep widgets res_opt_$r.txt | cut -d= -f4 | cut -d' ' -f1)
  echo "widgets=$w base=${b}ms opt=${o}ms"
done
