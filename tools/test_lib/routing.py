"""4×2 routing scenario matrix.

Stages:        no-exit (exit_node_ip="")          exit-on (exit_node_ip=<peer>)
Directions:    Pi → external 8.8.8.8
               Pi → tailnet peer IP (DK_TAILNET_IP)
               Pi → tailnet-advertised subnet (DK_SUBNET_TARGET)
               Tailnet → Pi LAN IP (from dk-lxc: tailscale ping)
Plus an external public-IP delta check: `curl ifconfig.me` should
return a *different* address with exit-on than with exit-off (since
egress is going through the tailnet exit node, not the local STA path).

Honours these env vars:
    EXIT_NODE_IP   — tailnet IP of a peer to use as exit node.
                     If unset we auto-pick the first online direct
                     exit-node-capable peer from /api/tailscale.
    TEST_QUICK=1   — skips the exit-on stage entirely.

Always restores the original exit_node_ip setting at the end."""
from __future__ import annotations
import os, time
from .common import Context, Result, SpaClient, SshClient, check, skip

MODULE_ID = "routing"
MODULE_DESC = "Pi/tailnet ingress+egress with and without exit-node"

PING_TIMEOUT = 4
SETTLE_SEC = 8  # how long to wait after toggling exit_node_ip


def _pi_ping(pi: SshClient, target: str, count: int = 2) -> bool:
    rc, out, _ = pi.run(f"ping -c {count} -W {PING_TIMEOUT} {target}", timeout=PING_TIMEOUT * count + 5)
    return rc == 0


def _pi_curl_public_ip(pi: SshClient) -> str | None:
    rc, out, _ = pi.run("curl -s --max-time 6 https://api.ipify.org || curl -s --max-time 6 https://ifconfig.me",
                        timeout=15)
    out = out.strip()
    if rc == 0 and out and len(out) < 64:
        return out
    return None


def _dk_tailscale_ping(dk: SshClient, target: str) -> bool:
    # `tailscale ping` exits 0 on first successful reply; it auto-bails
    # after a few attempts when there's no route.
    rc, out, _ = dk.run(f"tailscale ping --c 3 --timeout 5s {target}", timeout=20)
    return rc == 0 and "pong" in out.lower()


def _auto_exit_node(spa: SpaClient) -> str | None:
    t = spa.fetch_json("/api/tailscale")
    if not isinstance(t, dict): return None
    for p in t.get("peers", []):
        if p.get("online") and p.get("direct_path") and p.get("is_exit_node") and p.get("vpn_ip"):
            return p["vpn_ip"]
    return None


def _set_exit_node(spa: SpaClient, ip: str) -> bool:
    resp = spa.post_json("/api/tailscale", {"settings": {"exit_node_ip": ip}})
    return isinstance(resp, dict) and resp.get("__http_status") == 200


def _stage_tests(label: str, ctx: Context, pi: SshClient, dk: SshClient | None,
                 accept_routes: bool, dk_has_esp_route: bool,
                 results: list[Result]) -> str | None:
    def tag(name): return f"[{label}] {name}"

    public_ip = _pi_curl_public_ip(pi)
    check(results, MODULE_ID, tag("Pi → external (curl)"),
          public_ip is not None, f"got {public_ip!r}")

    check(results, MODULE_ID, tag("Pi → external (ping 8.8.8.8)"),
          _pi_ping(pi, "8.8.8.8"))

    check(results, MODULE_ID, tag(f"Pi → tailnet peer ({ctx.dk_tailnet_ip})"),
          _pi_ping(pi, ctx.dk_tailnet_ip))

    # Pi → tailnet-advertised subnet only makes sense when the ESP has
    # accept_routes enabled — otherwise the peer's subnet route is never
    # programmed into microlink and there's no path to the LAN-side IP.
    if accept_routes:
        check(results, MODULE_ID, tag(f"Pi → tailnet subnet ({ctx.dk_subnet_target})"),
              _pi_ping(pi, ctx.dk_subnet_target))
    else:
        skip(results, MODULE_ID, tag(f"Pi → tailnet subnet ({ctx.dk_subnet_target})"),
             "ESP settings.accept_routes=false — peer subnet routes not programmed")

    # Tailnet → Pi LAN requires the upstream peer to have accepted the
    # ESP's advertise_routes (Tailscale admin / Headscale `nodes update
    # --route`). If the peer's kernel routing table doesn't even mention
    # the ESP AP CIDR, the ping below would FAIL by definition — skip
    # rather than report a false negative on the ESP.
    if dk is None:
        skip(results, MODULE_ID, tag(f"Tailnet (dk-lxc) → Pi LAN ({ctx.pi_wifi_ip})"),
             "dk-lxc unreachable")
    elif not dk_has_esp_route:
        skip(results, MODULE_ID, tag(f"Tailnet (dk-lxc) → Pi LAN ({ctx.pi_wifi_ip})"),
             "dk-lxc has no kernel route to ESP AP subnet — approve route in admin")
    else:
        check(results, MODULE_ID, tag(f"Tailnet (dk-lxc) → Pi LAN ({ctx.pi_wifi_ip})"),
              _dk_tailscale_ping(dk, ctx.pi_wifi_ip))

    return public_ip


def run(ctx: Context) -> list[Result]:
    results: list[Result] = []
    spa = SpaClient(ctx)
    pi = dk = None
    saved_exit = None
    try:
        # Open Pi + DK SSH sessions
        try:
            pi = SshClient(ctx.pi_host, ctx.pi_user, password=ctx.pi_pw, timeout=10)
        except Exception as e:
            check(results, MODULE_ID, "ssh to Pi", False, repr(e))
            return results
        try:
            dk = SshClient(ctx.dk_host, ctx.dk_user, timeout=10)  # key-based
        except Exception as e:
            check(results, MODULE_ID, "ssh to dk-lxc", False, repr(e))
            # Still continue the Pi tests below; DK-side checks will skip.

        # Snapshot the current exit_node_ip so we restore at the end,
        # and capture pre-conditions that gate later checks.
        t0 = spa.fetch_json("/api/tailscale")
        saved_exit    = (t0 or {}).get("settings", {}).get("exit_node_ip", "")
        accept_routes = bool((t0 or {}).get("settings", {}).get("accept_routes", False))

        # Pre-condition: does dk-lxc actually have a kernel route to the ESP AP CIDR?
        dk_has_esp_route = False
        if dk is not None:
            ap_prefix = ".".join(ctx.pi_wifi_ip.split(".")[:3]) + "."
            rc, out, _ = dk.run(f"ip route | grep -E '^{ap_prefix}0/'", timeout=10)
            dk_has_esp_route = rc == 0 and ap_prefix in out

        # ---- Stage A: no-exit ----
        if saved_exit != "":
            _set_exit_node(spa, "")
            time.sleep(SETTLE_SEC)
        public_off = _stage_tests("no-exit", ctx, pi, dk, accept_routes, dk_has_esp_route, results)

        # ---- Stage B: exit-on ----
        if ctx.quick:
            skip(results, MODULE_ID, "exit-on stage", "TEST_QUICK=1")
        else:
            exit_ip = os.environ.get("EXIT_NODE_IP") or _auto_exit_node(spa)
            if not exit_ip:
                skip(results, MODULE_ID, "exit-on stage",
                     "no online direct exit-node peer found and EXIT_NODE_IP unset")
            else:
                ok_set = _set_exit_node(spa, exit_ip)
                check(results, MODULE_ID, f"set exit_node_ip={exit_ip}", ok_set)
                time.sleep(SETTLE_SEC)
                public_on = _stage_tests("exit-on", ctx, pi, dk, accept_routes, dk_has_esp_route, results) if pi else None

                # Public-IP delta — should differ between stages if exit-routing actually applied
                if public_off and public_on:
                    check(results, MODULE_ID, "public IP changed when exit-on",
                          public_off != public_on,
                          f"off={public_off} on={public_on} (same address => exit-routing didn't take effect)")
                else:
                    skip(results, MODULE_ID, "public-IP delta",
                         f"missing samples: off={public_off!r} on={public_on!r}")

    finally:
        # Restore exit_node_ip
        if saved_exit is not None:
            _set_exit_node(spa, saved_exit)
        spa.close()
        if pi: pi.close()
        if dk: dk.close()
    return results
