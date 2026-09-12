#!@bash@/bin/bash
set -eu
umask 077

coreutils='@coreutils@/bin'
export PATH="$coreutils:@bash@/bin"
self=$0
case "$self" in
  */*) ;;
  *) self=$(command -v "$self") ;;
esac
launcher=$("$coreutils/dirname" "$self")/pidp-idled-demo
if [ ! -x "$launcher" ]; then
  printf '%s\n' 'pidp panel menu: pidp-idled-demo is missing' >&2
  exit 1
fi

runtime_dir=$($coreutils/mktemp -d /tmp/pidp-panel-menu.XXXXXX)
request_file="$runtime_dir/request"
child_pid=
stop_requested=0
current_program=idled

# keep the request directory private even when the caller has a permissive umask.
if ! $coreutils/chmod 700 "$runtime_dir"; then
  printf '%s\n' 'pidp panel menu: unable to protect request directory' >&2
  $coreutils/rmdir "$runtime_dir" 2>/dev/null || :
  exit 1
fi

stop_requested()
{
  stop_requested=1
  if [ -n "$child_pid" ]; then
    kill -TERM "$child_pid" 2>/dev/null || :
  fi
}
trap stop_requested INT TERM HUP

wait_child()
{
  wait_status=0
  while :; do
    if wait "$child_pid"; then
      wait_status=0
      break
    else
      wait_status=$?
    fi
    # a signal can interrupt wait before the launcher has finished its own
    # cleanup. keep waiting until the exact launcher process is reaped.
    if kill -0 "$child_pid" 2>/dev/null; then
      continue
    fi
    break
  done
  child_pid=
}

remove_request()
{
  if [ -e "$request_file" ] || [ -L "$request_file" ]; then
    if ! $coreutils/rm -f -- "$request_file"; then
      printf '%s\n' 'pidp panel menu: unable to remove request' >&2
      return 1
    fi
  fi
  return 0
}

cleanup()
{
  status=$?
  trap '' INT TERM HUP
  if [ -n "$child_pid" ]; then
    kill -TERM "$child_pid" 2>/dev/null || :
    wait_child
  fi
  if ! remove_request; then
    status=1
  fi
  if ! $coreutils/rmdir "$runtime_dir" 2>/dev/null; then
    printf '%s\n' 'pidp panel menu: request directory was not empty' >&2
    status=1
  fi
  printf 'PIDP_PANEL_MENU_STOPPED status=%d\n' "$status"
  trap - EXIT
  exit "$status"
}
child_alive()
{
  proc_pid=
  proc_comm=
  proc_state=
  proc_rest=
  if [ -z "$child_pid" ] || ! kill -0 "$child_pid" 2>/dev/null; then
    return 1
  fi
  if [ ! -r "/proc/$child_pid/stat" ]; then
    return 1
  fi
  IFS=' ' read -r proc_pid proc_comm proc_state proc_rest \
    < "/proc/$child_pid/stat" || :
  [ "$proc_state" != Z ] && [ "$proc_state" != X ]
}

trap cleanup EXIT

launch_child()
{
  program=$1
  if [ -e "$request_file" ] || [ -L "$request_file" ]; then
    printf '%s\n' 'pidp panel menu: request remained before launch' >&2
    return 1
  fi
  export PIDP_BOOT_REQUEST_FILE="$request_file"
  export PIDP_IDLED_PROGRAM="$program"
  child_pid=
  # publish the PID immediately after forking. stop_requested handles a
  # signal that arrives in the publication window as well as before it.
  "$launcher" &
  child_pid=$!
  if [ "$stop_requested" -ne 0 ]; then
    kill -TERM "$child_pid" 2>/dev/null || :
  fi
  printf 'PIDP_PANEL_MENU_LAUNCHED program=%s pid=%s\n' "$program" "$child_pid"
}

# do not inspect a short file until it reaches five bytes. O_CREAT|O_EXCL
# makes the pathname visible before the server's write completes. Once the
# expected length is present, od checks every byte, including NUL.
inspect_request()
{
  request_result=0
  request_selection=
  if [ -L "$request_file" ] || [ ! -f "$request_file" ]; then
    if [ -L "$request_file" ]; then
      request_result=2
    fi
    return 0
  fi

  request_size=$($coreutils/wc -c < "$request_file" 2>/dev/null || printf '0')
  if [ "$request_size" -lt 5 ]; then
    return 0
  fi
  if [ "$request_size" -gt 5 ]; then
    request_result=2
    return 0
  fi
  request_bytes=$($coreutils/od -An -tx1 "$request_file" \
    | $coreutils/tr -d ' \n')
  case "$request_bytes" in
    303030300a)
      request_selection=panel
      request_result=1
      ;;
    313030310a)
      request_selection=idled
      request_result=1
      ;;
    *) request_result=2 ;;
  esac
}

if ! launch_child idled; then
  exit 1
fi
printf 'PIDP_PANEL_MENU_START program=idled request=%s\n' "$request_file"

while :; do
  if [ "$stop_requested" -ne 0 ]; then
    if [ -n "$child_pid" ]; then
      kill -TERM "$child_pid" 2>/dev/null || :
      wait_child
    fi
    exit 143
  fi

  # check liveness before accepting a request: a dead launcher is never
  # silently replaced, even if a stale request happens to be present.
  if ! child_alive; then
    wait_child
    printf '%s\n' 'pidp panel menu: launcher exited unexpectedly' >&2
    exit 1
  fi

  if [ -e "$request_file" ] || [ -L "$request_file" ]; then
    inspect_request
    if [ "$request_result" -eq 2 ]; then
      printf 'PIDP_PANEL_MENU_REJECTED request=%s\n' "$request_file" >&2
      if ! remove_request; then
        exit 1
      fi
    elif [ "$request_result" -eq 1 ]; then
      printf 'PIDP_PANEL_MENU_SWITCH from=%s to=%s\n' \
        "$current_program" "$request_selection"
      if ! kill -TERM "$child_pid" 2>/dev/null; then
        wait_child
        printf '%s\n' 'pidp panel menu: launcher exited unexpectedly' >&2
        exit 1
      fi
      wait_child
      if [ "$stop_requested" -ne 0 ]; then
        exit 143
      fi
      # the request stays in place until the old launcher has fully exited.
      if ! remove_request; then
        exit 1
      fi
      current_program=$request_selection
      if ! launch_child "$current_program"; then
        exit 1
      fi
    fi
  fi
  $coreutils/sleep 0.1 || :
done
