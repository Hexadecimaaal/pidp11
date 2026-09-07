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
    runHook postInstallCheck
  '';

  meta = {
    description = "PiDP-11 SIMH emulator with real console support";
    mainProgram = "pdp11_realcons";
    platforms = lib.platforms.linux;
  };
}
