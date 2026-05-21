"""DNS relay × routing scenario matrix.

Walks (relay-off / relay-on with STA-learned upstream /
relay-on with custom 1.1.1.1 upstream) × (no-exit / exit-on) and
verifies the Pi can resolve a hostname in every combination.
HTTPS-egress (curl) is collected as advisory diagnostics — exit-node
mode is known-flaky on rapid toggles and we don't want it to wash out
the relay-specific verdict.

Always restores both relay state and exit_node_ip at the end.

Honours env vars:
    EXIT_NODE_IP   — tailnet IP to use as exit node; auto-discovered
                     from /api/tailscale peers if unset.
    TEST_QUICK=1   — skips the four exit-on scenarios (run only the
                     two no-exit ones).
"""
from __future__ import annotations
import os, time
from .common import Context, Result, SpaClient, SshClient, check, skip

MODULE_ID = "dns_relay"
MODULE_DESC = "DNS relay × routing scenarios (Pi can resolve + reach external)"

# Settle windows tuned to the relay's 12 s boot-delay + the 1–2 s
# softap-DHCP restart that follows each healthy transition.
SETTLE_AFTER_TOGGLE_S = 6


def _login_payload(ap: dict, enabled: bool, upstream: str) -> dict:
    """Build a /api/network POST body that keeps the AP settings intact
    and just patches dns_relay. The handler honours omit-to-keep on most
    fields but the SSID-validator wants a non-empty value."""
    return {
        "ap": {
            "ssid":    ap.get("ssid", ""),
            "channel": ap.get("channel", 6),
            "ip":      ap.get("ip", ""),
            "mask":    ap.get("mask", ""),
            "hidden":  bool(ap.get("hidden")),
            "dns_relay": {"enabled": enabled, "upstream": upstream},
        },
    }


def _set_relay(spa: SpaClient, enabled: bool, upstream: str) -> bool:
    state = spa.fetch_json("/api/network") or {}
    body = _login_payload(state.get("ap", {}), enabled, upstream)
    resp = spa.post_json("/api/network", body)
    return isinstance(resp, dict) and resp.get("__http_status") == 200


def _set_exit_node(spa: SpaClient, ip_str: str) -> bool:
    resp = spa.post_json("/api/tailscale", {"settings": {"exit_node_ip": ip_str}})
    return isinstance(resp, dict) and resp.get("__http_status") == 200


def _wait_relay_state(spa: SpaClient, want_healthy: bool, timeout_s: int = 30) -> bool:
    t0 = time.time()
    while time.time() - t0 < timeout_s:
        n = spa.fetch_json("/api/network") or {}
        if bool(n.get("ap", {}).get("dns_relay", {}).get("healthy")) == want_healthy:
            return True
        time.sleep(2)
    return False


def _auto_exit_node(spa: SpaClient) -> str:
    t = spa.fetch_json("/api/tailscale") or {}
    for p in t.get("peers", []):
        if (p.get("online") and p.get("direct_path") and p.get("is_exit_node")
                and p.get("vpn_ip")):
            return p["vpn_ip"]
    return ""


def _pi_renew(pi: SshClient) -> None:
    """Bounce the Pi's wlan0 profile so it requests a fresh DHCP lease.
    `nmcli connection up` on an active profile is a no-op; we need a
    real down+up cycle to flush the cached DNS-server entry."""
    pi.run("nmcli device disconnect wlan0", sudo=True, timeout=15)
    time.sleep(2)
    pi.run("nmcli connection up Csontikka-ESP32-Dev", sudo=True, timeout=15)
    # Wait for state 100 (connected) before sampling
    for _ in range(20):
        rc, out, _ = pi.run("nmcli -t -f GENERAL.STATE device show wlan0", timeout=10)
        if "100 (connected)" in out:
            break
        time.sleep(1)
    time.sleep(SETTLE_AFTER_TOGGLE_S)


def _pi_diag(pi: SshClient) -> dict:
    _, ns,  _ = pi.run("cat /etc/resolv.conf | grep -v '^#' | grep nameserver | head -1")
    _, dns, _ = pi.run("getent hosts cloudflare.com | head -1")
    # short timeout — the exit-on path can hang the egress briefly during
    # the toggle, and we'd rather log empty than block for minutes.
    _, curl, _ = pi.run("curl -s -m 12 https://api.ipify.org", timeout=20)
    return {"ns": ns.strip(), "dns": dns.strip(), "curl": curl.strip()}


def run(ctx: Context) -> list[Result]:
    results: list[Result] = []
    spa = SpaClient(ctx)
    pi = None
    saved_relay = None
    saved_exit = None
    try:
        try:
            pi = SshClient(ctx.pi_host, ctx.pi_user, password=ctx.pi_pw, timeout=10)
        except Exception as e:
            check(results, MODULE_ID, "ssh to Pi", False, repr(e))
            return results

        n0 = spa.fetch_json("/api/network") or {}
        t0 = spa.fetch_json("/api/tailscale") or {}
        ap_ip = n0.get("ap", {}).get("ip") or "192.168.31.1"
        saved_relay = n0.get("ap", {}).get("dns_relay") or {}
        saved_exit  = t0.get("settings", {}).get("exit_node_ip") or ""

        exit_ip = os.environ.get("EXIT_NODE_IP") or _auto_exit_node(spa) or ""
        if not exit_ip:
            skip(results, MODULE_ID, "exit-on scenarios", "no online direct exit-node peer available")

        scenarios = [
            # (label, relay_enabled, relay_upstream, exit_node_ip, requires_exit)
            ("relay-OFF | no-exit",            False, "",        "",      False),
            ("relay-OFF | exit-on",            False, "",        exit_ip, True),
            ("relay-ON  | no-exit | sta-up",   True,  "",        "",      False),
            ("relay-ON  | exit-on | sta-up",   True,  "",        exit_ip, True),
            ("relay-ON  | no-exit | 1.1.1.1",  True,  "1.1.1.1", "",      False),
            ("relay-ON  | exit-on | 1.1.1.1",  True,  "1.1.1.1", exit_ip, True),
        ]
        if ctx.quick:
            scenarios = [s for s in scenarios if not s[4]]

        for label, en, up, en_ip, needs_exit in scenarios:
            if needs_exit and not exit_ip:
                skip(results, MODULE_ID, label, "exit node unavailable")
                continue

            _set_relay(spa, en, up)
            _set_exit_node(spa, en_ip)
            _wait_relay_state(spa, want_healthy=en, timeout_s=30)
            # Extra settle: the state_cb runs softap_set_dns_addr async
            # after wait_relay_state returns; give it a clean window.
            time.sleep(SETTLE_AFTER_TOGGLE_S)
            _pi_renew(pi)
            d = _pi_diag(pi)

            # The relay's job is DNS resolution. ns-prefix verifies which
            # path was chosen (relay-on must hand out AP IP, relay-off
            # must NOT). dns verifies a real query made it through.
            ok_ns  = (ap_ip in d["ns"]) if en else (ap_ip not in d["ns"])
            ok_dns = "." in d["dns"] or ":" in d["dns"]
            # curl is advisory — exit-on egress is known to be flaky on
            # rapid toggles. Don't gate the relay verdict on it.
            ok_curl = d["curl"].count(".") == 3 and 0 < len(d["curl"]) < 20
            ok = ok_ns and ok_dns
            detail = f"ns={d['ns']!r} dns_ok={ok_dns} curl_ok={ok_curl}"
            check(results, MODULE_ID, label, ok, detail if not ok else "")

    finally:
        # Restore the original device state
        try:
            if saved_relay is not None:
                _set_relay(spa, bool(saved_relay.get("enabled")),
                           saved_relay.get("upstream") or "")
            if saved_exit is not None:
                _set_exit_node(spa, saved_exit)
        except Exception:
            pass
        spa.close()
        if pi: pi.close()
    return results
