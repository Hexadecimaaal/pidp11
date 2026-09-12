{
  config,
  lib,
  pkgs,
  ...
}:
let
  cfg = config.services.pidp11;
in
{
  options.services.pidp11 = {
    enable = lib.mkEnableOption "the PiDP-11 front panel and simulator";
    package = lib.mkOption {
      type = lib.types.package;
      default = import ./idled-demo.nix {
        inherit pkgs;
        target = pkgs;
      };
      defaultText = lib.literalExpression "import ./idled-demo.nix { inherit pkgs; target = pkgs; }";
      description = "Package providing the pidp-panel-menu supervisor.";
    };
  };

  config = lib.mkIf cfg.enable {
    systemd.services.pidp11 = {
      description = "PiDP-11 front panel and switch-selected simulator";
      wantedBy = [ "multi-user.target" ];
      after = [ "systemd-udev-trigger.service" ];
      serviceConfig = {
        Type = "exec";
        User = "root";
        ExecStart = "${cfg.package}/bin/pidp-panel-menu";
        Restart = "on-failure";
        RestartSec = "5s";
        SuccessExitStatus = [ 143 ];
        # let the supervisor release GPIO and restore SPI before killing children.
        KillMode = "mixed";
        TimeoutStopSec = "60s";
        UMask = "0077";
        PrivateTmp = true;
        StandardInput = "null";
        StandardOutput = "journal";
        StandardError = "journal";
      };
    };
  };
}
