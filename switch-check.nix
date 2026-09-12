{
  nixpkgs ? <nixpkgs>,
  pkgs ? import nixpkgs { },
  enableSanitizers ? false,
}:
let
  inherit (pkgs) lib stdenv;
  native = stdenv.buildPlatform == stdenv.hostPlatform;
  sanitizerFlags = lib.optionals enableSanitizers [
    "-O1"
    "-g"
    "-fsanitize=address,undefined"
    "-fno-omit-frame-pointer"
  ];
  source = ./.;
  server = source + "/src/11_pidp_server/pidp11";
  script = pkgs.replaceVars (source + "/switch-monitor.sh") {
    bash = pkgs.bash;
    coreutils = pkgs.coreutils;
    util_linux = pkgs.util-linux;
  };
in
assert lib.assertMsg (
  !enableSanitizers || native
) "pidp switch monitor sanitizer builds require a native package set";
stdenv.mkDerivation {
  pname = "pidp-switch-check";
  version = "unstable";

  src = lib.fileset.toSource {
    root = source;
    fileset = lib.fileset.unions [
      (server + "/gpio_v2.c")
      (server + "/gpio_v2.h")
      (server + "/switch_check.c")
      (source + "/switch-monitor.sh")
    ];
  };

  strictDeps = true;
  dontConfigure = true;
  dontStrip = enableSanitizers;
  hardeningDisable = lib.optional enableSanitizers "fortify";

  buildPhase = ''
    runHook preBuild
    $CC -std=c11 -D_GNU_SOURCE -Wall -Wextra -Werror -O2 \
      ${lib.escapeShellArgs sanitizerFlags} \
      -I src/11_pidp_server/pidp11 \
      src/11_pidp_server/pidp11/gpio_v2.c \
      src/11_pidp_server/pidp11/switch_check.c \
      -o pidp-switch-monitor
    runHook postBuild
  '';

  installPhase = ''
    runHook preInstall
    install -Dm755 pidp-switch-monitor "$out/bin/pidp-switch-monitor"
    install -Dm755 ${script} "$out/bin/pidp-switch-check"
    runHook postInstall
  '';
  doInstallCheck = native;
  installCheckPhase = ''
    runHook preInstallCheck
    export ASAN_OPTIONS=detect_leaks=1:halt_on_error=1:exitcode=86
    export UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1:exitcode=87

    "$out/bin/pidp-switch-monitor" --help > help.log

    expect_cli_failure() {
      local status=0
      "$out/bin/pidp-switch-monitor" "$@" > cli.log 2>&1 || status=$?
      test "$status" -eq 2
    }
    expect_cli_failure --seconds 0
    expect_cli_failure --seconds 86401
    expect_cli_failure --seconds not-a-number
    expect_cli_failure --chip

    status=0
    "$out/bin/pidp-switch-monitor" --inspect --chip /dev/null \
      > inspect.log 2>&1 || status=$?
    test "$status" -eq 1
    inspect_output=$(<inspect.log)
    case "$inspect_output" in
      *"switch-only monitor"*|*"GPIO_V2_GET_LINE_IOCTL"*)
        echo "inspect unexpectedly entered acquisition" >&2
        exit 1
        ;;
    esac

    echo "pidp switch monitor CLI checks passed"
    runHook postInstallCheck
  '';

  meta = {
    description = "switch-only PiDP-11 GPIO diagnostic for VisionFive 2";
    mainProgram = "pidp-switch-check";
    platforms = [
      "riscv64-linux"
      "x86_64-linux"
    ];
  };
}
