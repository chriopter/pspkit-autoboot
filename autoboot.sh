#!/usr/bin/env bash
# Power-cycle the PSP into PSPLink through the relay.
#
#   ./autoboot.sh            clean power-off, relay off, relay on, wait for PSPLink
#   ./autoboot.sh off        clean power-off, relay off
#   ./autoboot.sh relay on|off|status
#
# Prints "PSPLink up after <s> s" and exits 0, or exits 1 after BOOT_MAX.
# Clean power-off = PSPLink "poweroff", so the Memory Stick is idle before
# the relay cuts power.
#
# Config: ~/.config/pspkit-autoboot.env (or $PSPKIT_ENV), one relay backend:
#   RELAY=homeassistant
#   HA_URL=http://homeassistant.local:8123
#   HA_TOKEN=<long-lived access token>
#   HA_ENTITY=switch.psp_relais
# or
#   RELAY=esphome             (ESPHome web_server, switch named "Relais")
#   ESPHOME_URL=http://psp-relais.local
#   ESPHOME_USER=... ESPHOME_PASS=...
#
# Env: OFF_S=15 (relay off, capacitors discharge), BOOT_MAX=90,
#      PSPSH (default: pspsh in PATH). PSPLink's usbhostfs_pc must run.

set -euo pipefail
export LC_ALL=C

# shellcheck source=/dev/null
. "${PSPKIT_ENV:-$HOME/.config/pspkit-autoboot.env}"
OFF_S=${OFF_S:-15}
BOOT_MAX=${BOOT_MAX:-90}
PSPSH=${PSPSH:-$(command -v pspsh || echo "$HOME/.local/opt/pspdev/bin/pspsh")}
USB_ID=054c:01c9   # PSPLink

relay() {   # relay on|off|status
  case "$RELAY" in
    homeassistant)
      local auth=(-H "Authorization: Bearer $HA_TOKEN" -H "Content-Type: application/json")
      if [ "$1" = status ]; then
        curl -fsS --max-time 5 "${auth[@]}" "$HA_URL/api/states/$HA_ENTITY" |
          python3 -c 'import json,sys; print(json.load(sys.stdin)["state"])'
      else
        curl -fsS --max-time 5 "${auth[@]}" -d "{\"entity_id\": \"$HA_ENTITY\"}" \
          "$HA_URL/api/services/homeassistant/turn_$1" >/dev/null
      fi ;;
    esphome)
      local c=(curl -fsS --max-time 5 --anyauth -u "$ESPHOME_USER:$ESPHOME_PASS")
      if [ "$1" = status ]; then
        "${c[@]}" "$ESPHOME_URL/switch/Relais" |
          python3 -c 'import json,sys; print(json.load(sys.stdin)["state"].lower())'
      else
        "${c[@]}" -X POST -d '' "$ESPHOME_URL/switch/Relais/turn_$1" >/dev/null
      fi ;;
    *) echo "RELAY must be homeassistant or esphome" >&2; exit 2 ;;
  esac
}

usb_up()   { lsusb -d "$USB_ID" >/dev/null 2>&1; }
shell_up() { timeout 10 "$PSPSH" -e ver 2>/dev/null | grep -q PSPLink; }

power_off() {
  if shell_up; then
    timeout 20 "$PSPSH" -e poweroff >/dev/null 2>&1 || true
    for _ in $(seq 100); do usb_up || break; sleep 0.2; done
    sleep 2
  fi
  relay off
}

case "${1:-boot}" in
  boot)
    power_off
    sleep "$OFF_S"
    relay on
    t0=$EPOCHREALTIME
    while awk -v a="$t0" -v b="$EPOCHREALTIME" -v m="$BOOT_MAX" 'BEGIN { exit !(b - a < m) }'; do
      if usb_up && shell_up; then
        awk -v a="$t0" -v b="$EPOCHREALTIME" 'BEGIN { printf "PSPLink up after %.1f s\n", b - a }'
        exit 0
      fi
      sleep 0.5
    done
    echo "no PSPLink after ${BOOT_MAX}s" >&2; exit 1 ;;
  off)   power_off ;;
  relay) relay "${2:?on|off|status}"; [ "$2" = status ] || relay status ;;
  *)     sed -n '2,24p' "$0" | sed 's/^# \{0,1\}//'; exit 1 ;;
esac
