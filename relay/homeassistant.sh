# Relay module: any switch entity in Home Assistant, REST API.
#   HA_URL=http://homeassistant.local:8123
#   HA_TOKEN=<long-lived access token>
#   HA_ENTITY=switch.psp_power
relay() {   # relay on|off|status
  local c=(curl -fsS --max-time 5 -H "Authorization: Bearer $HA_TOKEN" -H "Content-Type: application/json")
  case "$1" in
    status) "${c[@]}" "$HA_URL/api/states/$HA_ENTITY" |
              python3 -c 'import json,sys; print(json.load(sys.stdin)["state"])' ;;
    on|off) "${c[@]}" -d "{\"entity_id\": \"$HA_ENTITY\"}" \
              "$HA_URL/api/services/homeassistant/turn_$1" >/dev/null ;;
  esac
}
