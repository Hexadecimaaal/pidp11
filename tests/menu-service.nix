{
  pkgs ? import <nixpkgs> { },
}:
let
  launcher = pkgs.writeShellScript "pidp-test-launcher" ''
    set -eu
    test "$(${pkgs.coreutils}/bin/id -u)" -eq 0
    test ! -e /tmp/pidp-host-marker
    request_dir=''${PIDP_BOOT_REQUEST_FILE%/*}
    test "$(${pkgs.coreutils}/bin/stat -c '%u:%a' "$request_dir")" = 0:700
    test ! -e "$PIDP_BOOT_REQUEST_FILE"
    ${pkgs.coreutils}/bin/mkdir -p /var/lib/pidp-test
    stop() {
      trap "" TERM INT HUP
      ${pkgs.coreutils}/bin/sleep 0.2
      printf 'child-stop:%s:%s\n' "$$" "$PIDP_IDLED_PROGRAM" >> /var/lib/pidp-test/events
      exit 143
    }
    trap stop TERM INT HUP
    trap 'printf "0000\n" > "$PIDP_BOOT_REQUEST_FILE"' USR1
    printf '%s\n' "$PPID" > /var/lib/pidp-test/parent.pid
    printf '%s\n' "$$" > /var/lib/pidp-test/child.pid
    printf '%s\n' "$PIDP_IDLED_PROGRAM" > /var/lib/pidp-test/program
    printf 'child-start:%s:%s\n' "$$" "$PIDP_IDLED_PROGRAM" >> /var/lib/pidp-test/events
    printf '%s\n' "$$" > /var/lib/pidp-test/child.ready
    while :; do
      ${pkgs.coreutils}/bin/sleep 1
    done
  '';
  menu = pkgs.replaceVars ../panel-menu.sh {
    bash = pkgs.bash;
    coreutils = pkgs.coreutils;
  };
  fixture = pkgs.runCommand "pidp11-menu-service-fixture" { } ''
    install -Dm755 ${menu} "$out/bin/pidp-panel-menu"
    install -Dm755 ${launcher} "$out/bin/pidp-idled-demo"
  '';
in
pkgs.testers.runNixOSTest {
  name = "pidp11-menu-service";
  nodes.machine = {
    imports = [ ../nixos-module.nix ];
    services.pidp11 = {
      enable = true;
      package = fixture;
    };
    systemd.services.pidp11.environment.PIDP_IDLED_PROGRAM = "panel";
    virtualisation.memorySize = 768;
  };
  testScript = ''
    def wait_for_child(previous_pid=0):
        machine.wait_until_succeeds(
            "test -s /var/lib/pidp-test/child.pid && "
            f'test "$(cat /var/lib/pidp-test/child.pid)" != "{previous_pid}" && '
            'test "$(cat /var/lib/pidp-test/child.pid)" = '
            '"$(cat /var/lib/pidp-test/child.ready)"'
        )
        return int(machine.succeed("cat /var/lib/pidp-test/child.pid"))

    start_all()
    machine.wait_for_unit("pidp11.service")
    machine.succeed("systemctl is-enabled --quiet pidp11.service")
    assert machine.succeed("systemctl show -p User --value pidp11.service").strip() == "root"
    assert machine.succeed("systemctl show -p KillMode --value pidp11.service").strip() == "mixed"
    first_child_pid = wait_for_child()
    assert machine.succeed("cat /var/lib/pidp-test/program").strip() == "idled"

    machine.succeed(f"kill -USR1 {first_child_pid}")
    child_pid = wait_for_child(first_child_pid)
    assert machine.succeed("cat /var/lib/pidp-test/program").strip() == "panel"
    machine.succeed(f"test ! -e /proc/{first_child_pid}")
    events = machine.succeed("cat /var/lib/pidp-test/events").splitlines()
    assert events.index(f"child-stop:{first_child_pid}:idled") < events.index(
        f"child-start:{child_pid}:panel"
    ), events
    machine.succeed("systemctl stop pidp11.service")
    machine.succeed(f"test ! -e /proc/{child_pid}")
    assert machine.succeed("systemctl show -p Result --value pidp11.service").strip() == "success"
    events = machine.succeed("cat /var/lib/pidp-test/events").splitlines()
    assert events[-1] == f"child-stop:{child_pid}:panel", events

    machine.succeed("touch /tmp/pidp-host-marker")
    machine.succeed("systemctl start pidp11.service")
    machine.wait_for_unit("pidp11.service")
    child_pid = wait_for_child(child_pid)
    assert machine.succeed("cat /var/lib/pidp-test/program").strip() == "idled"
    parent_pid = int(machine.succeed("systemctl show -p MainPID --value pidp11.service"))
    assert int(machine.succeed("cat /var/lib/pidp-test/parent.pid")) == parent_pid
    machine.succeed("systemctl kill --kill-whom=main --signal=KILL pidp11.service")
    machine.wait_until_succeeds(
        f'test "$(cat /var/lib/pidp-test/parent.pid)" != "{parent_pid}"'
    )
    machine.wait_for_unit("pidp11.service")
    restarted_child_pid = wait_for_child(child_pid)
    assert machine.succeed("cat /var/lib/pidp-test/program").strip() == "idled"
    machine.succeed(f"test ! -e /proc/{child_pid}")
    assert int(machine.succeed("systemctl show -p NRestarts --value pidp11.service")) >= 1
    machine.succeed("systemctl stop pidp11.service")
    machine.succeed(f"test ! -e /proc/{restarted_child_pid}")
    assert machine.succeed("systemctl show -p Result --value pidp11.service").strip() == "success"
    events = machine.succeed("cat /var/lib/pidp-test/events").splitlines()
    assert events[-1] == f"child-stop:{restarted_child_pid}:idled", events
  '';
}
