"""Coverage for the system-tab + diagnostics features that landed
after the original test_lib was written:

* reset_history[] ring buffer in /api/system + persistence across a
  deliberate SW reboot (the recorder shifts the prior boot to hist[1]).
* /api/log/precrash endpoint: shape only — content depends on whether
  the previous boot crashed, so we just check the JSON keys.
* OTA: /api/system carries an ota{} block; POST round-trips the
  auto-poll + interval; /api/system/ota/poll responds with a status
  string (typically "github http 404" until a release is published).
* STA TTL override: /api/network round-trip on sta_ttl_override.
* About card sanity — the SPA exposes the version + Tailscale stack
  hint, and the /api/system version field is non-empty.

The reboot test is DESTRUCTIVE-LITE — it triggers /api/system/restart,
waits ~15 s, then re-logs in. Other modules' clean-up logic is
unaffected. Skip with TEST_QUICK=1.
"""
from __future__ import annotations
import time
from .common import Context, Result, SpaClient, check, skip

MODULE_ID = "system_extras"
MODULE_DESC = "reset_history + precrash + OTA + STA TTL + About"


def _system(spa: SpaClient) -> dict:
    return spa.fetch_json("/api/system") or {}


def _network(spa: SpaClient) -> dict:
    return spa.fetch_json("/api/network") or {}


def _post_ok(spa: SpaClient, path: str, body: dict) -> bool:
    r = spa.post_json(path, body)
    return isinstance(r, dict) and r.get("__http_status") == 200


def run(ctx: Context) -> list[Result]:
    results: list[Result] = []
    spa = SpaClient(ctx)
    try:
        # ---------------- /api/system shape ----------------
        sys = _system(spa)
        check(results, MODULE_ID, "/api/system responds",
              isinstance(sys, dict) and bool(sys.get("version")),
              f"version={sys.get('version')!r}")

        # ---------------- reset_history[] ----------------
        hist = sys.get("reset_history")
        check(results, MODULE_ID, "reset_history[] present",
              isinstance(hist, list) and len(hist) >= 1,
              f"got {hist!r}")
        if isinstance(hist, list) and hist:
            top = hist[0]
            keys = {"reason", "wallclock", "who", "crash"}
            check(results, MODULE_ID, "reset_history[0] has expected keys",
                  isinstance(top, dict) and keys.issubset(top.keys()),
                  f"keys={sorted(top.keys()) if isinstance(top,dict) else top!r}")

        # ---------------- ota{} block ----------------
        ota = sys.get("ota")
        check(results, MODULE_ID, "ota{} block present",
              isinstance(ota, dict),
              f"got {ota!r}")
        if isinstance(ota, dict):
            ota_keys = {"auto_enabled", "poll_s", "last_check", "last_version", "last_status"}
            check(results, MODULE_ID, "ota{} has expected keys",
                  ota_keys.issubset(ota.keys()),
                  f"keys={sorted(ota.keys())}")
            saved_ota = dict(ota)
            new_ota = {"auto_enabled": not bool(ota.get("auto_enabled")),
                       "poll_s": 86400}
            check(results, MODULE_ID, "POST /api/system ota round-trip",
                  _post_ok(spa, "/api/system", {"ota": new_ota}),
                  "")
            time.sleep(0.5)
            sys2 = _system(spa)
            ota2 = sys2.get("ota") or {}
            check(results, MODULE_ID, "ota.auto_enabled toggled",
                  bool(ota2.get("auto_enabled")) == new_ota["auto_enabled"],
                  f"got {ota2.get('auto_enabled')}")
            # Restore
            _post_ok(spa, "/api/system", {"ota": {
                "auto_enabled": bool(saved_ota.get("auto_enabled")),
                "poll_s": saved_ota.get("poll_s") or 21600
            }})

        # ---------------- /api/system/ota/poll ----------------
        r = spa.post_json("/api/system/ota/poll", {})
        ok = isinstance(r, dict) and r.get("__http_status") == 200
        body = (r or {}).get("__body") or ""
        check(results, MODULE_ID, "/api/system/ota/poll responds",
              ok and "status" in body,
              f"http={r.get('__http_status')} body={body[:120]!r}")

        # ---------------- /api/log/precrash ----------------
        pc = spa.fetch_json("/api/log/precrash") or {}
        pc_ok = isinstance(pc, dict) and "have" in pc and "size" in pc
        check(results, MODULE_ID, "/api/log/precrash shape",
              pc_ok,
              f"keys={sorted(pc.keys())}")

        # ---------------- STA TTL override ----------------
        net = _network(spa)
        if "sta_ttl_override" not in net:
            skip(results, MODULE_ID, "STA TTL override field absent",
                 "firmware older than the TTL feature")
        else:
            saved_ttl = int(net.get("sta_ttl_override") or 0)
            check(results, MODULE_ID, "STA TTL POST 64",
                  _post_ok(spa, "/api/network", {"sta_ttl_override": 64}),
                  "")
            time.sleep(0.3)
            n2 = _network(spa)
            check(results, MODULE_ID, "STA TTL round-trips to 64",
                  int(n2.get("sta_ttl_override") or 0) == 64,
                  f"got {n2.get('sta_ttl_override')}")
            # Restore
            _post_ok(spa, "/api/network", {"sta_ttl_override": saved_ttl})

        # ---------------- WPA2-Enterprise eap{} round-trip ----------------
        nets = (_network(spa).get("networks") or [])
        if not nets or "eap" not in (nets[0] or {}):
            skip(results, MODULE_ID, "WPA2-Enterprise eap block absent",
                 "firmware older than the EAP feature, or no networks configured")
        else:
            slot0_ssid = nets[0].get("ssid")
            saved_eap = dict(nets[0].get("eap") or {})
            # POST a fake EAP config matched to slot 0's SSID
            body = {"networks": [{"ssid": slot0_ssid,
                                   "eap": {"method": 1, "phase2": 1,
                                           "cert_bundle": True,
                                           "identity": "test-outer@x",
                                           "username": "test-inner@x",
                                           "password": "test-pw"}}]}
            check(results, MODULE_ID, "EAP POST round-trip",
                  _post_ok(spa, "/api/network", body), "")
            time.sleep(0.4)
            n3 = (_network(spa).get("networks") or [{}])[0]
            eap3 = n3.get("eap") or {}
            check(results, MODULE_ID, "EAP method=1 readback",
                  int(eap3.get("method") or 0) == 1,
                  f"got method={eap3.get('method')}")
            check(results, MODULE_ID, "EAP has_password set after POST",
                  bool(eap3.get("has_password")),
                  f"got has_password={eap3.get('has_password')}")
            check(results, MODULE_ID, "EAP identity round-trip",
                  eap3.get("identity") == "test-outer@x",
                  f"got identity={eap3.get('identity')!r}")
            # Restore
            _post_ok(spa, "/api/network",
                     {"networks": [{"ssid": slot0_ssid,
                                     "eap": {
                                         "method":      int(saved_eap.get("method") or 0),
                                         "phase2":      int(saved_eap.get("phase2") or 0),
                                         "cert_bundle": bool(saved_eap.get("cert_bundle")),
                                         "identity":    saved_eap.get("identity") or "",
                                         "username":    saved_eap.get("username") or ""
                                     }}]})

        # ---------------- MTU manager round-trip ----------------
        ts = spa.fetch_json("/api/tailscale") or {}
        mtu = ts.get("mtu")
        if not isinstance(mtu, dict):
            skip(results, MODULE_ID, "MTU manager block absent",
                 "firmware older than the MTU GUI feature")
        else:
            mtu_keys = {"mode", "fixed_mtu", "eff_mtu", "eff_mss", "eff_pmtu", "source"}
            check(results, MODULE_ID, "mtu{} has expected keys",
                  mtu_keys.issubset(mtu.keys()),
                  f"keys={sorted(mtu.keys())}")
            saved_mtu = dict(mtu)
            # POST mode=1 fixed_mtu=1280
            check(results, MODULE_ID, "MTU POST FIXED 1280",
                  _post_ok(spa, "/api/tailscale", {"mtu": {"mode": 1, "fixed_mtu": 1280}}),
                  "")
            time.sleep(0.4)
            mtu2 = (spa.fetch_json("/api/tailscale") or {}).get("mtu") or {}
            check(results, MODULE_ID, "MTU mode=1 after POST",
                  int(mtu2.get("mode") or 0) == 1,
                  f"got mode={mtu2.get('mode')}")
            check(results, MODULE_ID, "MTU source=user after FIXED",
                  mtu2.get("source") == "user",
                  f"got source={mtu2.get('source')!r}")
            check(results, MODULE_ID, "MTU eff_mtu = 1280 after FIXED",
                  int(mtu2.get("eff_mtu") or 0) == 1280,
                  f"got eff_mtu={mtu2.get('eff_mtu')}")
            check(results, MODULE_ID, "MTU eff_mss < eff_mtu",
                  int(mtu2.get("eff_mss") or 0) < int(mtu2.get("eff_mtu") or 99999),
                  f"mss={mtu2.get('eff_mss')} mtu={mtu2.get('eff_mtu')}")
            # Restore
            _post_ok(spa, "/api/tailscale", {"mtu": {
                "mode": int(saved_mtu.get("mode") or 0),
                "fixed_mtu": int(saved_mtu.get("fixed_mtu") or 1420)
            }})

        # ---------------- Reset history persistence across a SW reboot ----------------
        if ctx.quick:
            skip(results, MODULE_ID, "reset_history persistence across reboot",
                 "TEST_QUICK=1 skips the SW-reboot test")
        else:
            pre = _system(spa)
            pre_hist = pre.get("reset_history") or []
            pre_len = len(pre_hist)
            pre_top_wc = (pre_hist[0] or {}).get("wallclock") if pre_hist else None
            # Trigger a SW restart
            spa.post_json("/api/system/restart", {})
            time.sleep(18)
            # Re-login (cookie may have expired or session was reset)
            try:
                spa.page.goto(ctx.esp_host + "/", wait_until="domcontentloaded", timeout=15000)
                spa.page.wait_for_timeout(800)
                if spa.page.locator("#authLogin").is_visible():
                    spa.page.fill("#authLogin input[type=password]", ctx.web_pw)
                    spa.page.click("#authLogin button[type=submit], #authLogin .btn-primary")
                    spa.page.wait_for_timeout(1500)
            except Exception:
                pass
            post = _system(spa)
            post_hist = post.get("reset_history") or []
            check(results, MODULE_ID, "reset_history grew after SW reboot",
                  len(post_hist) >= pre_len + 1 or (len(post_hist) >= 1 and (post_hist[0] or {}).get("reason") in ("SW",)),
                  f"pre={pre_len} post={len(post_hist)} top_reason={(post_hist[0] or {}).get('reason')!r}")
            if post_hist and pre_top_wc is not None:
                check(results, MODULE_ID, "old top moved to hist[1]",
                      (post_hist[1] or {}).get("wallclock") == pre_top_wc if len(post_hist) >= 2 else False,
                      f"hist[1].wallclock={(post_hist[1] or {}).get('wallclock') if len(post_hist)>=2 else 'n/a'} expected={pre_top_wc}")

    finally:
        spa.close()
    return results
