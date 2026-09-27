# Relay module: ESPHome web_server, switch named "Relais".
#   ESPHOME_URL=http://psp-relais.local
#   ESPHOME_USER=...  ESPHOME_PASS=...
relay() {   # relay on|off|status
  local c=(curl -fsS --max-time 5 --anyauth -u "$ESPHOME_USER:$ESPHOME_PASS")
  case "$1" in
    status) "${c[@]}" "$ESPHOME_URL/switch/Relais" |
              python3 -c 'import json,sys; print(json.load(sys.stdin)["state"].lower())' ;;
    on|off) "${c[@]}" -X POST -d '' "$ESPHOME_URL/switch/Relais/turn_$1" >/dev/null ;;
  esac
}
