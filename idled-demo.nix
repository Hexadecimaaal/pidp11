{
  pkgs ? import <nixpkgs> { },
  target ? pkgs.pkgsCross.riscv64,
}:
let
  simulator = import ./default.nix {
    nixpkgs = <nixpkgs>;
    pkgs = target;
    enableNetwork = false;
  };
  server = import ./gpio-server.nix {
    nixpkgs = <nixpkgs>;
    pkgs = target;
  };
  script = pkgs.replaceVars ./idled-demo.sh {
    inherit simulator server;
    rpcbind = target.rpcbind;
    util_linux = target.util-linux;
    iproute2 = target.iproute2;
    gnused = target.gnused;
    coreutils = target.coreutils;
    bash = target.bash;
  };
in
target.stdenv.mkDerivation {
  pname = "pidp-idled-demo";
  version = "unstable";
  dontUnpack = true;
  strictDeps = true;

  installPhase = ''
    runHook preInstall
    install -Dm755 ${script} "$out/bin/pidp-idled-demo"
    install -Dm644 ${./systems/idled/boot.ini} \
      "$out/share/pidp-visionfive2/idled/boot.ini"
    runHook postInstall
  '';

  meta = {
    description = "switch-free low-load PiDP-11 IDLED demonstration";
    mainProgram = "pidp-idled-demo";
    platforms = [ "riscv64-linux" ];
  };
}
