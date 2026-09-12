{
  nixpkgs ? <nixpkgs>,
  pkgs ? import nixpkgs { },
}:
let
  inherit (pkgs) lib stdenv;
  native = stdenv.buildPlatform == stdenv.hostPlatform;
  source = ./.;
  mock = pkgs.replaceVars (source + "/tests/menu-launcher-mock.sh") {
    bash = pkgs.bash;
    coreutils = pkgs.coreutils;
  };
  menu = pkgs.replaceVars (source + "/panel-menu.sh") {
    bash = pkgs.bash;
    coreutils = pkgs.coreutils;
  };
in
stdenv.mkDerivation {
  pname = "pidp-panel-menu-check";
  version = "0.1";
  src = lib.fileset.toSource {
    root = source;
    fileset = lib.fileset.unions [
      (source + "/panel-menu.sh")
      (source + "/tests/menu-launcher-mock.sh")
    ];
  };
  strictDeps = true;
  dontConfigure = true;
  dontBuild = true;

  installPhase = ''
    runHook preInstall
    install -Dm755 ${menu} "$out/bin/pidp-panel-menu"
    install -Dm755 ${mock} "$out/bin/pidp-idled-demo"
    runHook postInstall
  '';

  doInstallCheck = native;
  installCheckPhase = ''
    runHook preInstallCheck
    set -eu
    coreutils=${pkgs.coreutils}/bin
    menu="$out/bin/pidp-panel-menu"
    tmp_root=''${TMPDIR:-/tmp}
    menu_pid=
    case_dir=
    request=
    request_dir=
    log=
    output=
    signal_parent=0

    fail()
    {
      printf 'menu check: %s\n' "$*" >&2
      exit 1
    }

    finish_case()
    {
      if [ -n "$menu_pid" ]; then
        kill -TERM "$menu_pid" 2>/dev/null || :
        wait "$menu_pid" 2>/dev/null || :
        menu_pid=
      fi
      if [ -n "$case_dir" ]; then
        "$coreutils/rm" -rf -- "$case_dir"
        case_dir=
      fi
      request=
      request_dir=
      log=
      output=
    }

    trap finish_case EXIT

    assert_contains()
    {
      haystack=$1
      needle=$2
      case "$haystack" in
        *"$needle"*) ;;
        *) fail "missing $needle" ;;
      esac
    }

    assert_not_contains()
    {
      haystack=$1
      needle=$2
      case "$haystack" in
        *"$needle"*) fail "unexpected $needle" ;;
        *) ;;
      esac
    }

    start_menu()
    {
      case_dir=$("$coreutils/mktemp" -d "$tmp_root/pidp-menu-check.XXXXXX")
      log="$case_dir/mock.log"
      output="$case_dir/menu.log"
      if [ "$signal_parent" -ne 0 ]; then
        PIDP_MENU_TEST_LOG="$log" \
          PIDP_MENU_TEST_CLEANUP_DELAY=0.2 \
          PIDP_MENU_TEST_SIGNAL_PARENT=1 \
          "$menu" > "$output" 2>&1 &
      else
        PIDP_MENU_TEST_LOG="$log" \
          PIDP_MENU_TEST_CLEANUP_DELAY=0.2 \
          "$menu" > "$output" 2>&1 &
      fi
      menu_pid=$!
      n=0
      while :; do
        menu_output=$("$coreutils/cat" "$output" 2>/dev/null || :)
        case "$menu_output" in
          *PIDP_PANEL_MENU_START*)
            request=''${menu_output##*request=}
            request=''${request%%$'\n'*}
            request_dir=''${request%/request}
            [ -n "$request" ] || fail 'request path was not published'
            [ -d "$request_dir" ] || fail 'request directory is missing'
            [ "$($coreutils/stat -c '%a' "$request_dir")" = 700 ] \
              || fail 'request directory is not mode 0700'
            [ ! -e "$request" ] || fail 'request exists before launch'
            return 0
            ;;
        esac
        if ! kill -0 "$menu_pid" 2>/dev/null; then
          "$coreutils/cat" "$output" >&2 || :
          fail 'menu exited before startup'
        fi
        n=$((n + 1))
        [ "$n" -lt 200 ] || fail 'menu startup timeout'
        "$coreutils/sleep" 0.01 || :
      done
    }

    wait_for_log()
    {
      needle=$1
      n=0
      while :; do
        log_output=$("$coreutils/cat" "$log" 2>/dev/null || :)
        case "$log_output" in
          *"$needle"*) return 0 ;;
        esac
        if ! kill -0 "$menu_pid" 2>/dev/null; then
          "$coreutils/cat" "$log" >&2 || :
          fail "menu exited before log marker $needle"
        fi
        n=$((n + 1))
        [ "$n" -lt 300 ] || fail "timeout waiting for $needle"
        "$coreutils/sleep" 0.01 || :
      done
    }

    stop_menu()
    {
      kill -TERM "$menu_pid" 2>/dev/null || :
      menu_status=0
      if wait "$menu_pid"; then
        menu_status=0
      else
        menu_status=$?
      fi
      [ "$menu_status" -eq 143 ] || fail "menu stop status was $menu_status"
      menu_pid=
      menu_output=$("$coreutils/cat" "$output" 2>/dev/null || :)
      log_output=$("$coreutils/cat" "$log" 2>/dev/null || :)
      assert_contains "$menu_output" 'PIDP_PANEL_MENU_STOPPED status=143'
      assert_contains "$log_output" 'cleanup-begin:idled'
      assert_contains "$log_output" 'cleanup-end:idled'
      case "$log_output" in
        *'cleanup-end:idled'*) ;;
        *) fail 'launcher cleanup did not finish' ;;
      esac
      [ ! -e "$request" ] || fail 'request remained after stop'
      [ ! -d "$request_dir" ] || fail 'request directory remained after stop'
    }

    # the initial program is IDLED, with no request left by startup.
    start_menu
    log_output=$("$coreutils/cat" "$log")
    assert_contains "$log_output" 'launch:idled'
    stop_menu
    finish_case

    # a valid selector reloads even when it names the current program.
    start_menu
    printf '1001\n' > "$request"
    wait_for_log 'cleanup-begin:idled'
    [ -e "$request" ] || fail 'reload request was removed before cleanup'
    wait_for_log 'cleanup-end:idled'
    n=0
    while :; do
      log_output=$("$coreutils/cat" "$log")
      case "$log_output" in
        *'cleanup-end:idled'*'launch:idled'*) break ;;
      esac
      n=$((n + 1))
      [ "$n" -lt 300 ] || fail 'same-program reload did not launch'
      "$coreutils/sleep" 0.01 || :
    done
    [ ! -e "$request" ] || fail 'reload request remained after launch'
    stop_menu
    finish_case

    # a panel request leaves the old launcher and request in place during
    # cleanup, then launches panel only after the old launcher exits.
    start_menu
    printf '0000\n' > "$request"
    wait_for_log 'cleanup-begin:idled'
    [ -e "$request" ] || fail 'request was removed before launcher cleanup'
    wait_for_log 'launch:panel'
    log_output=$("$coreutils/cat" "$log")
    case "$log_output" in
      *'cleanup-end:idled'*'launch:panel'*) ;;
      *) fail 'panel launch overlapped IDLED cleanup' ;;
    esac
    [ ! -e "$request" ] || fail 'request remained after panel launch'
    stop_menu
    finish_case
    # pressing the panel selector again reloads the halted, zeroed panel.
    start_menu
    printf '0000\n' > "$request"
    wait_for_log 'cleanup-begin:idled'
    wait_for_log 'launch:panel'
    printf '0000\n' > "$request"
    wait_for_log 'cleanup-begin:panel'
    [ -e "$request" ] || fail 'panel reload request was removed before cleanup'
    wait_for_log 'cleanup-end:panel'
    n=0
    while :; do
      log_output=$("$coreutils/cat" "$log")
      case "$log_output" in
        *'cleanup-end:panel'*'launch:panel'*) break ;;
      esac
      n=$((n + 1))
      [ "$n" -lt 300 ] || fail 'same-panel reload did not launch'
      "$coreutils/sleep" 0.01 || :
    done
    [ ! -e "$request" ] || fail 'panel reload request remained after launch'
    stop_menu
    finish_case


    # switch back to IDLED through the other valid selector.
    start_menu
    printf '0000\n' > "$request"
    wait_for_log 'launch:panel'
    printf '1001\n' > "$request"
    wait_for_log 'cleanup-begin:panel'
    [ -e "$request" ] || fail 'IDLED request was removed before cleanup'
    wait_for_log 'cleanup-end:panel'
    n=0
    while :; do
      log_output=$("$coreutils/cat" "$log")
      case "$log_output" in
        *'cleanup-end:panel'*'launch:idled'*) break ;;
      esac
      n=$((n + 1))
      [ "$n" -lt 300 ] || fail 'IDLED launch did not follow cleanup'
      "$coreutils/sleep" 0.01 || :
    done
    [ ! -e "$request" ] || fail 'IDLED request remained after launch'
    stop_menu
    finish_case

    # a NUL terminator is not the required newline and must be rejected.
    start_menu
    printf '0000\\0' > "$request"
    n=0
    while :; do
      menu_output=$("$coreutils/cat" "$output" 2>/dev/null || :)
      case "$menu_output" in
        *PIDP_PANEL_MENU_REJECTED*) break ;;
      esac
      n=$((n + 1))
      [ "$n" -lt 300 ] || fail 'NUL-terminated request was not rejected'
      "$coreutils/sleep" 0.01 || :
    done
    log_output=$("$coreutils/cat" "$log")
    assert_not_contains "$log_output" 'cleanup-begin:idled'
    [ ! -e "$request" ] || fail 'NUL-terminated request was not discarded'
    stop_menu
    finish_case

    # an unknown selector is rejected without stopping the current program;
    # a later valid request must still be accepted.
    start_menu
    printf '7777\n' > "$request"
    wait_for_log 'launch:idled'
    n=0
    while :; do
      menu_output=$("$coreutils/cat" "$output" 2>/dev/null || :)
      case "$menu_output" in
        *PIDP_PANEL_MENU_REJECTED*) break ;;
      esac
      n=$((n + 1))
      [ "$n" -lt 300 ] || fail 'invalid request was not rejected'
      "$coreutils/sleep" 0.01 || :
    done
    log_output=$("$coreutils/cat" "$log")
    assert_not_contains "$log_output" 'cleanup-begin:idled'
    [ ! -e "$request" ] || fail 'invalid request was not discarded'
    kill -0 "$menu_pid" 2>/dev/null || fail 'invalid request stopped menu'
    printf '0000\n' > "$request"
    wait_for_log 'launch:panel'
    stop_menu
    finish_case

    # a short first read is retained while the server finishes its write.
    start_menu
    : > "$request"
    "$coreutils/sleep" 0.25
    menu_output=$("$coreutils/cat" "$output" 2>/dev/null || :)
    assert_not_contains "$menu_output" 'PIDP_PANEL_MENU_REJECTED'
    printf '0000\n' >> "$request"
    wait_for_log 'launch:panel'
    stop_menu
    finish_case

    # a signal from the mocked launcher during PID publication must still
    # terminate and reap that launcher before the menu exits.
    signal_parent=1
    start_menu
    menu_status=0
    if wait "$menu_pid"; then
      menu_status=0
    else
      menu_status=$?
    fi
    menu_pid=
    [ "$menu_status" -eq 143 ] || fail "publication stop status was $menu_status"
    log_output=$("$coreutils/cat" "$log" 2>/dev/null || :)
    assert_contains "$log_output" 'cleanup-begin:idled'
    assert_contains "$log_output" 'cleanup-end:idled'
    menu_output=$("$coreutils/cat" "$output" 2>/dev/null || :)
    request=''${log_output##*request=}
    request=''${request%%$'\n'*}
    request_dir=''${request%/request}
    assert_contains "$menu_output" 'PIDP_PANEL_MENU_STOPPED status=143'
    [ ! -d "$request_dir" ] || fail 'publication-stop request directory remained'
    signal_parent=0
    finish_case

    # a child that exits without a requested stop fails the supervisor.
    start_menu
    launch_line=$("$coreutils/cat" "$log")
    child_pid=''${launch_line#*pid=}
    child_pid=''${child_pid%% *}
    [ -n "$child_pid" ] || fail 'mock child pid was not logged'
    kill -KILL "$child_pid"
    menu_status=0
    if wait "$menu_pid"; then
      menu_status=0
    else
      menu_status=$?
    fi
    menu_pid=
    [ "$menu_status" -eq 1 ] || fail "unexpected child status was $menu_status"
    menu_output=$("$coreutils/cat" "$output" 2>/dev/null || :)
    assert_contains "$menu_output" 'launcher exited unexpectedly'
    assert_contains "$menu_output" 'PIDP_PANEL_MENU_STOPPED status=1'
    log_output=$("$coreutils/cat" "$log" 2>/dev/null || :)
    assert_not_contains "$log_output" 'launch:panel'
    [ ! -d "$request_dir" ] || fail 'unexpected-exit request directory remained'
    finish_case

    printf '%s\n' 'pidp panel menu lifecycle checks passed'
    runHook postInstallCheck
  '';

  meta = {
    description = "offline lifecycle checks for the PiDP-11 panel menu supervisor";
    mainProgram = "pidp-panel-menu";
    platforms = lib.platforms.linux;
  };
}
