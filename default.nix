{
  nixpkgs ? <nixpkgs>,
  pkgs ? import nixpkgs { },
  enableSanitizers ? false,
  enableNetwork ? true,
  enableVideo ? false,
}:

let
  inherit (pkgs) lib stdenv;
  simulatorDirectory = "src/02.3_simh/4.x+realcons/src";
  sourceFiles = path: lib.fileset.fileFilter (file: file.hasExt "c" || file.hasExt "h") path;
  buildFlags = [
    "CC=${stdenv.cc}/bin/${stdenv.cc.targetPrefix}cc"
    "PKG_CONFIG=${pkgs.pkg-config}/bin/${pkgs.pkg-config.targetPrefix}pkg-config"
    "BIN=build"
    "USE_NETWORK=${if enableNetwork then "1" else "0"}"
    "USE_VIDEO=${if enableVideo then "1" else "0"}"
  ]
  ++ lib.optionals enableSanitizers [
    "CFLAGS=-O1 -g -Wno-unused-result -fsanitize=address,undefined -fno-omit-frame-pointer"
    "LDFLAGS=-fsanitize=address,undefined"
  ];
in
assert lib.assertMsg (
  !enableSanitizers || stdenv.buildPlatform == stdenv.hostPlatform
) "pidp-visionfive2 sanitizer builds require a native package set";
stdenv.mkDerivation {
  pname = "pidp-visionfive2";
  version = "unstable";

  # include only build inputs, not metadata, bundled binaries, or generated output.
  src = lib.fileset.toSource {
    root = ./.;
    fileset = lib.fileset.unions [
      (sourceFiles ./src/00_common)
      (sourceFiles ./src/07.0_blinkenlight_api)
      (sourceFiles ./src/02.3_simh/4.x+realcons/src)
      ./src/02.3_simh/4.x+realcons/src/quickmake
      ./tests/headless.ini
      ./tests/console.ini
      ./tests/ether_bounds.c
      ./tests/panel-memory.ini
      ./tests/panel-prompt.ini
      ./tests/panel-running.ini
      ./tests/panel-connected.ini
      ./tests/panel-rpc.c
      ./systems/panel/boot.ini
    ];
  };

  strictDeps = true;
  nativeBuildInputs = [
    pkgs.pkg-config
    pkgs.buildPackages.file
  ];
  buildInputs = [
    pkgs.libtirpc
    pkgs.readline
    pkgs.dbus
  ]
  ++ lib.optional enableNetwork pkgs.libpcap
  ++ lib.optional enableVideo pkgs.SDL2;

  dontConfigure = true;
  dontStrip = enableSanitizers;
  hardeningDisable = lib.optional enableSanitizers "fortify";
  makeFlags = buildFlags;

  buildPhase = ''
    runHook preBuild
    make -C ${simulatorDirectory} -f quickmake ${lib.escapeShellArgs buildFlags}
    runHook postBuild
  '';

  installPhase = ''
    runHook preInstall
    install -Dm755 ${simulatorDirectory}/build/pdp11_realcons "$out/bin/pdp11_realcons"
    runHook postInstall
  '';

  doInstallCheck = stdenv.buildPlatform.canExecute stdenv.hostPlatform;
  installCheckPhase = ''
    runHook preInstallCheck
    ${lib.optionalString enableSanitizers ''
      export ASAN_OPTIONS=detect_leaks=1
      export UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1
    ''}
    "$out/bin/pdp11_realcons" tests/headless.ini > headless.log 2>&1 || {
      cat headless.log
      exit 1
    }
    cat headless.log
    case "$(cat headless.log)" in
      *HEADLESS_SMOKE_PASSED*) ;;
      *) echo "Headless instruction smoke check did not complete" >&2; exit 1 ;;
    esac
    "$out/bin/pdp11_realcons" -e tests/console.ini > console.log 2>&1 || {
      cat console.log
      exit 1
    }
    case "$(cat console.log)" in
      *CONSOLE_SMOKE_PASSED*) ;;
      *) cat console.log; echo "Console smoke check did not complete" >&2; exit 1 ;;
    esac
    "$out/bin/pdp11_realcons" tests/panel-memory.ini > panel-memory.log 2>&1 || {
      cat panel-memory.log
      exit 1
    }
    panel_memory_output=$(<panel-memory.log)
    case "$panel_memory_output" in
      *$'0:\t000000'*$'200:\t000000'*$'17757776:\t000000'*PANEL_MEMORY_SMOKE_PASSED*) ;;
      *) cat panel-memory.log; echo "Panel memory smoke check failed" >&2; exit 1 ;;
    esac
    PIDP_REALCONS_PANEL_ONLY=1 "$out/bin/pdp11_realcons" tests/panel-prompt.ini \
      < /dev/null > panel-prompt.log 2>&1 || {
        cat panel-prompt.log
        exit 1
      }
    panel_prompt_output=$(<panel-prompt.log)
    case "$panel_prompt_output" in
      *"panel-only console requires a connected panel"*) ;;
      *) cat panel-prompt.log; exit 1 ;;
    esac
    case "$panel_prompt_output" in
      *PANEL_CONSOLE_READY*) cat panel-prompt.log; exit 1 ;;
    esac
    $CC -std=c99 -D_GNU_SOURCE -I${pkgs.libtirpc.dev}/include/tirpc \
      -Isrc/07.0_blinkenlight_api/rpcgen_linux -DPIDP_PANEL_RPC_PRELOAD \
      -fPIC -shared tests/panel-rpc.c -ltirpc -o panel-rpc-preload.so
    $CC -std=c99 -D_GNU_SOURCE -I${pkgs.libtirpc.dev}/include/tirpc \
      -Isrc/07.0_blinkenlight_api/rpcgen_linux \
      tests/panel-rpc.c \
      src/07.0_blinkenlight_api/rpcgen_linux/rpc_blinkenlight_api_xdr.c \
      src/07.0_blinkenlight_api/rpcgen_linux/rpc_blinkenlight_api_svc.c \
      -ltirpc -o panel-rpc-server
    $CC -std=c99 -D_GNU_SOURCE -ffunction-sections -fdata-sections \
      ${lib.optionalString enableSanitizers "-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer"} \
      tests/ether_bounds.c -Wl,--gc-sections -o ether-bounds
    ./ether-bounds

    simulator="$out/bin/pdp11_realcons"
    shutdown_preload="$PWD/panel-rpc-preload.so"
    ${lib.optionalString enableSanitizers ''
      shutdown_preload="$($CC -print-file-name=libasan.so):$shutdown_preload"
    ''}
    shutdown_dir=$(mktemp -d "''${TMPDIR:-/tmp}/pidp-simulator-shutdown.XXXXXX")
    shutdown_sim_pid=
    shutdown_rpc_pid=
    shutdown_watchdog_pid=

    shutdown_fail()
    {
      printf 'simulator shutdown check: %s\n' "$*" >&2
      if [ -n "$shutdown_sim_pid" ]; then
        cat "$shutdown_dir/$shutdown_case.log" >&2 || :
      fi
      if [ -n "$shutdown_rpc_pid" ]; then
        cat "$shutdown_dir/rpc.log" >&2 || :
      fi
      exit 1
    }

    shutdown_cleanup()
    {
      if [ -n "$shutdown_sim_pid" ]; then
        kill -TERM "$shutdown_sim_pid" 2>/dev/null || :
        kill -KILL "$shutdown_sim_pid" 2>/dev/null || :
        wait "$shutdown_sim_pid" 2>/dev/null || :
      fi
      if [ -n "$shutdown_rpc_pid" ]; then
        kill -TERM "$shutdown_rpc_pid" 2>/dev/null || :
        wait "$shutdown_rpc_pid" 2>/dev/null || :
      fi
      if [ -n "$shutdown_watchdog_pid" ]; then
        kill -TERM "$shutdown_watchdog_pid" 2>/dev/null || :
        wait "$shutdown_watchdog_pid" 2>/dev/null || :
      fi
      rm -rf "$shutdown_dir"
    }
    trap shutdown_cleanup EXIT

    wait_for_rpc()
    {
      shutdown_case=halted
      rpc_port_file="$shutdown_dir/rpc.port"
      "$PWD/panel-rpc-server" "$rpc_port_file" "$shutdown_dir/rpc.tty" \
        > "$shutdown_dir/rpc.log" 2>&1 &
      shutdown_rpc_pid=$!
      n=0
      while [ ! -s "$rpc_port_file" ]; do
        if ! kill -0 "$shutdown_rpc_pid" 2>/dev/null; then
          shutdown_fail 'panel RPC fixture exited before readiness'
        fi
        n=$((n + 1))
        [ "$n" -lt 300 ] || shutdown_fail 'panel RPC fixture readiness timeout'
        sleep 0.01
      done
      shutdown_rpc_port=$(cat "$rpc_port_file")
      shutdown_tty=$(cat "$shutdown_dir/rpc.tty")
      case "$shutdown_rpc_port" in
        ""|*[!0-9]*) shutdown_fail 'panel RPC fixture published an invalid port' ;;
      esac
    }

    wait_for_marker()
    {
      shutdown_marker=$1
      shutdown_log=$2
      n=0
      while :; do
        shutdown_output=$(cat "$shutdown_log" 2>/dev/null || :)
        case "$shutdown_output" in
          *"$shutdown_marker"*) return 0 ;;
        esac
        if ! kill -0 "$shutdown_sim_pid" 2>/dev/null; then
          cat "$shutdown_log" >&2 || :
          shutdown_fail "simulator exited before $shutdown_marker"
        fi
        n=$((n + 1))
        [ "$n" -lt 300 ] || shutdown_fail "timeout waiting for $shutdown_marker"
        sleep 0.01
      done
    }

    stop_shutdown_simulator()
    {
      shutdown_case=$1
      shutdown_log="$shutdown_dir/$shutdown_case.log"
      kill -TERM "$shutdown_sim_pid" 2>/dev/null \
        || shutdown_fail 'simulator exited before TERM'
      (
        sleep 3
        kill -KILL "$shutdown_sim_pid" 2>/dev/null || :
      ) &
      shutdown_watchdog_pid=$!
      if wait "$shutdown_sim_pid"; then
        shutdown_status=0
      else
        shutdown_status=$?
      fi
      kill -TERM "$shutdown_watchdog_pid" 2>/dev/null || :
      wait "$shutdown_watchdog_pid" 2>/dev/null || :
      shutdown_watchdog_pid=
      shutdown_sim_pid=
      [ "$shutdown_status" -eq 0 ] \
        || {
          cat "$shutdown_log" >&2 || :
          shutdown_fail "$shutdown_case exited with status $shutdown_status"
        }
    }

    wait_for_rpc
    PIDP_REALCONS_PANEL_ONLY=1 \
      PIDP_PANEL_RPC_PORT="$shutdown_rpc_port" \
      LD_PRELOAD="$shutdown_preload" \
      "$simulator" tests/panel-running.ini < "$shutdown_tty" \
      > "$shutdown_dir/running.log" 2>&1 &
    shutdown_sim_pid=$!
    shutdown_case=running
    wait_for_marker PANEL_RUNNING_READY "$shutdown_dir/running.log"
    wait_for_marker PANEL_CPU_RUNNING "$shutdown_dir/rpc.log"
    case "$(<"$shutdown_dir/running.log")" in
      *PANEL_CONSOLE_READY*) shutdown_fail "CPU halted before TERM" ;;
    esac
    stop_shutdown_simulator running

    PIDP_REALCONS_PANEL_ONLY=1 \
      PIDP_PANEL_RPC_PORT="$shutdown_rpc_port" \
      LD_PRELOAD="$shutdown_preload" \
      "$simulator" tests/panel-connected.ini < "$shutdown_tty" \
      > "$shutdown_dir/halted.log" 2>&1 &
    shutdown_sim_pid=$!
    shutdown_case=halted
    wait_for_marker PANEL_CONSOLE_READY "$shutdown_dir/halted.log"
    kill -INT "$shutdown_sim_pid"
    sleep 0.05
    kill -0 "$shutdown_sim_pid" 2>/dev/null \
      || shutdown_fail "SIGINT terminated the panel-only console"
    stop_shutdown_simulator halted
    kill -TERM "$shutdown_rpc_pid" 2>/dev/null || :
    wait "$shutdown_rpc_pid" 2>/dev/null || :
    shutdown_rpc_pid=
    printf '%s\n' 'PANEL_SHUTDOWN_RUNNING_AND_HALTED_PASSED'
    runHook postInstallCheck
  '';

  meta = {
    description = "PiDP-11 SIMH emulator with real console support";
    mainProgram = "pdp11_realcons";
    platforms = lib.platforms.linux;
  };
}
