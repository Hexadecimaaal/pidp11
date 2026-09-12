{
  nixpkgs ? <nixpkgs>,
  pkgs ? import nixpkgs { },
}:
let
  inherit (pkgs) lib stdenv;
  native = stdenv.buildPlatform == stdenv.hostPlatform;
  source = ./.;
  server = source + "/src/11_pidp_server/pidp11";
  commonFlags = [
    "-std=c11"
    "-D_GNU_SOURCE"
    "-DBLINKENLIGHT_SERVER"
    "-Wall"
    "-Wextra"
    "-Werror"
    "-g"
    "-O1"
    "-I."
    "-I../../00_common"
    "-I../../07.0_blinkenlight_api"
    "-I${pkgs.libtirpc.dev}/include/tirpc"
  ];
  sanitizerFlags = lib.optionals native [
    "-fsanitize=address,undefined"
    "-fno-omit-frame-pointer"
  ];
  fixtureWarningFlags = [
    "-Wno-unused-parameter"
    "-Wno-unused-variable"
    "-Wno-sign-compare"
  ];
in
stdenv.mkDerivation {
  pname = "pidp-gpio-v2-check";
  version = "0.1";
  src = lib.fileset.toSource {
    root = source;
    fileset = lib.fileset.unions [
      (server + "/gpio_v2.c")
      (server + "/gpio_v2.h")
      (server + "/panel_actions.h")
      (server + "/panel_actions_test.c")
      (server + "/gpio_v2_test.c")
      (server + "/gpio_scan.c")
      (server + "/gpio_scan.h")
      (server + "/gpio.c")
      (server + "/pinctrl/gpiolib.h")
      (server + "/gpio_original_fixture.c")
      (server + "/gpio_waveform_test.c")
      (server + "/gpio_fixture.h")
      (server + "/gpiopattern.h")
      (server + "/gpiopattern.c")
      (server + "/main.c")
      (server + "/main.h")
      (server + "/gpio.h")
      (server + "/gpio_linux.h")
      (source + "/src/00_common/bitcalc.c")
      (source + "/src/00_common/bitcalc.h")
      (source + "/src/07.1_blinkenlight_server/print.h")
      (source + "/src/07.0_blinkenlight_api/blinkenlight_api_server_procs.h")
      (source + "/src/07.0_blinkenlight_api/rpcgen_linux/rpc_blinkenlight_api.h")
      (source + "/src/07.0_blinkenlight_api/blinkenlight_panels.c")
      (source + "/src/07.0_blinkenlight_api/blinkenlight_panels.h")
      (source + "/src/07.0_blinkenlight_api/historybuffer.h")
      (source + "/src/07.0_blinkenlight_api/historybuffer.c")
    ];
  };
  strictDeps = true;
  buildInputs = [ pkgs.libtirpc ];

  dontConfigure = true;
  dontStrip = true;
  hardeningDisable = lib.optional native "fortify";

  buildPhase = ''
    runHook preBuild
    cd src/11_pidp_server/pidp11
    common_flags=(${lib.escapeShellArgs commonFlags})
    fixture_warning_flags=(${lib.escapeShellArgs fixtureWarningFlags})
    sanitizer_flags=(${lib.escapeShellArgs sanitizerFlags})

    $CC "''${common_flags[@]}" "''${sanitizer_flags[@]}" \
      gpio_v2.c gpio_v2_test.c -o gpio-v2-test
    panel_fixture_flags=("''${common_flags[@]}" "''${sanitizer_flags[@]}"
      "''${fixture_warning_flags[@]}" -DPIDP_GPIO_V2
      -ffunction-sections -fdata-sections
      -I../../07.0_blinkenlight_api/rpcgen_linux
      -I../../07.1_blinkenlight_server)
    $CC "''${panel_fixture_flags[@]}" -Wno-error=format-overflow \
      -c panel_actions_test.c gpiopattern.c \
      ../../07.0_blinkenlight_api/blinkenlight_panels.c
    # retain upstream helper warnings without weakening the new fixture checks.
    $CC "''${panel_fixture_flags[@]}" -Wno-error=unused-function \
      -Wno-error=unused-but-set-variable \
      -c ../../07.0_blinkenlight_api/historybuffer.c
    $CC "''${panel_fixture_flags[@]}" -Wno-error=comment \
      -Wno-error=implicit-fallthrough -c ../../00_common/bitcalc.c
    $CC "''${sanitizer_flags[@]}" -Wl,--gc-sections \
      panel_actions_test.o gpiopattern.o blinkenlight_panels.o \
      historybuffer.o bitcalc.o -o panel-actions-test

    $CC "''${common_flags[@]}" "''${sanitizer_flags[@]}" \
      -c gpio_v2.c -o gpio-v2-waveform.o
    $CC "''${common_flags[@]}" "''${sanitizer_flags[@]}" \
      -c gpio_scan.c -o gpio-scan-waveform.o
    $CC "''${common_flags[@]}" "''${sanitizer_flags[@]}" \
      -c gpio_waveform_test.c -o gpio-waveform-test.o
    $CC "''${common_flags[@]}" "''${sanitizer_flags[@]}" \
      "''${fixture_warning_flags[@]}" \
      -c gpio_original_fixture.c -o gpio-original-fixture.o
    $CC "''${sanitizer_flags[@]}" \
      gpio-v2-waveform.o gpio-scan-waveform.o \
      gpio-waveform-test.o gpio-original-fixture.o \
      -pthread -lrt -o gpio-waveform-test
    runHook postBuild
  '';

  doCheck = native;
  checkPhase = ''
    runHook preCheck
    ASAN_OPTIONS=detect_leaks=1 \
      UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
      ./gpio-v2-test
    ASAN_OPTIONS=detect_leaks=1 \
      UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
      ./panel-actions-test
    ASAN_OPTIONS=detect_leaks=1 \
      UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
      ./gpio-waveform-test
    runHook postCheck
  '';

  installPhase = ''
    runHook preInstall
    install -Dm755 gpio-v2-test "$out/bin/gpio-v2-test"
    install -Dm755 panel-actions-test "$out/bin/panel-actions-test"
    install -Dm755 gpio-waveform-test "$out/bin/gpio-waveform-test"
    runHook postInstall
  '';
}
