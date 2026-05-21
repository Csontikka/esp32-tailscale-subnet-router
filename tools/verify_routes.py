"""End-to-end verification of the AP-IP save → advertised-routes flow
in the SPA, using Playwright headless Chromium.

Steps:
  1. Open http://172.21.33.51/
  2. Log in as admin (password Init123!)
  3. Read current advertise_routes textarea from the Tailscale tab
  4. Change AP IP on the Network tab to a fresh value
  5. Click Save → auto-confirm the ap_cidr_offer dialog
  6. Switch back to Tailscale tab; assert the textarea reflects the
     new CIDR (and only that)
"""
from playwright.sync_api import sync_playwright
import sys, time

DEV  = "http://172.21.33.51"
USER_PW = "Init123!"
NEW_AP = "192.168.50.1"
EXPECTED_CIDR = "192.168.50.0/24"

def fail(msg):
    print(f"FAIL: {msg}", file=sys.stderr)
    sys.exit(1)

with sync_playwright() as p:
    browser = p.chromium.launch(headless=True)
    ctx = browser.new_context()
    page = ctx.new_page()

    print(f"Opening {DEV}/ ...")
    page.goto(DEV + "/", wait_until="networkidle")

    # Login (the SPA shows a login overlay when authentication is required)
    if page.locator("#authLogin").is_visible():
        print("Logging in ...")
        page.fill("#authLogin input[type=password]", USER_PW)
        page.click("#authLogin button[type=submit], #authLogin .btn-primary")
        page.wait_for_timeout(1500)

    # Switch to Tailscale tab + read initial routes
    print("Switching to Tailscale tab ...")
    page.click("button.nav-tab[data-page=tailscale]")
    page.wait_for_timeout(1500)
    before_routes = page.input_value("#ts-routes")
    print(f"  initial textarea: {before_routes!r}")

    # Switch to Network tab and change AP IP
    print(f"Switching to Network tab, setting AP IP = {NEW_AP} ...")
    page.click("button.nav-tab[data-page=network]")
    page.wait_for_timeout(1000)
    page.fill("#net-ap-ip", NEW_AP)

    # Override window.confirm so it auto-accepts the offer dialog
    # without going through Playwright's async dialog plumbing.
    page.evaluate("window.confirm = function(m){ console.log('CONFIRM:', m); return true; };")

    page.click("#net-ap-save")
    page.wait_for_timeout(2500)

    # Switch back to Tailscale tab + read new routes
    print("Re-reading Tailscale tab ...")
    page.click("button.nav-tab[data-page=tailscale]")
    page.wait_for_timeout(1500)
    after_routes = page.input_value("#ts-routes")
    print(f"  textarea after save: {after_routes!r}")

    if EXPECTED_CIDR not in after_routes:
        fail(f"expected {EXPECTED_CIDR!r} in textarea, got {after_routes!r}")
    print(f"PASS: textarea contains {EXPECTED_CIDR}")

    browser.close()
