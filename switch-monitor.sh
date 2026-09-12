#!@bash@/bin/bash
set -eu
umask 077

coreutils='@coreutils@/bin'
flock='@util_linux@/bin/flock'
export PATH="$coreutils:@bash@/bin"

if [ "$("$coreutils/id" -u)" -ne 0 ]; then
  printf '%s\n' 'pidp-switch-check must run as root' >&2
  exit 1
fi
seconds=${1:-600}
settle_us=${PIDP_SWITCH_SETTLE_US:-100}
column_batch=${PIDP_SWITCH_COLUMN_BATCH:-12}
if [ "$#" -gt 1 ]; then
  printf 'usage: %s [seconds]\n' "$0" >&2
  exit 2
fi
case "$seconds" in
  ''|*[!0-9]*) printf '%s\n' 'seconds must be a positive integer' >&2; exit 2 ;;
esac
if [ "$seconds" -lt 1 ] || [ "$seconds" -gt 3600 ]; then
  printf '%s\n' 'seconds must be between 1 and 3600' >&2
  exit 2
fi

# share ownership with the IDLED launcher; /run is root-owned.
lock=/run/pidp-idled-demo.lock
if [ -L "$lock" ] || { [ -e "$lock" ] && [ "$("$coreutils/stat" -c '%u' "$lock")" -ne 0 ]; }; then
  printf '%s\n' 'unsafe panel lock file' >&2
  exit 1
fi
exec 9>>"$lock"
if ! "$flock" -n 9; then
  printf '%s\n' 'another panel launcher is running' >&2
  exit 1
fi
self=$("$coreutils/readlink" -f "$0")
monitor="$("$coreutils/dirname" "$self")/pidp-switch-monitor"
[ -x "$monitor" ] || { printf '%s\n' 'switch monitor is missing' >&2; exit 1; }

model=$("$coreutils/tr" -d '\000' < /proc/device-tree/model)
case "$model" in
  *VisionFive\ 2\ v1.3B*) ;;
  *) printf 'unexpected board: %s\n' "$model" >&2; exit 1 ;;
esac
header=/sys/bus/amba/devices/10060000.spi
driver=/sys/bus/amba/drivers/ssp-pl022
if [ "$("$coreutils/readlink" -f "$header/driver")" != "$driver" ]; then
  printf '%s\n' 'header SPI has an unexpected binding' >&2
  exit 1
fi
flash=$("$coreutils/readlink" -f /sys/bus/spi/devices/spi1.0)
case "$flash" in
  */13010000.spi/*) ;;
  *) printf '%s\n' 'onboard flash controller identity mismatch' >&2; exit 1 ;;
esac
for child in /sys/bus/spi/devices/spi*; do
  [ -e "$child" ] || continue
  target=$("$coreutils/readlink" -f "$child")
  case "$target" in
    */10060000.spi/*) printf 'header SPI has a child: %s\n' "$child" >&2; exit 1 ;;
  esac
done

child_pid=
restore_needed=0
cleanup()
{
  status=$?
  trap '' INT TERM HUP
  if [ -n "$child_pid" ]; then
    kill -TERM "$child_pid" 2>/dev/null || :
    wait "$child_pid" 2>/dev/null || :
  fi
  if [ "$restore_needed" -ne 0 ] && [ ! -e "$driver/10060000.spi" ]; then
    if ! printf '%s\n' 10060000.spi > "$driver/bind"; then
      printf '%s\n' 'failed to restore header SPI' >&2
      status=1
    fi
  fi
  if [ "$restore_needed" -ne 0 ] && [ ! -e "$driver/10060000.spi" ]; then
    printf '%s\n' 'header SPI is not bound after cleanup' >&2
    status=1
  fi
  printf 'SWITCH_CHECK_STOPPED status=%d\n' "$status"
  trap - EXIT
  exit "$status"
}
trap cleanup EXIT
trap 'exit 143' INT TERM HUP

# mark restoration first so a signal immediately after unbind cannot skip it.
restore_needed=1
printf '%s\n' 10060000.spi > "$driver/unbind"
[ ! -e "$driver/10060000.spi" ]
printf 'SWITCH_CHECK_START seconds=%s settle_us=%s column_batch=%s leds=off flash=untouched\n' "$seconds" "$settle_us" "$column_batch"
# defer exit until the child's PID is recorded so cleanup cannot miss it.
stop_requested=0
trap 'stop_requested=1' INT TERM HUP
"$monitor" --seconds "$seconds" --settle-us "$settle_us" --column-batch "$column_batch" &
child_pid=$!
trap 'exit 143' INT TERM HUP
if [ "$stop_requested" -ne 0 ]; then
  exit 143
fi
if wait "$child_pid"; then
  child_pid=
else
  status=$?
  child_pid=
  exit "$status"
fi
