"""Tailscale state + identity + routes round-trip.

GET /api/tailscale shape, then a careful advertise_routes round-trip
that restores the original value at the end. The exit-node toggle is
exercised by tools/test_lib/routing.py — kept separate because it's
slow (needs ~10s settle time per change)."""
from __future__ import annotations
import time
from .common import Context, Result, SpaClient, check

MODULE_ID = "tailscale"
MODULE_DESC = "Tailscale settings/runtime/peers shape + advertise_routes round-trip"


def run(ctx: Context) -> list[Result]:
    results: list[Result] = []
    spa = SpaClient(ctx)
    try:
        t = spa.fetch_json("/api/tailscale")
        check(results, MODULE_ID, "shape: settings+runtime+peers",
              isinstance(t, dict) and "settings" in t and "runtime" in t and "peers" in t,
              f"got keys {list(t.keys()) if isinstance(t, dict) else type(t).__name__}")
        if not isinstance(t, dict):
            return results

        st = t.get("settings", {})
        rt = t.get("runtime", {})

        for key in ("enabled", "auth_key_set", "hostname", "login_server",
                    "advertise_routes", "max_peers", "accept_routes", "lan_bypass"):
            check(results, MODULE_ID, f"settings.{key} present", key in st, f"settings={list(st.keys())}")

        for key in ("connected", "tunnel_ip", "identity_persistent", "identity_pubkey_prefix"):
            check(results, MODULE_ID, f"runtime.{key} present", key in rt, f"runtime={list(rt.keys())}")

        check(results, MODULE_ID, "runtime.connected", bool(rt.get("connected")),
              f"runtime={rt}")
        check(results, MODULE_ID, "tunnel_ip starts with 100.",
              isinstance(rt.get("tunnel_ip"), str) and rt["tunnel_ip"].startswith("100."),
              f"tunnel_ip={rt.get('tunnel_ip')!r}")
        check(results, MODULE_ID, "identity_persistent",
              bool(rt.get("identity_persistent")),
              "node should boot with stored keys, not generate new ones each time")
        check(results, MODULE_ID, "identity_pubkey_prefix 16 hex",
              isinstance(rt.get("identity_pubkey_prefix"), str) and
              len(rt["identity_pubkey_prefix"]) == 16,
              f"prefix={rt.get('identity_pubkey_prefix')!r}")

        check(results, MODULE_ID, "peers is list",
              isinstance(t.get("peers"), list), f"peers={type(t.get('peers')).__name__}")

        # Cross-check: advertise_routes should include the AP CIDR
        ap = (spa.fetch_json("/api/status") or {}).get("ap", {})
        ap_ip = ap.get("ip", "")
        adv = st.get("advertise_routes", "")
        if ap_ip:
            # Just check the first three octets match — the CIDR-form may vary
            expected_prefix = ".".join(ap_ip.split(".")[:3]) + "."
            check(results, MODULE_ID, "advertise_routes covers AP subnet",
                  expected_prefix in adv,
                  f"ap_ip={ap_ip} advertise_routes={adv!r}")

        # ----- advertise_routes round-trip -----
        orig = adv
        probe = "10.99.99.0/24"
        resp = spa.post_json("/api/tailscale", {"settings": {"advertise_routes": probe}})
        check(results, MODULE_ID, "POST advertise_routes",
              isinstance(resp, dict) and resp.get("__http_status") == 200, f"resp={resp}")

        time.sleep(1)
        t2 = spa.fetch_json("/api/tailscale")
        new_adv = (t2 or {}).get("settings", {}).get("advertise_routes", "")
        check(results, MODULE_ID, "advertise_routes readback",
              new_adv == probe, f"want {probe!r} got {new_adv!r}")

        # Restore
        spa.post_json("/api/tailscale", {"settings": {"advertise_routes": orig}})
        time.sleep(1)
        t3 = spa.fetch_json("/api/tailscale")
        rest_adv = (t3 or {}).get("settings", {}).get("advertise_routes", "")
        check(results, MODULE_ID, "advertise_routes restored",
              rest_adv == orig, f"want {orig!r} got {rest_adv!r}")
    finally:
        spa.close()
    return results
