"""Shared helpers, context and result types for the test runner."""
from __future__ import annotations
import os
import sys
import time
import json
from dataclasses import dataclass, field
from typing import Any, Callable, Optional

# Optional deps — imported lazily so a single missing dep doesn't break --only modes
try:
    import paramiko  # type: ignore
except Exception:
    paramiko = None  # type: ignore

try:
    from playwright.sync_api import sync_playwright  # type: ignore
except Exception:
    sync_playwright = None  # type: ignore


# ---------------------------------------------------------------------------
# Config (env-var driven; nothing checked into the repo)
# ---------------------------------------------------------------------------
def _env(name: str, default: Optional[str] = None, required: bool = False) -> str:
    v = os.environ.get(name, default)
    if required and not v:
        sys.exit(f"environment variable {name} is required (see docs/TESTING.md)")
    return v or ""


@dataclass
class Context:
    esp_host:  str   # e.g. http://192.0.2.10
    web_pw:    str
    pi_host:   str   # USB-Ethernet IP of the test Pi
    pi_user:   str
    pi_pw:     str
    pi_wifi_ip: str  # IP the Pi gets from the ESP DHCP
    dk_host:   str   # tailnet IP or LAN IP of the read-only diag node
    dk_user:   str
    dk_tailnet_ip: str  # this peer's tailnet IP (for Pi→tailnet egress test)
    dk_subnet_target: str  # an IP inside the LAN advertised by dk-lxc
    quick:     bool = False  # skip slow checks (e.g. exit-node toggle)

    @classmethod
    def from_env(cls) -> "Context":
        return cls(
            esp_host  = _env("ESP_HOST",  required=True),
            web_pw    = _env("ESP_WEB_PW", required=True),
            pi_host   = _env("PI_HOST",   required=True),
            pi_user   = _env("PI_USER",   "pi"),
            pi_pw     = _env("PI_PW",     required=True),
            pi_wifi_ip = _env("PI_WIFI_IP", required=True),
            dk_host   = _env("DK_HOST",   required=True),
            dk_user   = _env("DK_USER",   "root"),
            dk_tailnet_ip   = _env("DK_TAILNET_IP", required=True),
            dk_subnet_target = _env("DK_SUBNET_TARGET", required=True),
            quick     = bool(int(_env("TEST_QUICK", "0") or "0")),
        )


@dataclass
class Result:
    module: str
    name:   str
    ok:     bool
    detail: str = ""
    skipped: bool = False

    def render(self) -> str:
        if self.skipped: tag = "SKIP"
        else:            tag = "PASS" if self.ok else "FAIL"
        line = f"[{tag}] {self.module}: {self.name}"
        if self.detail and (not self.ok or self.skipped):
            line += f" — {self.detail}"
        return line


# ---------------------------------------------------------------------------
# HTTP / SPA helpers
# ---------------------------------------------------------------------------
class SpaClient:
    """Playwright-backed client that logs into the SPA once and exposes
    `fetch_json(path)` + `post_json(path, body)` to drive the device API."""

    def __init__(self, ctx: Context):
        if sync_playwright is None:
            raise RuntimeError("playwright not installed (`pip install playwright && playwright install chromium`)")
        self.ctx = ctx
        self._pw  = sync_playwright().start()
        self._br  = self._pw.chromium.launch(headless=True)
        self._cx  = self._br.new_context()
        self.page = self._cx.new_page()
        self.console_errors: list[tuple[str, str]] = []
        self.page.on("pageerror", lambda e: self.console_errors.append(("pageerror", str(e))))
        self.page.on("console",   lambda m: m.type == "error" and self.console_errors.append(("console.error", m.text)))
        self.page.goto(ctx.esp_host + "/", wait_until="domcontentloaded", timeout=15000)
        self.page.wait_for_timeout(500)
        if self.page.locator("#authLogin").is_visible():
            self.page.fill("#authLogin input[type=password]", ctx.web_pw)
            self.page.click("#authLogin button[type=submit], #authLogin .btn-primary")
            self.page.wait_for_timeout(1500)

    def close(self) -> None:
        try: self._br.close()
        except Exception: pass
        try: self._pw.stop()
        except Exception: pass

    def fetch_json(self, path: str) -> Any:
        return self.page.evaluate(
            f"""async () => {{
                const r = await fetch({json.dumps(path)}, {{cache:'no-store'}});
                if (!r.ok) return {{__http_status: r.status, __error: await r.text()}};
                return r.json();
            }}"""
        )

    def post_json(self, path: str, body: dict) -> Any:
        return self.page.evaluate(
            f"""async () => {{
                const r = await fetch({json.dumps(path)}, {{
                    method:'POST',
                    headers:{{'Content-Type':'application/json'}},
                    cache:'no-store',
                    body: {json.dumps(json.dumps(body))}
                }});
                let txt = '';
                try {{ txt = await r.text(); }} catch (e) {{}}
                return {{__http_status: r.status, __body: txt}};
            }}"""
        )

    def click_tab(self, tab: str) -> None:
        self.page.click(f"button.nav-tab[data-page={tab}]", timeout=4000)
        self.page.wait_for_timeout(800)


# ---------------------------------------------------------------------------
# SSH helpers
# ---------------------------------------------------------------------------
class SshClient:
    def __init__(self, host: str, user: str, password: Optional[str] = None, key: Optional[str] = None, timeout: int = 8):
        if paramiko is None:
            raise RuntimeError("paramiko not installed (`pip install paramiko`)")
        self.c = paramiko.SSHClient()
        self.c.set_missing_host_key_policy(paramiko.AutoAddPolicy())
        kw: dict = {"username": user, "timeout": timeout, "look_for_keys": True, "allow_agent": True}
        if password:
            kw.update(password=password, look_for_keys=False, allow_agent=False)
        if key:
            kw.update(key_filename=key)
        self.c.connect(host, **kw)
        self._password = password

    def run(self, cmd: str, sudo: bool = False, timeout: int = 20) -> tuple[int, str, str]:
        if sudo and self._password:
            cmd = f"echo {self._password} | sudo -S -p '' bash -c {repr(cmd)}"
        elif sudo:
            cmd = f"sudo -n bash -c {repr(cmd)}"
        stdin, stdout, stderr = self.c.exec_command(cmd, timeout=timeout)
        out = stdout.read().decode("utf-8", "replace")
        err = stderr.read().decode("utf-8", "replace")
        rc  = stdout.channel.recv_exit_status()
        # filter the sudo password prompt out of stderr
        err_lines = [l for l in err.splitlines() if "password for" not in l.lower()]
        return rc, out, "\n".join(err_lines)

    def close(self) -> None:
        try: self.c.close()
        except Exception: pass


# ---------------------------------------------------------------------------
# Tiny assertion helpers used by every module
# ---------------------------------------------------------------------------
def check(results: list[Result], module: str, name: str, ok: bool, detail: str = "") -> bool:
    results.append(Result(module=module, name=name, ok=ok, detail=detail))
    return ok


def skip(results: list[Result], module: str, name: str, reason: str) -> None:
    results.append(Result(module=module, name=name, ok=True, detail=reason, skipped=True))


def render_summary(results: list[Result]) -> str:
    n_pass = sum(1 for r in results if r.ok and not r.skipped)
    n_skip = sum(1 for r in results if r.skipped)
    n_fail = sum(1 for r in results if not r.ok)
    by_mod: dict[str, list[Result]] = {}
    for r in results:
        by_mod.setdefault(r.module, []).append(r)
    lines = ["", "=" * 70, "SUMMARY"]
    for mod, rs in by_mod.items():
        pf = sum(1 for r in rs if not r.ok)
        sk = sum(1 for r in rs if r.skipped)
        lines.append(f"  {mod:<10}  {len(rs):>3} checks, {pf} fail, {sk} skip")
    lines.append(f"  TOTAL     {len(results):>3} checks, {n_fail} fail, {n_skip} skip, {n_pass} pass")
    lines.append("=" * 70)
    return "\n".join(lines)
