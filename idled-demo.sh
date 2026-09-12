#!@bash@/bin/bash
set -eu
umask 077

coreutils='@coreutils@/bin'
if [ "$("$coreutils/id" -u)" -ne 0 ]; then
  printf '%s\n' 'pidp-idled-demo must run as root for gpio-v2' >&2
  exit 1
fi

simulator='@simulator@/bin/pdp11_realcons'
server='@server@/bin/pidp1170_blinkenlightd'
rpcbind='@rpcbind@/bin/rpcbind'
rpcinfo='@rpcbind@/bin/rpcinfo'
bash='@bash@/bin/bash'
flock='@util_linux@/bin/flock'
mount='@util_linux@/bin/mount'
unshare='@util_linux@/bin/unshare'
ip='@iproute2@/bin/ip'
sed='@gnused@/bin/sed'
tail="$coreutils/tail"
readlink="$coreutils/readlink"
# /run/current-system is hidden by the private runtime mount.
export PATH="$coreutils:@bash@/bin"

gpio_chip="${PIDP_GPIO_CHIP:-/dev/gpiochip0}"
gpio_offsets=61,44,47,54,51,50,60,43,55,37,39,56,49,53,52,48,46,59,36,42,38
program_mode="${PIDP_IDLED_PROGRAM-idled}"
menu_mode=0
if [ "${PIDP_BOOT_REQUEST_FILE+x}" = x ]; then
  menu_mode=1
fi
case "$program_mode" in
  idled)
    default_scan=single
    default_input=fixed
    ;;
  panel)
    default_scan=rows
    default_input=physical
    ;;
  *)
    printf '%s\n' 'PIDP_IDLED_PROGRAM must be idled or panel' >&2
    exit 2
    ;;
esac
if [ "$menu_mode" -ne 0 ]; then
  default_scan=rows
  default_input=physical
fi
scan_mode="${PIDP_IDLED_SCAN-$default_scan}"
input_mode="${PIDP_IDLED_INPUT-$default_input}"
if [ "$menu_mode" -ne 0 ]; then
  if [ "$scan_mode" != rows ] || [ "$input_mode" != physical ]; then
    printf '%s\n' 'menu mode requires rows and physical inputs' >&2
    exit 2
  fi
fi
case "$scan_mode" in
  single) server_args=(-D) ;;
  rows) server_args=(-D -R) ;;
  *)
    printf '%s\n' 'PIDP_IDLED_SCAN must be single or rows' >&2
    exit 2
    ;;
esac
case "$input_mode" in
  fixed) ;;
  physical) server_args+=(-S) ;;
  *)
    printf '%s\n' 'PIDP_IDLED_INPUT must be fixed or physical' >&2
    exit 2
    ;;
esac
if [ "$program_mode" = panel ] || [ "$menu_mode" -ne 0 ]; then
  if [ "$input_mode" != physical ]; then
    printf '%s\n' 'panel learning mode requires physical inputs' >&2
    exit 2
  fi
  server_args+=(-F)
  export PIDP_REALCONS_PANEL_ONLY=1
else
  unset PIDP_REALCONS_PANEL_ONLY
fi

if [ "${1:-}" = '--inside' ]; then
  parent_mount=$("$readlink" "/proc/$PPID/ns/mnt")
  parent_network=$("$readlink" "/proc/$PPID/ns/net")
  if [ "$#" -ne 1 ] \
      || [ "$("$readlink" /proc/self/ns/mnt)" = "$parent_mount" ] \
      || [ "$("$readlink" /proc/self/ns/net)" = "$parent_network" ] \
      || [ "$("$coreutils/stat" -Lc '%d:%i:%u' /proc/self/fd/9 2>/dev/null)" \
          != "$("$coreutils/stat" -c '%d:%i:%u' /run/pidp-idled-demo.lock)" ] \
      || ! "$flock" -n 9 2>/dev/null; then
    printf '%s\n' 'pidp-idled-demo internal invocation rejected' >&2
    exit 2
  fi
else
  if [ "$#" -ne 0 ]; then
    printf 'Usage: %s\n' "$0" >&2
    exit 2
  fi
  lock_file=/run/pidp-idled-demo.lock
  if [ -L "$lock_file" ]; then
    printf '%s\n' 'pidp-idled-demo lock is a symlink' >&2
    exit 1
  fi
  if [ -e "$lock_file" ] \
      && [ "$("$coreutils/stat" -c '%u' "$lock_file")" -ne 0 ]; then
    printf '%s\n' 'pidp-idled-demo lock is not root-owned' >&2
    exit 1
  fi
  exec 9>>"$lock_file"
  if ! "$flock" -n 9; then
    printf '%s\n' 'pidp-idled-demo is already running' >&2
    exit 1
  fi
  self=$0
  case "$self" in
    */*) ;;
    *) self=$(command -v "$self") ;;
  esac
  child_pid=
  outer_stop=0
  forward_signal()
  {
    outer_stop=1
    if [ -n "$child_pid" ]; then
      kill -TERM "$child_pid" 2>/dev/null || :
    fi
  }
  trap forward_signal INT TERM HUP
  status=0
  "$unshare" --mount --net "$bash" "$self" --inside &
  child_pid=$!
  if [ "$outer_stop" -ne 0 ]; then
    kill -TERM "$child_pid" 2>/dev/null || :
  fi
  while :; do
    if wait "$child_pid"; then
      status=0
      break
    else
      status=$?
    fi
    if ! kill -0 "$child_pid" 2>/dev/null; then
      break
    fi
  done
  trap - INT TERM HUP
  exit "$status"
fi

if [ "$gpio_chip" != /dev/gpiochip0 ] \
    || [ "${PIDP_GPIO_OFFSETS:-$gpio_offsets}" != "$gpio_offsets" ]; then
  printf '%s\n' 'pidp-idled-demo requires the fixed VisionFive 2 GPIO mapping' >&2
  exit 1
fi

mountpoint=/run
if [ ! -d "$mountpoint" ]; then
  printf '%s\n' 'pidp-idled-demo requires /run for the private RPC namespace' >&2
  exit 1
fi
"$mount" --make-rprivate /
"$mount" -t tmpfs -o mode=0755,size=16M tmpfs "$mountpoint"

script_dir=$("$coreutils/dirname" "$0")
boot="$script_dir/../share/pidp-visionfive2/$program_mode/boot.ini"
runtime_dir=$("$coreutils/mktemp" -d /tmp/pidp-idled-demo.XXXXXX)
rpc_log="$runtime_dir/rpcbind.log"
server_log="$runtime_dir/server.log"
simulator_log="$runtime_dir/simulator.log"
status_log="$runtime_dir/status.log"
run_boot="$runtime_dir/boot.ini"
rpc_pid=
server_pid=
simulator_pid=
spi_unbound=0

fail()
{
  printf 'pidp-idled-demo: %s\n' "$*" >&2
  exit 1
}

stop_pid()
{
  pid=$1
  if [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null; then
    kill -TERM "$pid" 2>/dev/null || :
    wait "$pid" 2>/dev/null || :
  fi
}

restore_spi()
{
  if [ "$spi_unbound" -eq 0 ]; then
    return 0
  fi
  if ! printf '%s\n' 10060000.spi > /sys/bus/amba/drivers/ssp-pl022/bind; then
    printf '%s\n' 'unable to rebind header SPI controller' >&2
    return 1
  fi
  spi_unbound=0
  n=0
  while [ "$n" -lt 100 ]; do
    if [ -e /sys/bus/amba/drivers/ssp-pl022/10060000.spi ]; then
      return 0
    fi
    n=$((n + 1))
    "$coreutils/sleep" 0.1
  done
  printf '%s\n' 'header SPI controller did not become bound' >&2
  return 1
}

cleanup()
{
  status=$?
  trap '' INT TERM HUP
  stop_pid "$simulator_pid"
  stop_pid "$server_pid"
  stop_pid "$rpc_pid"
  if ! restore_spi; then
    status=1
  fi
  printf 'IDLED_DEMO_STOPPED status=%d runtime=%s\n' "$status" "$runtime_dir" \
    | "$coreutils/tee" -a "$status_log"
  trap - EXIT
  exit "$status"
}
trap cleanup EXIT
trap 'exit 143' INT TERM HUP

model=$("$coreutils/tr" -d '\000' < /proc/device-tree/model 2>/dev/null || :)
case "$model" in
  *VisionFive\ 2\ v1.3B*) ;;
  *) fail "unexpected board model: $model" ;;
esac
[ -c "$gpio_chip" ] || fail "missing GPIO chip: $gpio_chip"
gpio_target=$("$readlink" -f /sys/bus/gpio/devices/gpiochip0 2>/dev/null || :)
case "$gpio_target" in
  */13040000.pinctrl/gpiochip0) ;;
  *) fail "unexpected GPIO chip device: $gpio_target" ;;
esac

spi_device=/sys/bus/amba/devices/10060000.spi
spi_driver=/sys/bus/amba/drivers/ssp-pl022
spi_binding="$spi_driver/10060000.spi"
[ -d "$spi_device" ] || fail 'header SPI controller is missing'
[ -e "$spi_binding" ] || fail 'header SPI controller is not bound'
spi_target=$("$readlink" -f "$spi_device/driver" 2>/dev/null || :)
[ "$spi_target" = "$spi_driver" ] \
  || fail "header SPI controller has unexpected driver: $spi_target"
flash_device=/sys/bus/spi/devices/spi1.0
flash_target=$("$readlink" -f "$flash_device" 2>/dev/null || :)
case "$flash_target" in
  */13010000.spi/*) ;;
  *) fail "unexpected onboard SPI-NOR device: $flash_target" ;;
esac
for child in /sys/bus/spi/devices/spi*; do
  [ -e "$child" ] || continue
  child_target=$("$readlink" -f "$child" 2>/dev/null || :)
  case "$child_target" in
    */10060000.spi/*) fail "header SPI controller has child device: $child" ;;
  esac
done

printf 'IDLED_DEMO_RUNTIME dir=%s mount_namespace=1 network_namespace=1\n' \
  "$runtime_dir" | "$coreutils/tee" -a "$status_log"
printf 'IDLED_DEMO_PREFLIGHT model=%s gpio_target=%s spi=ssp-pl022 ' \
  "$model" "$gpio_target" | "$coreutils/tee" -a "$status_log"
printf 'flash=13010000.spi untouched children=0\n' \
  | "$coreutils/tee" -a "$status_log"

if ! printf '%s\n' 10060000.spi > "$spi_driver/unbind"; then
  fail 'unable to unbind header SPI controller'
fi
spi_unbound=1
[ ! -e "$spi_binding" ] || fail 'header SPI controller remained bound after unbind'

export PIDP_GPIO_CHIP="$gpio_chip"
export PIDP_GPIO_OFFSETS="$gpio_offsets"
"$ip" link set lo up
"$rpcbind" -f -h 127.0.0.1 > "$rpc_log" 2>&1 &
rpc_pid=$!
n=0
while ! "$rpcinfo" -p 127.0.0.1 > /dev/null 2>&1; do
  if ! kill -0 "$rpc_pid" 2>/dev/null; then
    fail 'rpcbind exited before readiness'
  fi
  n=$((n + 1))
  [ "$n" -lt 100 ] || fail 'rpcbind readiness timeout'
  "$coreutils/sleep" 0.1
done

"$server" "${server_args[@]}" > "$server_log" 2>&1 &
server_pid=$!
n=0
while :; do
  content=$("$coreutils/cat" "$server_log" 2>/dev/null || :)
  case "$content" in
    *IDLED_DEMO_READY*) break ;;
  esac
  if ! kill -0 "$server_pid" 2>/dev/null; then
    fail 'pidp gpio server exited before readiness'
  fi
  n=$((n + 1))
  [ "$n" -lt 100 ] || fail 'pidp gpio server readiness timeout'
  "$coreutils/sleep" 0.1
done

if [ "$program_mode" = panel ]; then
  "$coreutils/cat" "$boot" > "$run_boot"
  printf '%s\n' 'set realcons panel=11/70' 'set realcons interval=8' \
    'set realcons connected' >> "$run_boot"
  simulator_args=(-e "$run_boot")
else
  "$sed" \
    -e 's/\r$//' \
    -e '/^!column -c 75 \.\.\/selections$/d' \
    -e '/^echo PiDP-11\/70 boot menu/d' \
    -e '/^echo Now running IDLED/d' \
    -e '/^echo [-][ -]*$/d' \
    "$boot" > "$run_boot"
  simulator_args=("$run_boot")
fi
"$simulator" "${simulator_args[@]}" < /dev/null > "$simulator_log" 2>&1 &
simulator_pid=$!
printf 'IDLED_DEMO_PROCESSES namespace=%d rpcbind=%s server=%s simulator=%s\n' \
  "$$" "$rpc_pid" "$server_pid" "$simulator_pid" \
  | "$coreutils/tee" -a "$status_log"

n=0
while :; do
  if [ "$program_mode" = panel ]; then
    content=$("$coreutils/cat" "$simulator_log")
    frames=$("$sed" -n 's/.*IDLED_DEMO_FRAME frame=\([0-9][0-9]*\).*/\1/p' \
      "$server_log" | "$tail" -n 1)
    case "$content" in
      *PANEL_CONSOLE_READY*)
        if [ -n "$frames" ] && [ "$frames" -gt 1 ]; then
          break
        fi
        ;;
    esac
  else
    changing=$("$sed" -n \
      's/.*IDLED_DEMO_FRAME.*changing_frames=\([0-9][0-9]*\).*/\1/p' \
      "$server_log" | "$tail" -n 1)
    if [ -n "$changing" ] && [ "$changing" -gt 1 ]; then
      break
    fi
  fi
  if ! kill -0 "$rpc_pid" 2>/dev/null; then
    fail 'rpcbind exited before simulator readiness'
  fi
  if ! kill -0 "$server_pid" 2>/dev/null; then
    fail 'pidp gpio server exited before simulator readiness'
  fi
  if ! kill -0 "$simulator_pid" 2>/dev/null; then
    fail 'simulator exited before readiness'
  fi
  n=$((n + 1))
  [ "$n" -lt 300 ] || fail 'simulator readiness timeout'
  "$coreutils/sleep" 0.1
done
if ! kill -0 "$rpc_pid" 2>/dev/null \
    || ! kill -0 "$server_pid" 2>/dev/null \
    || ! kill -0 "$simulator_pid" 2>/dev/null; then
  fail 'one demo process exited before readiness'
fi
if [ "$program_mode" = panel ]; then
  printf 'PANEL_LEARNING_READY cpu=halted memory=zeroed frame=%s ' "$frames" \
    | "$coreutils/tee" -a "$status_log"
else
  printf 'IDLED_DEMO_READY panel_frames=changing changing_frames=%s ' "$changing" \
    | "$coreutils/tee" -a "$status_log"
fi
printf 'rpc_scope=private-network-namespace scan_mode=%s input_mode=%s runtime=%s\n' \
  "$scan_mode" "$input_mode" "$runtime_dir" \
  | "$coreutils/tee" -a "$status_log"

while kill -0 "$simulator_pid" 2>/dev/null; do
  if ! kill -0 "$rpc_pid" 2>/dev/null; then
    fail 'rpcbind exited while simulator was running'
  fi
  if ! kill -0 "$server_pid" 2>/dev/null; then
    fail 'pidp gpio server exited while simulator was running'
  fi
  "$coreutils/sleep" 1
done
if wait "$simulator_pid"; then
  :
else
  status=$?
  exit "$status"
fi
