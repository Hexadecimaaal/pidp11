{
  nixpkgs ? <nixpkgs>,
  pkgs ? import nixpkgs { },
  enableSanitizers ? false,
}:
let
  inherit (pkgs) lib stdenv;
  native = stdenv.buildPlatform == stdenv.hostPlatform;
  source = ./.;
  server = source + "/src/11_pidp_server/pidp11";
  sourceFiles = path: lib.fileset.fileFilter (file: file.hasExt "c" || file.hasExt "h") path;
  cflags = [
    "-std=c11"
    "-O2"
  ]
  ++ lib.optionals enableSanitizers [
    "-O1"
    "-g"
    "-fsanitize=address,undefined"
    "-fno-omit-frame-pointer"
  ];
  ldflags = [
    "-L${pkgs.libtirpc}/lib"
  ]
  ++ lib.optionals enableSanitizers [
    "-fsanitize=address,undefined"
  ];
  makeFlags = [
    "GPIO_BACKEND=v2"
    "CC=${stdenv.cc}/bin/${stdenv.cc.targetPrefix}cc"
    "CPPFLAGS=-I${pkgs.libtirpc.dev}/include/tirpc"
    "CFLAGS=${lib.concatStringsSep " " cflags}"
    "LDFLAGS=${lib.concatStringsSep " " ldflags}"
    "LDLIBS=-ltirpc -pthread -lrt"
  ];
in
assert lib.assertMsg (
  !enableSanitizers || native
) "pidp GPIO-v2 sanitizer builds require a native package set";
stdenv.mkDerivation {
  pname = "pidp-gpio-server";
  version = "unstable";

  src = lib.fileset.toSource {
    root = source;
    fileset = lib.fileset.unions [
      (server + "/makefile")
      (server + "/main.c")
      (server + "/main.h")
      (server + "/panel_actions.h")
      (server + "/gpio.h")
      (server + "/gpio_linux.c")
      (server + "/gpio_linux.h")
      (server + "/gpio_scan.c")
      (server + "/gpio_scan.h")
      (server + "/gpio_v2.c")
      (server + "/gpio_v2.h")
      (server + "/gpiopattern.c")
      (server + "/gpiopattern.h")
      (source + "/src/07.1_blinkenlight_server/print.c")
      (source + "/src/07.1_blinkenlight_server/print.h")
      (sourceFiles (source + "/src/00_common"))
      (sourceFiles (source + "/src/07.0_blinkenlight_api"))
    ];
  };

  strictDeps = true;
  nativeBuildInputs = [ pkgs.buildPackages.gnumake ];
  buildInputs = [ pkgs.libtirpc ];

  dontConfigure = true;
  dontStrip = enableSanitizers;
  hardeningDisable = lib.optional enableSanitizers "fortify";

  buildPhase = ''
    runHook preBuild
    make -C src/11_pidp_server/pidp11 ${lib.escapeShellArgs makeFlags}
    runHook postBuild
  '';

  installPhase = ''
    runHook preInstall
    install -Dm755 src/11_pidp_server/pidp11/bin-gpio-v2/pidp1170_blinkenlightd-v2 \
      "$out/bin/pidp1170_blinkenlightd"
    runHook postInstall
  '';

  doInstallCheck = native;
  installCheckPhase = ''
    runHook preInstallCheck
    ${lib.optionalString enableSanitizers ''
      export ASAN_OPTIONS=detect_leaks=1
      export UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1
    ''}

    # both diagnostic modes must work without opening gpio or an rpc listener.
    for option in -h -t; do
      PIDP_GPIO_CHIP= PIDP_GPIO_OFFSETS= \
        timeout 5 "$out/bin/pidp1170_blinkenlightd" "$option" > diagnostic.log 2>&1 || {
          cat diagnostic.log
          exit 1
        }
    done

    expect_mapping_error() {
      local expected="$1"
      shift
      local status=0
      timeout 5 "$@" > mapping.log 2>&1 || status=$?
      local diagnostic
      diagnostic=$(<mapping.log)
      cat mapping.log
      test "$status" -eq 1
      case "$diagnostic" in
        "$expected"*) ;;
        *) echo "unexpected startup diagnostic" >&2; exit 1 ;;
      esac
      case "$diagnostic" in
        *$'\n'*) echo "unexpected extra startup output" >&2; exit 1 ;;
      esac
    }

    expect_mapping_error "pidp gpio-v2 mapping is missing" \
      env -u PIDP_GPIO_CHIP -u PIDP_GPIO_OFFSETS "$out/bin/pidp1170_blinkenlightd"
    expect_mapping_error "pidp gpio-v2 mapping is malformed" \
      env PIDP_GPIO_CHIP=/dev/unused-pidp-test-chip PIDP_GPIO_OFFSETS=0,0 \
      "$out/bin/pidp1170_blinkenlightd"
    expect_mapping_error "pidp gpio-v2 demo mapping is missing" \
      env -u PIDP_GPIO_CHIP -u PIDP_GPIO_OFFSETS \
      "$out/bin/pidp1170_blinkenlightd" -D
    expect_mapping_error "pidp gpio-v2 demo mapping is malformed" \
      env PIDP_GPIO_CHIP=/dev/unused-pidp-test-chip PIDP_GPIO_OFFSETS=0,0 \
      "$out/bin/pidp1170_blinkenlightd" -D
    expect_mapping_error "pidp gpio-v2 demo chip preflight" \
      env PIDP_GPIO_CHIP=/dev/unused-pidp-test-chip \
      PIDP_GPIO_OFFSETS=61,44,47,54,51,50,60,43,55,37,39,56,49,53,52,48,46,59,36,42,38 \
      "$out/bin/pidp1170_blinkenlightd" -D
    expect_mapping_error "pidp gpio-v2 demo mapping is missing" \
      env -u PIDP_GPIO_CHIP -u PIDP_GPIO_OFFSETS \
      "$out/bin/pidp1170_blinkenlightd" -D -R -S
    expect_mapping_error "pidp gpio-v2 demo mapping is missing" \
      env -u PIDP_GPIO_CHIP -u PIDP_GPIO_OFFSETS \
      "$out/bin/pidp1170_blinkenlightd" -D -R -S -F
    for options in "-F" "-D -F" "-S -F"; do
      status=0
      timeout 5 "$out/bin/pidp1170_blinkenlightd" $options > option.log 2>&1 || status=$?
      test "$status" -eq 1
      diagnostic=$(<option.log)
      case "$diagnostic" in
        "Front-panel demo inputs (-F) require -D and -S."*) ;;
        *) cat option.log; exit 1 ;;
      esac
    done
    for option in -R -S; do
      status=0
      timeout 5 "$out/bin/pidp1170_blinkenlightd" "$option" > option.log 2>&1 || status=$?
      test "$status" -eq 1
      diagnostic=$(<option.log)
      case "$diagnostic" in
        *"requires -D."*|*"require -D."*) ;;
        *) cat option.log; exit 1 ;;
      esac
      case "$diagnostic" in
        *"IDLED_DEMO_READY"*|*"pidp gpio-v2"*) cat option.log; exit 1 ;;
      esac
    done
    echo "pidp gpio-v2 startup checks passed"
    runHook postInstallCheck
  '';
}
