#!/usr/bin/env bash
# Copy of the game helper CLI but pointing at the video helper binary
set -u
XDG_CONFIG_HOME="${XDG_CONFIG_HOME:-$HOME/.config}"
XDG_DATA_HOME="${XDG_DATA_HOME:-$HOME/.local/share}"
XDG_STATE_HOME="${XDG_STATE_HOME:-$HOME/.local/state}"
RUNTIME_DIR="/tmp/dlssnr-${DLSSNR_UID:-$UID}"
CONFIG_DIR="$XDG_CONFIG_HOME/dlssnr"
CONFIG_FILE="$CONFIG_DIR/config.ini"
DATA_DIR="$XDG_DATA_HOME/dlssnr"
STATE_DIR="$XDG_STATE_HOME/dlssnr"
PREFIX_DIR="$XDG_DATA_HOME/dlssnr/prefix"
PID_FILE="$RUNTIME_DIR/helper.pid"
SHM_FILE="$RUNTIME_DIR/shm.bin"
LOG_FILE="$STATE_DIR/helper.log"
HELPER_EXE="/home/vthieu/Downloads/DLSS5Video/build/windows/windows/dlssnr_helper.exe"
case "${1:-}" in
  start) mkdir -p "$RUNTIME_DIR" "$STATE_DIR"; [ -f "$SHM_FILE" ] || : > "$SHM_FILE";
         echo "video helper started (pid $$)" ;; 
  status) echo "video helper tree: CONFIG=$CONFIG_FILE SHM=$SHM_FILE HELPER=$HELPER_EXE" ;;
  stop) echo "video helper stop" ;;
  *) echo "usage: $0 start|stop|status" ;;
esac
