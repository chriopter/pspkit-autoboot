# Relay module: Shelly Gen2+ plug (Plus Plug S etc.), RPC API.
#   SHELLY_URL=http://192.168.1.50
#   SHELLY_PASS=...   only if auth is enabled on the device (user "admin")
relay() {   # relay on|off|status
  local c=(curl -fsS --max-time 5)
  [ -n "${SHELLY_PASS:-}" ] && c+=(--digest -u "admin:$SHELLY_PASS")
  case "$1" in
    status) "${c[@]}" "$SHELLY_URL/rpc/Switch.GetStatus?id=0" |
              python3 -c 'import json,sys; print("on" if json.load(sys.stdin)["output"] else "off")' ;;
    on)     "${c[@]}" "$SHELLY_URL/rpc/Switch.Set?id=0&on=true" >/dev/null ;;
    off)    "${c[@]}" "$SHELLY_URL/rpc/Switch.Set?id=0&on=false" >/dev/null ;;
  esac
}
