#!/usr/bin/env bash
# Endurance test: autoboot, start an app through PSPLink, check it runs, repeat.
#
#   test/cycletest.sh <runs> <out dir>
#
# Env: APP=ms0:/PSP/GAME/ExtremeTuxRacer/ETR.PRX (PRX to ldstart),
#      APP_MOD="Extreme Tux Racer" (module that must be in modlist),
#      APP_WAIT=25 (seconds before the check), plus autoboot.sh's env.
# Output: <out dir>/results.csv (run, time, boot seconds, app ok, PSP alive).

set -uo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
RUNS=${1:?runs}; OUT=${2:?out dir}
APP=${APP:-ms0:/PSP/GAME/ExtremeTuxRacer/ETR.PRX}
APP_MOD=${APP_MOD:-Extreme Tux Racer}
APP_WAIT=${APP_WAIT:-25}
PSPSH=${PSPSH:-$(command -v pspsh || echo "$HOME/.local/opt/pspdev/bin/pspsh")}
psp() { timeout 20 "$PSPSH" -e "$*" 2>&1; }

mkdir -p "$OUT"
CSV=$OUT/results.csv
[ -f "$CSV" ] || echo "run,start,boot_s,app,alive" > "$CSV"

for run in $(seq "$RUNS"); do
  start=$(date +%T); boot_s="-"; app=no; alive=no
  if out=$("$ROOT/autoboot.sh"); then
    boot_s=$(echo "$out" | awk '{print $(NF-1)}')
    psp ldstart "$APP" > "$OUT/run$run-ldstart.txt"
    sleep "$APP_WAIT"
    if psp modlist > "$OUT/run$run-modlist.txt" && grep -qi psplink "$OUT/run$run-modlist.txt"; then
      alive=yes
      grep -q "$APP_MOD" "$OUT/run$run-modlist.txt" && app=yes
    fi
  fi
  echo "$run,$start,$boot_s,$app,$alive" | tee -a "$CSV"
done
