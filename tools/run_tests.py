#!/usr/bin/env python3
"""Functional-test runner for esp32-tailscale-subnet-router.

Usage:
    python tools/run_tests.py                   # run every module
    python tools/run_tests.py --only spa,pi     # run a subset
    python tools/run_tests.py --list            # list available modules
    TEST_QUICK=1 python tools/run_tests.py      # skip slow toggles

See docs/TESTING.md for the env-var configuration the modules expect.
"""
from __future__ import annotations
import argparse, importlib, sys, time

# UTF-8 stdout — Windows defaults to cp1252 and mangles em-dashes etc.
try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")  # type: ignore[attr-defined]
    sys.stderr.reconfigure(encoding="utf-8", errors="replace")  # type: ignore[attr-defined]
except Exception:
    pass

from test_lib.common import Context, Result, render_summary

# (module_id, dotted-import) — order = run order
MODULES = [
    ("spa",       "test_lib.spa"),
    ("network",   "test_lib.network"),
    ("firewall",  "test_lib.firewall"),
    ("tailscale", "test_lib.tailscale"),
    ("pi",        "test_lib.pi"),
    ("routing",   "test_lib.routing"),
]


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--only",  help="comma-separated module list (default: all)")
    ap.add_argument("--list",  action="store_true", help="list modules and exit")
    args = ap.parse_args()

    if args.list:
        for mid, dotted in MODULES:
            try:
                m = importlib.import_module(dotted)
                desc = getattr(m, "MODULE_DESC", "")
            except Exception as e:
                desc = f"(import failed: {e!r})"
            print(f"  {mid:<10}  {desc}")
        return 0

    requested = set((args.only or ",".join(m for m, _ in MODULES)).split(","))
    ctx = Context.from_env()
    results: list[Result] = []
    t0 = time.time()

    for mid, dotted in MODULES:
        if mid not in requested:
            continue
        print(f"\n--- module: {mid}")
        try:
            mod = importlib.import_module(dotted)
            rs  = mod.run(ctx)
        except Exception as e:
            rs = [Result(module=mid, name="module crashed", ok=False, detail=repr(e))]
        for r in rs:
            results.append(r)
            print("  " + r.render())

    print(render_summary(results))
    print(f"elapsed: {time.time()-t0:.1f}s")
    return 0 if all(r.ok for r in results) else 1


if __name__ == "__main__":
    sys.exit(main())
