{
  nixpkgs ? <nixpkgs>,
  pkgs ? import nixpkgs { },
  enableSanitizers ? false,
  enableNetwork ? true,
  enableVideo ? false,
}:

let
  inherit (pkgs) lib stdenv;
  package = import ./default.nix {
    inherit
      nixpkgs
      pkgs
      enableSanitizers
      enableNetwork
      enableVideo
      ;
  };
in
pkgs.mkShell (
  {
    # reuse the package's native tools and target libraries.
    inputsFrom = [ package ];

    CC = "${stdenv.cc}/bin/${stdenv.cc.targetPrefix}cc";
    PKG_CONFIG = "${pkgs.pkg-config}/bin/${pkgs.pkg-config.targetPrefix}pkg-config";
    BIN = "build";
    USE_NETWORK = if enableNetwork then "1" else "0";
    USE_VIDEO = if enableVideo then "1" else "0";
  }
  // lib.optionalAttrs enableSanitizers {
    CFLAGS = "-O1 -g -Wno-unused-result -fsanitize=address,undefined -fno-omit-frame-pointer";
    LDFLAGS = "-fsanitize=address,undefined";
  }
)
