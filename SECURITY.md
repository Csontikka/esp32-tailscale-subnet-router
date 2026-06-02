# Security Policy

## Reporting a Vulnerability

If you think you've found a security issue in this firmware — for example a
memory-corruption bug in the C code, a way to leak the web-UI password or the
Tailscale auth key / WireGuard keys out of NVS, an ACL/firewall bypass, a
web-UI authentication bypass, or any other concern that affects the safety of a
device running this code — please report it privately rather than opening a
public issue.

**Preferred channel:** use GitHub's [private vulnerability reporting](https://github.com/Csontikka/esp32-tailscale-subnet-router/security/advisories/new)
on this repository. This creates a private security advisory that only the
maintainer and invited collaborators can see.

If that isn't available to you, you can also email the maintainer directly (see
the GitHub profile at [@Csontikka](https://github.com/Csontikka)).

Please include:

- A description of the issue and why you think it's a security problem.
- The exact commit / version of the firmware you observed it on.
- Steps to reproduce, if possible — ideally the relevant web-UI / `/api/*`
  configuration or serial-console input and a description of the runtime
  behavior.
- Any logs, crash dumps (coredump), or serial output that illustrates the
  problem.

I will acknowledge the report within a few days, work with you to confirm the
issue, and coordinate a fix and disclosure timeline.

## Scope

This repository is the **ESP-IDF firmware** for the router (AP/STA NAT, web UI,
ACL firewall, DHCP/DNS, etc.). The Tailscale-compatible WireGuard userspace
stack lives in the [microlink](https://github.com/Csontikka/microlink)
submodule — if the vulnerability is in the DISCO/DERP/magicsock/WireGuard
protocol handling itself, please consider reporting it there as well, since the
fix will likely need to land in that project first.

## Supported Versions

This project is under **heavy development**. Only the current `main` branch is
supported for security fixes. Older commits and unreleased snapshots are not
maintained.

## History rewrite — 2026-06-01

The git history of this repository was rewritten on 2026-06-01 to remove
development-environment data that had been committed in earlier code comments,
a deleted helper script, and test utilities:

- the maintainer's real Tailscale **tailnet name** and a development **WiFi
  SSID** (replaced with generic placeholders);
- development-network **IP ranges** (replaced with generic RFC1918 examples);
- a development **admin password** that appeared in a since-deleted helper
  script (`tools/verify_routes.py`) — replaced with `REDACTED`.

That password protected only a local development board, but since it was once
public it must be considered **burned** and should be **rotated** on any
affected device. No Tailscale auth keys, WireGuard keys, API tokens, or private
keys were ever committed (those have always been git-ignored).

Because history was rewritten, commit hashes prior to this date differ from any
older clone or fork. Re-clone rather than pull if you have an old copy. The
rewrite is marked by the annotated tag `history-scrub-2026-06-01`.

## Non-affiliation notice

This project is **not** affiliated with, sponsored by, or endorsed by Tailscale
Inc., Jason A. Donenfeld, or the WireGuard project. Please do not report
Tailscale-service or WireGuard-protocol vulnerabilities here — report those to
the respective upstream projects.
