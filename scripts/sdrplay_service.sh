# Sourced by wwv_rsp2_live.sh and wwv_band_scan.sh: start and stop sdrplay.service.
#
# sdrplay.service stays disabled except while the RSP2 is in use. sdrplay_start sets
# SDRPLAY_STARTED=1 before it starts the unit, so a caller's EXIT trap that calls
# sdrplay_stop stops it on every exit path, including a failed start. SDRPLAY_TAG
# prefixes the messages.

SDRPLAY_STARTED=
: "${SDRPLAY_TAG:=sdrplay}"

sdrplay_state() { systemctl is-active sdrplay.service || true; }

# sdrplay_compact: ask the kernel to compact memory, so the API service's libusb
# finds the large contiguous blocks its isochronous transfers need. On saga the
# service crashed on libusb errno=12 (ENOMEM) with usbfs_memory_mb already 0, while
# free high-order pages were scarce (2026-11-21). Alex chose compaction before each
# run and reopen over a permanent vm setting. SDRPLAY_COMPACT=0 skips it; a failure
# is only reported.
sdrplay_compact() {
  [ "${SDRPLAY_COMPACT:-1}" = 0 ] && return 0
  if sudo -n sh -c 'echo 1 > /proc/sys/vm/compact_memory' 2>/dev/null; then
    echo "[$SDRPLAY_TAG] compacted memory before opening the RSP2" >&2
  else
    echo "[$SDRPLAY_TAG] memory compaction failed (continuing)" >&2
  fi
}

sdrplay_start() {
  sdrplay_compact
  echo "[$SDRPLAY_TAG] starting sdrplay.service" >&2
  SDRPLAY_STARTED=1
  sudo systemctl start sdrplay.service
  echo "[$SDRPLAY_TAG] sdrplay.service: $(sdrplay_state)" >&2
}

# Stop the unit if this script started it, then print its state. Call only after
# every process holding the RSP2 has exited: the API service must not be stopped
# under an open stream.
sdrplay_stop() {
  if [ -n "$SDRPLAY_STARTED" ]; then
    echo "[$SDRPLAY_TAG] stopping sdrplay.service" >&2
    sudo systemctl stop sdrplay.service || echo "[$SDRPLAY_TAG] ERROR: systemctl stop sdrplay.service failed" >&2
    # sdrplay_apiService (3.15.2) sometimes segfaults while it shuts down, after the
    # stream is closed; the unit is then stopped but "failed". Record that and clear
    # the flag, so the unit reads inactive (the journal keeps the crash).
    if [ "$(sdrplay_state)" = failed ]; then
      echo "[$SDRPLAY_TAG] sdrplay.service stopped with Result=$(systemctl show -p Result --value sdrplay.service); clearing the failed state" >&2
      sudo systemctl reset-failed sdrplay.service || true
    fi
    SDRPLAY_STARTED=
  fi
  echo "[$SDRPLAY_TAG] sdrplay.service: $(sdrplay_state)" >&2
}
