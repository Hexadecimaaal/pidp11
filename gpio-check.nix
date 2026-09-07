{
  nixpkgs ? <nixpkgs>,
  pkgs ? import nixpkgs { },
}:
let
  inherit (pkgs) lib stdenv;
  native = stdenv.buildPlatform == stdenv.hostPlatform;
  source = ./src/11_pidp_server/pidp11;
in
stdenv.mkDerivation {
  pname = "pidp-gpio-v2-check";
  version = "0.1";
  src = lib.fileset.toSource {
    root = source;
    fileset = lib.fileset.unions [
      (source + "/gpio_v2.c")
      (source + "/gpio_v2.h")
      (source + "/gpio_v2_test.c")
    ];
  };
  dontConfigure = true;
  dontStrip = true;
  hardeningDisable = lib.optional native "fortify";
  buildPhase = ''
    runHook preBuild
    $CC -std=c11 -D_GNU_SOURCE -Wall -Wextra -Werror -g -O1 \
      ${lib.optionalString native "-fsanitize=address,undefined -fno-omit-frame-pointer"} \
      gpio_v2.c gpio_v2_test.c -o gpio-v2-test
    runHook postBuild
  '';
  doCheck = native;
  checkPhase = ''
    runHook preCheck
    ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 ./gpio-v2-test
    runHook postCheck
  '';
  installPhase = ''
    runHook preInstall
    install -Dm755 gpio-v2-test "$out/bin/gpio-v2-test"
    runHook postInstall
  '';
}
