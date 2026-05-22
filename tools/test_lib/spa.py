"""SPA tests: login, tab walk, pristine-validator sanity, console errors."""
from __future__ import annotations
from .common import Context, Result, SpaClient, check

MODULE_ID = "spa"
MODULE_DESC = "Web UI: login, each tab renders, no JS errors, no spurious red validators"

TABS = ["status", "network", "tailscale", "firewall", "system", "tools"]


def run(ctx: Context) -> list[Result]:
    results: list[Result] = []
    spa = SpaClient(ctx)
    try:
        # Login flow already happened in SpaClient.__init__. Test that the
        # main nav is reachable (more robust than asserting on the auth
        # overlay's CSS visibility, which can flap during the dismiss
        # animation).
        nav_ok = spa.page.locator(".nav-tab[data-page=status]").is_visible()
        check(results, MODULE_ID, "login dismissed, nav visible", nav_ok,
              "main navigation still hidden after login")

        for tab in TABS:
            try:
                spa.click_tab(tab)
                spa.page.wait_for_timeout(1500)
                check(results, MODULE_ID, f"tab {tab}: click", True)
            except Exception as e:
                check(results, MODULE_ID, f"tab {tab}: click", False, repr(e))
                continue

            # Firewall populates its cards lazily via loadFirewall() →
            # /api/firewall → renderAclCard. Wait up to 6 s for at least
            # one card to appear before counting; otherwise we read 0
            # while the async fetch is still in flight.
            try:
                spa.page.wait_for_selector(f"#page-{tab} .card", timeout=6000)
            except Exception:
                pass
            cards = spa.page.locator(f"#page-{tab} .card").count()
            check(results, MODULE_ID, f"tab {tab}: cards rendered",
                  cards > 0, f"{cards} cards found")

            invalid = spa.page.locator(f"#page-{tab} input.input-invalid").count()
            check(results, MODULE_ID, f"tab {tab}: pristine clean",
                  invalid == 0, f"{invalid} inputs are red on first render")

        # No duplicate ids on routes / hostname (the bug we hit before)
        for elt_id in ("ts-routes", "ts-hostname"):
            n = spa.page.evaluate(f"document.querySelectorAll('#{elt_id}').length")
            check(results, MODULE_ID, f"id #{elt_id} unique",
                  n == 1, f"found {n} elements (should be 1)")

        # JS console clean
        check(results, MODULE_ID, "console errors",
              len(spa.console_errors) == 0,
              f"{len(spa.console_errors)} errors: {spa.console_errors[:3]}")

        # /api/status reachable — retry once with a short pause to
        # ride out any transient 401 right after the tab walk (the
        # browser context occasionally races the session cookie
        # refresh when the tab nav fires several /api/* hits at once).
        st = spa.fetch_json("/api/status")
        if not (isinstance(st, dict) and "version" in st):
            spa.page.wait_for_timeout(1500)
            st = spa.fetch_json("/api/status")
        detail = ""
        if isinstance(st, dict) and "__http_status" in st:
            detail = f"http={st.get('__http_status')} err={(st.get('__error') or '')[:80]!r}"
        else:
            detail = f"got {list(st.keys()) if isinstance(st, dict) else type(st).__name__}"
        check(results, MODULE_ID, "/api/status JSON",
              isinstance(st, dict) and "version" in st and "ap" in st and "sta" in st,
              detail)
    finally:
        spa.close()
    return results
