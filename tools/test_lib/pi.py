"""Pi-side checks: SSH to PI_HOST, verify wlan0 is connected to the ESP AP,
the autoconnect-retries hardening landed, and the wlan-watchdog timer is
enabled+active. Also cross-checks the Pi's wlan0 IP against the ESP's DHCP
lease table.
"""
from __future__ import annotations
from .common import Context, Result, SpaClient, SshClient, check, skip

MODULE_ID = "pi"
MODULE_DESC = "Pi WiFi state, NM hardening, wlan-watchdog, ESP DHCP cross-check"


def run(ctx: Context) -> list[Result]:
    results: list[Result] = []
    try:
        ssh = SshClient(ctx.pi_host, ctx.pi_user, password=ctx.pi_pw, timeout=10)
    except Exception as e:
        check(results, MODULE_ID, "ssh to Pi", False, repr(e))
        return results

    try:
        rc, out, _ = ssh.run("nmcli -t -f GENERAL.STATE device show wlan0")
        state = ""
        for line in out.splitlines():
            if line.startswith("GENERAL.STATE:"):
                state = line.split(":", 1)[1].strip()
        check(results, MODULE_ID, "wlan0 connected", state == "100 (connected)", f"state={state!r}")

        rc, out, _ = ssh.run("nmcli -t -f connection.autoconnect-retries connection show Csontikka-ESP32-Dev")
        retries = ""
        for line in out.splitlines():
            if line.startswith("connection.autoconnect-retries:"):
                retries = line.split(":", 1)[1].strip()
        check(results, MODULE_ID, "autoconnect-retries=0 (infinite)", retries == "0",
              f"got {retries!r}")

        rc, out, _ = ssh.run("systemctl is-enabled wlan-watchdog.timer && systemctl is-active wlan-watchdog.timer")
        flags = out.strip().split()
        check(results, MODULE_ID, "wlan-watchdog.timer enabled+active",
              flags == ["enabled", "active"], f"got {flags}")

        rc, out, _ = ssh.run('grep "^Storage" /etc/systemd/journald.conf')
        check(results, MODULE_ID, "journald Storage=persistent",
              "Storage=persistent" in out, f"got {out!r}")

        rc, out, _ = ssh.run("ip -br addr show wlan0")
        # parse 'wlan0   UP   192.168.31.2/24 fe80::...'
        ip_present = ctx.pi_wifi_ip in out
        check(results, MODULE_ID, f"wlan0 has expected IP {ctx.pi_wifi_ip}",
              ip_present, f"ip line: {out!r}")

    finally:
        ssh.close()

    # Cross-check: ESP DHCP table sees the Pi
    spa = SpaClient(ctx)
    try:
        d = spa.fetch_json("/api/dhcp/leases")
        clients = d.get("clients", []) if isinstance(d, dict) else []
        pi_lease = next((c for c in clients if c.get("ip") == ctx.pi_wifi_ip), None)
        check(results, MODULE_ID, "ESP DHCP table includes Pi", pi_lease is not None,
              f"clients={[c.get('ip') for c in clients]}")
        if pi_lease:
            check(results, MODULE_ID, "Pi hostname seen by ESP",
                  pi_lease.get("hostname") in ("pizero2w", "pi-tailscale-test"),
                  f"hostname={pi_lease.get('hostname')!r}")
            rssi = pi_lease.get("rssi")
            check(results, MODULE_ID, "Pi RSSI plausible",
                  isinstance(rssi, (int, float)) and -90 < rssi < 0,
                  f"rssi={rssi}")
    finally:
        spa.close()
    return results
