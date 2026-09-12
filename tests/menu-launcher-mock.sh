#!@bash@/bin/bash
set -eu

coreutils='@coreutils@/bin'
log=${PIDP_MENU_TEST_LOG:?}
program=${PIDP_IDLED_PROGRAM:?}
cleanup_delay=${PIDP_MENU_TEST_CLEANUP_DELAY:-0.2}
cleanup()
{
  trap '' INT TERM HUP
  printf 'cleanup-begin:%s\n' "$program" >> "$log"
  "$coreutils/sleep" "$cleanup_delay"
  printf 'cleanup-end:%s\n' "$program" >> "$log"
  exit 0
}
trap cleanup INT TERM HUP
if [ "${PIDP_MENU_TEST_SIGNAL_PARENT:-0}" -ne 0 ]; then
  kill -TERM "$PPID" 2>/dev/null || :
fi

printf 'launch:%s pid=%s request=%s\n' "$program" "$$" \
  "${PIDP_BOOT_REQUEST_FILE:?}" >> "$log"

if [ "${PIDP_MENU_TEST_EXIT:-0}" -ne 0 ]; then
  exit "$PIDP_MENU_TEST_EXIT"
fi
while :; do
  "$coreutils/sleep" 0.05 || :
done
