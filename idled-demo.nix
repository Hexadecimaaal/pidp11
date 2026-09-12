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
  # the temporary runtime must not require the NixOS rpcbind service account.
  rpcbind = target.rpcbind.overrideAttrs (old: {
    configureFlags =
      builtins.filter (flag: !(pkgs.lib.hasPrefix "--with-rpcuser=" flag)) old.configureFlags
      ++ [ "--with-rpcuser=nobody" ];
  });
  script = pkgs.replaceVars ./idled-demo.sh {
    inherit simulator server rpcbind;
    util_linux = target.util-linux;
    iproute2 = target.iproute2;
    gnused = target.gnused;
    coreutils = target.coreutils;
    bash = target.bash;
  };
  menu_script = pkgs.replaceVars ./panel-menu.sh {
    bash = target.bash;
    coreutils = target.coreutils;
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
    install -Dm755 ${menu_script} "$out/bin/pidp-panel-menu"
    install -Dm644 ${./systems/idled/boot.ini} \
      "$out/share/pidp-visionfive2/idled/boot.ini"
    install -Dm644 ${./systems/panel/boot.ini} \
      "$out/share/pidp-visionfive2/panel/boot.ini"
    runHook postInstall
  '';

  meta = {
    description = "isolated PiDP-11 IDLED and front-panel learning runtime";
    mainProgram = "pidp-idled-demo";
    platforms = [ "riscv64-linux" ];
  };
}
