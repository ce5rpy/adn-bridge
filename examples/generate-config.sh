#!/usr/bin/env bash
# Interactive INI (+ optional systemd unit) generator for adn-bridge.
# Copyright (C) 2026  Rodrigo Pérez, CE5RPY <ce5rpy@qmd.cl>
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Usage (from repo root or anywhere):
#   ./examples/generate-config.sh
#   ./examples/generate-config.sh --lang es

set -euo pipefail

LANG_UI=en
LANG_FROM_CLI=0
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
# Self-contained install layout (make install PREFIX=/opt/adn-bridge).
WORKDIR_DEFAULT="/opt/adn-bridge"
CONFDIR_DEFAULT="${WORKDIR_DEFAULT}/config"
BIN_DEFAULT="${WORKDIR_DEFAULT}/adn-bridge"

ADN_SUBSCRIBER_URL="https://servers.adn.systems/subscriber_ids.json"
ADN_CHECKSUM_URL="https://servers.adn.systems/file_checksums.json"
EL_DIR_DEFAULT="server1.echolink.org, server2.echolink.org, server3.echolink.org, server4.echolink.org"

usage() {
  cat <<'EOF'
Usage: generate-config.sh [--lang en|es] [-h|--help]

Interactive prompts for ysf-dmr / echolink-dmr / echolink-ysf ([peer.*] bus).
EchoLink modes: vocoder_host / vocoder_port on [peer.el] (required).
Empty answer → suggested default. Writes a local *.ini (gitignored)
and optionally a systemd unit under the output directory.
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --lang)
      LANG_UI="${2:-en}"
      LANG_FROM_CLI=1
      shift 2
      ;;
    -h|--help) usage; exit 0 ;;
    *) echo "Unknown option: $1" >&2; usage; exit 1 ;;
  esac
done

msg() {
  if [[ "$LANG_UI" == "es" ]]; then
    printf '%s' "$2"
  else
    printf '%s' "$1"
  fi
}

ask() {
  # ask VAR "EN prompt" "ES prompt" "default"
  # Defaults may contain ';' (RPTO OPTIONS) — keep locals as separate
  # assignments so a broken line-continuation never executes the default.
  local _var _en _es _def _ans _prompt
  _var="$1"
  _en="$2"
  _es="$3"
  _def="${4-}"
  if [[ -n "${_def}" ]]; then
    _prompt="$(msg "${_en}" "${_es}") [${_def}]: "
    read -r -p "${_prompt}" _ans || true
    printf -v "${_var}" '%s' "${_ans:-${_def}}"
  else
    _prompt="$(msg "${_en}" "${_es}"): "
    read -r -p "${_prompt}" _ans || true
    printf -v "${_var}" '%s' "${_ans}"
  fi
}

ask_secret() {
  local _var _en _es _def _ans _prompt
  _var="$1"
  _en="$2"
  _es="$3"
  _def="${4-}"
  if [[ -n "${_def}" ]]; then
    _prompt="$(msg "${_en}" "${_es}") [${_def}]: "
    read -r -s -p "${_prompt}" _ans || true
    echo
    printf -v "${_var}" '%s' "${_ans:-${_def}}"
  else
    _prompt="$(msg "${_en}" "${_es}"): "
    read -r -s -p "${_prompt}" _ans || true
    echo
    printf -v "${_var}" '%s' "${_ans}"
  fi
}

ask_yn() {
  # ask_yn VAR "EN" "ES" default_y_or_n → sets VAR to 1 or 0
  local _var _en _es _def _ans _dlabel _prompt
  _var="$1"
  _en="$2"
  _es="$3"
  _def="$4"
  if [[ "$LANG_UI" == "es" ]]; then
    if [[ "${_def}" == "y" ]]; then _dlabel="S/n"; else _dlabel="s/N"; fi
  else
    if [[ "${_def}" == "y" ]]; then _dlabel="Y/n"; else _dlabel="y/N"; fi
  fi
  _prompt="$(msg "${_en}" "${_es}") [${_dlabel}]: "
  read -r -p "${_prompt}" _ans || true
  _ans=$(printf '%s' "${_ans:-${_def}}" | tr '[:upper:]' '[:lower:]')
  case "${_ans}" in
    y|yes|s|si|sí|1) printf -v "${_var}" '%s' 1 ;;
    *) printf -v "${_var}" '%s' 0 ;;
  esac
}

if [[ "$LANG_FROM_CLI" -eq 0 && -t 0 ]]; then
  _pick=""
  read -r -p "$(msg 'Language / Idioma [en/es]' 'Idioma / Language [es/en]') [en]: " _pick || true
  case "$(printf '%s' "${_pick:-en}" | tr '[:upper:]' '[:lower:]')" in
    es|spa*|español) LANG_UI=es ;;
    *) LANG_UI=en ;;
  esac
fi

echo
echo "$(msg 'adn-bridge — interactive config generator' \
            'adn-bridge — generador interactivo de configuración')"
echo

echo "$(msg 'Bridge mode:' 'Modo del puente:')"
echo "  1) ysf-dmr        $(msg 'YSF reflector ↔ DMR' 'Reflector YSF ↔ DMR')"
echo "  2) echolink-dmr   EchoLink ↔ DMR"
echo "  3) echolink-ysf   EchoLink ↔ YSF"
ask MODE_NUM "Choose 1/2/3" "Elija 1/2/3" "1"
case "$MODE_NUM" in
  2) MODE=echolink-dmr ;;
  3) MODE=echolink-ysf ;;
  *) MODE=ysf-dmr ;;
esac

ask INSTANCE "Instance name for INI / systemd" "Nombre de instancia INI / systemd" "$MODE"
INSTANCE=$(printf '%s' "$INSTANCE" | tr -c 'A-Za-z0-9._-' '-' | sed 's/^-*//;s/-*$//')
[[ -n "$INSTANCE" ]] || INSTANCE="$MODE"

ask OUT_DIR "Config output directory" "Directorio de configuración" "$CONFDIR_DEFAULT"
mkdir -p "$OUT_DIR"
OUT_DIR="$(cd "$OUT_DIR" && pwd)"
INI_PATH="${OUT_DIR}/adn-bridge-${INSTANCE}.ini"

LOG_LEVEL=INFO

NEED_DMR_PEER=0
NEED_YSF=0
NEED_EL=0
case "$MODE" in
  ysf-dmr)       NEED_DMR_PEER=1; NEED_YSF=1 ;;
  echolink-dmr)  NEED_DMR_PEER=1; NEED_EL=1 ;;
  echolink-ysf)  NEED_YSF=1; NEED_EL=1 ;;
esac

DMR_CALL=""; DMR_ID=""; DMR_DESC=""; DMR_LOC="ADN bridge"
DMR_HOST=""; DMR_PORT="62031"; DMR_TG="9"; DMR_PASS=""; DMR_OPTS=""
CLEAR_DYN=0
case "$MODE" in
  ysf-dmr)      DMR_DESC="DMR <> YSF" ;;
  echolink-dmr) DMR_DESC="DMR <> EchoLink" ;;
  *)            DMR_DESC="adn-bridge" ;;
esac
if [[ "$NEED_DMR_PEER" -eq 1 ]]; then
  echo
  echo "=== [peer.*] DMR ==="
  ask DMR_CALL "DMR callsign" "Indicativo DMR" "N0CALL"
  ask DMR_ID "DMR ID" "ID DMR" "1234567"
  ask DMR_DESC "Description (monitor)" "Descripción (monitor)" "$DMR_DESC"
  ask DMR_LOC "Location shown on dashboard" "Ubicación se ve en el dashboard" "ADN bridge"
  ask DMR_HOST "DMR master host" "Host del master DMR" "7301.adn.systems"
  ask DMR_PORT "DMR master port" "Puerto del master DMR" "62031"
  ask DMR_TG "Talkgroup tg" "Talkgroup tg" "9"
  ask_secret DMR_PASS "DMR password" "Contraseña DMR" "passw0rd"
  ask_yn DMR_XLX "DMR master is XLX - no OPTIONS/RPTO?" "El master DMR es XLX - sin OPTIONS/RPTO?" "n"
  DMR_OPTS=""
  if [[ "$DMR_XLX" -eq 0 ]]; then
    # Pre-assign: a lone "TS2=…;SINGLE=0;" line is executed by bash if a
    # previous '\' continuation breaks (trailing spaces / CRLF).
    DMR_OPTS="TS2=${DMR_TG};SINGLE=0;"
    ask DMR_OPTS "RPTO options" "OPTIONS RPTO" "${DMR_OPTS}"
  fi
  ask_yn CLEAR_DYN "clear_dynamic_tg - PTT TG 4000 before connect?" "clear_dynamic_tg - PTT a TG 4000 antes de conectar?" "y"
fi

YSF_HOST=""; YSF_PORT="42000"; YSF_DGID="1"; YSF_CALL=""
if [[ "$NEED_YSF" -eq 1 ]]; then
  echo
  echo "=== [peer.*] YSF ==="
  ask YSF_HOST "YSF reflector host" "Host del reflector YSF" "reflector.example.net"
  ask YSF_PORT "YSF port" "Puerto YSF" "42000"
  ask YSF_DGID "DGID" "DGID" "1"
fi

EL_CALL=""; EL_PASS=""; EL_BIND=""; EL_HOST=""; EL_QTH=""; EL_EMAIL=""
EL_DIR="$EL_DIR_DEFAULT"; EL_LOGIN="360"; EL_LIST="600"; EL_GAIN="1.0"
USE_PROXY=0; EL_PROXY=""; EL_PROXY_PORT="8100"; EL_PROXY_PASS="PUBLIC"
VOC_HOST="127.0.0.1"; VOC_PORT="2460"
EL_MAX_INBOUND="1"; EL_ALLOWED=""; EL_BLOCKED=""; EL_WELCOME=""
if [[ "$NEED_EL" -eq 1 ]]; then
  echo
  echo "=== [peer.*] EchoLink ==="
  # Single-line asks: prompts contain () / *CONF* — broken '\' would make bash
  # treat those strings as commands ("syntax error near unexpected token '('").
  ask EL_CALL "EchoLink callsign (-L/-R/CONF)" "Indicativo EchoLink (-L/-R/CONF)" "N0CALL-L"
  ask_secret EL_PASS "EchoLink password" "Contraseña EchoLink" "change-me"
  EL_HOST_DEFAULT='*EXAMPLE*'
  ask EL_HOST "Peer to link (node or CONF)" "Peer a enlazar (nodo o CONF)" "${EL_HOST_DEFAULT}"
  ask EL_QTH "QTH (optional)" "QTH (opcional)" ""
  ask EL_EMAIL "Email for directory login (optional)" "Email para login al directorio (opcional)" ""
  echo
  echo "$(msg '=== Inbound EchoLink connections (e.g. the app) ===' \
              '=== Conexiones EchoLink entrantes (ej. la app) ===')"
  ask EL_MAX_INBOUND "Max simultaneous inbound connections (0 disables)" "Max conexiones entrantes simultaneas (0 deshabilita)" "1"
  if [[ "$EL_MAX_INBOUND" != "0" ]]; then
    ask EL_ALLOWED "Allowed inbound callsigns (comma-separated, required)" "Indicativos entrantes permitidos (separados por coma, requerido)" "$EL_CALL"
    ask EL_BLOCKED "Blocked callsigns (optional, always wins)" "Indicativos bloqueados (opcional, siempre gana)" ""
    ask EL_WELCOME "Welcome text shown to inbound stations (optional, \\n = line break)" "Texto de bienvenida para entrantes (opcional, \\n = salto de linea)" ""
  fi
  if [[ "$MODE" == "echolink-ysf" ]]; then
    EL_GAIN="0.5"
  else
    EL_GAIN="1.0"
  fi
  ask EL_GAIN "Volume gain 0 to 4" "Ganancia volumen 0 a 4" "$EL_GAIN"
  ask_yn USE_PROXY "Use EchoLink Proxy NAT?" "Usar EchoLink Proxy NAT?" "n"
  if [[ "$USE_PROXY" -eq 1 ]]; then
    ask EL_PROXY "proxy_server" "proxy_server" "proxy.example.net"
    ask EL_PROXY_PORT "proxy_port" "proxy_port" "8100"
    ask_secret EL_PROXY_PASS "proxy_password" "proxy_password" "PUBLIC"
  else
    ask EL_BIND "bind_addr local IP for UDP 5198/5199" "bind_addr IP local UDP 5198/5199" "192.168.1.20"
  fi
  echo
  echo "$(msg '=== Vocoder (AMBE on [peer.el], required) ===' \
              '=== Vocoder (AMBE en [peer.el], obligatorio) ===')"
  ask VOC_HOST "vocoder_host" "vocoder_host" "127.0.0.1"
  ask VOC_PORT "vocoder_port" "vocoder_port" "2460"
fi

if [[ "$NEED_YSF" -eq 1 ]]; then
  _ysf_def="$DMR_CALL"
  if [[ -z "$_ysf_def" && -n "$EL_CALL" ]]; then
    _ysf_def="${EL_CALL%%-*}"
  fi
  [[ -z "$_ysf_def" ]] && _ysf_def="N0CALL"
  ask YSF_CALL "YSF gateway callsign" "Indicativo gateway YSF" "$_ysf_def"
fi

AL_STALE="1440"
AL_RELOAD="15"
AL_DIR="./data"
AL_SUB_URL="$ADN_SUBSCRIBER_URL"
AL_CK_URL="$ADN_CHECKSUM_URL"

{
  echo "# Generated by examples/generate-config.sh — $(date -u +%Y-%m-%dT%H:%MZ)"
  echo "# Layout: $MODE ([peer.*] bus model)"
  echo "# Docs: docs/bridge.md · docs/bridge.es.md"
  echo

  if [[ "$NEED_YSF" -eq 1 ]]; then
    echo "[peer.fusion]"
    echo "type = ysf"
    echo "enabled = true"
    echo "host = $YSF_HOST"
    echo "port = $YSF_PORT"
    echo "callsign = $YSF_CALL"
    echo "dgid = $YSF_DGID"
    echo "log_level = $LOG_LEVEL"
    echo
  fi

  if [[ "$NEED_DMR_PEER" -eq 1 ]]; then
    echo "[peer.master]"
    echo "type = dmr"
    echo "enabled = true"
    echo "callsign    = $DMR_CALL"
    echo "dmrid       = $DMR_ID"
    echo "location    = $DMR_LOC"
    echo "description = $DMR_DESC"
    echo "host        = $DMR_HOST"
    echo "port        = $DMR_PORT"
    echo "tg          = $DMR_TG"
    if [[ "$CLEAR_DYN" -eq 1 ]]; then
      echo "clear_dynamic_tg = 1"
    fi
    if [[ -n "$DMR_OPTS" ]]; then
      echo "options     = $DMR_OPTS"
    fi
    echo "password    = $DMR_PASS"
    echo "log_level   = $LOG_LEVEL"
    echo
  fi

  if [[ "$NEED_EL" -eq 1 ]]; then
    echo "[peer.el]"
    echo "type = echolink"
    echo "enabled = true"
    echo "callsign = $EL_CALL"
    echo "password = $EL_PASS"
    if [[ "$USE_PROXY" -eq 1 ]]; then
      echo "proxy_server = $EL_PROXY"
      echo "proxy_port = $EL_PROXY_PORT"
      echo "proxy_password = $EL_PROXY_PASS"
    else
      echo "bind_addr = $EL_BIND"
    fi
    echo "host = $EL_HOST"
    echo "max_inbound = $EL_MAX_INBOUND"
    [[ -n "$EL_ALLOWED" ]] && echo "allowed_callsigns = $EL_ALLOWED"
    [[ -n "$EL_BLOCKED" ]] && echo "blocked_callsigns = $EL_BLOCKED"
    [[ -n "$EL_WELCOME" ]] && echo "welcome_text = $EL_WELCOME"
    echo "vocoder_host = $VOC_HOST"
    echo "vocoder_port = $VOC_PORT"
    echo "vocoder_log_level = $LOG_LEVEL"
    [[ -n "$EL_QTH" ]] && echo "qth = $EL_QTH"
    [[ -n "$EL_EMAIL" ]] && echo "email = $EL_EMAIL"
    echo "directory_servers = $EL_DIR"
    echo "login_interval = $EL_LOGIN"
    echo "station_list_interval = $EL_LIST"
    echo "gain = $EL_GAIN"
    echo "log_level = $LOG_LEVEL"
    echo
  fi

  echo "[aliases]"
  echo "stale_minutes = $AL_STALE"
  echo "reload_minutes = $AL_RELOAD"
  echo "data_dir = $AL_DIR"
  echo "subscriber_file = subscriber_ids.json"
  echo "subscriber_url = $AL_SUB_URL"
  echo "local_subscriber_file = local_subcriber_ids.json"
  echo "checksum_file = file_checksums.json"
  echo "checksum_url = $AL_CK_URL"
  echo
  echo "[log]"
  echo "level = $LOG_LEVEL"
} >"$INI_PATH"

chmod 600 "$INI_PATH" 2>/dev/null || true

echo
echo "$(msg 'Wrote' 'Escrito') $INI_PATH"

ask_yn WANT_UNIT "Also write a systemd unit file here?" "Tambien generar un unit de systemd aqui?" "y"

if [[ "$WANT_UNIT" -eq 1 ]]; then
  ask UNIT_USER "systemd User=" "Usuario systemd User=" "$(id -un)"
  ask WORKDIR "WorkingDirectory" "WorkingDirectory" "$WORKDIR_DEFAULT"
  ask BIN_PATH "Path to adn-bridge binary" "Ruta al binario adn-bridge" "$BIN_DEFAULT"
  UNIT_PATH="${OUT_DIR}/adn-bridge-${INSTANCE}.service"
  {
    echo "# Generated by examples/generate-config.sh"
    echo "# Install:"
    echo "#   sudo cp $UNIT_PATH /etc/systemd/system/adn-bridge-${INSTANCE}.service"
    echo "#   sudo systemctl daemon-reload"
    echo "#   sudo systemctl enable --now adn-bridge-${INSTANCE}.service"
    echo
    echo "[Unit]"
    echo "Description=adn-bridge ${MODE} (${INSTANCE})"
    echo "After=network-online.target"
    echo "Wants=network-online.target"
    echo
    echo "[Service]"
    echo "User=${UNIT_USER}"
    echo "Type=simple"
    echo "Restart=always"
    echo "RestartSec=3"
    echo "SyslogIdentifier=adn-bridge-${INSTANCE}"
    echo "WorkingDirectory=${WORKDIR}"
    echo "ExecStart=${BIN_PATH} -c ${INI_PATH}"
    echo "KillSignal=SIGINT"
    echo "TimeoutStopSec=15"
    echo
    echo "[Install]"
    echo "WantedBy=multi-user.target"
  } >"$UNIT_PATH"
  echo "$(msg 'Wrote' 'Escrito') $UNIT_PATH"
  echo
  echo "$(msg 'Install tip:' 'Para instalar:')"
  echo "  sudo cp \"$UNIT_PATH\" /etc/systemd/system/"
  echo "  sudo systemctl daemon-reload"
  echo "  sudo systemctl enable --now adn-bridge-${INSTANCE}.service"
  echo
  echo "$(msg 'Or use the instance template:' 'O use la plantilla de instancias:')"
  echo "  sudo cp \"${ROOT}/examples/adn-bridge@.service\" /etc/systemd/system/"
  echo "  # INI must be: ${CONFDIR_DEFAULT}/adn-bridge-${INSTANCE}.ini"
  echo "  sudo systemctl enable --now adn-bridge@${INSTANCE}.service"
fi

echo
echo "$(msg 'Done. Run:' 'Listo. Ejecutar:')"
echo "  ${BIN_DEFAULT} -c ${INI_PATH}"
echo
