#!/bin/bash
# Runs tools/test_conceal.c for tone / sweep / speech and stalls of 5-200 ms with a test binary, then tools/check_conceal.py on each.
#   [CONCEAL=0|1] [BUFMS=n (fixed buffer, default automatic)] tools/run_conceal.sh BINARY OUTDIR [stalls...]
#   (memory-capped, 4 in parallel; each run is 8 s real time)
R=$(cd "$(dirname "$0")/.." && pwd)
BIN=$1; O=$2; shift 2; mkdir -p "$O"
STALLS=${*:-5 10 20 40 80 120 200}
run() { SDL_AUDIODRIVER=disk SDL_DISKAUDIOFILE="$O/$1_$2.raw" SDL_DISKAUDIODELAY=12 systemd-run --user --scope -q -p MemoryMax=2G -p MemorySwapMax=0 timeout -s KILL 60 "$BIN" $1 $2 8 ${CONCEAL:-1} ${BUFMS:-0} 2>/dev/null; }
export -f run; export BIN O CONCEAL BUFMS
for s in tone sweep speech noise; do for d in 0 $STALLS; do echo "$s $d"; done; done | xargs -P 4 -L 1 bash -c 'run $0 $1'
for s in tone sweep speech noise; do for d in $STALLS; do printf '%-6s stall %3d ms  ' $s $d; python3 "$R/tools/check_conceal.py" $s "$O/${s}_$d.raw" "$O/${s}_0.raw" || rc=1; done; done
exit ${rc:-0}
