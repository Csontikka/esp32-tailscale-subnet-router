"""Functional-test catalog for esp32-tailscale-subnet-router.

Each module under this package exposes:
    MODULE_ID  = "<short-name>"
    MODULE_DESC = "<one-line summary>"
    def run(ctx) -> list[Result]

Modules are wired into tools/run_tests.py; see docs/TESTING.md.
"""
