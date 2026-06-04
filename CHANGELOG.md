# Changelog

All notable changes to this project are documented here. The format is
based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and
this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added

- **Source-NAT advertised routes** — an opt-in toggle (Tailscale settings, off by
  default) that masquerades tunnel→LAN forwarded traffic to this device's uplink
  IP, the way Tailscale's `--snat-subnet-routes` does. With it on, advertising the
  uplink subnet makes that LAN reachable from the tailnet out of the box (the
  upstream router no longer needs a route back to the tailnet). Enabling it offers
  to add the live uplink subnet to the advertised routes.
- **Subnet prefix in the Status cards** — the Uplink and Access Point cards now
  show the address with its CIDR prefix (e.g. `192.168.1.50/24`, `192.168.4.1/24`).
- **Factory-reset dialog recommends a backup** — the confirmation now highlights
  that you should download an encrypted config backup first, with a one-click
  "Back up first" link into the backup flow.
- **Project logo in the header** — the web UI nav brand icon now uses the project
  logo instead of the generic placeholder icon.

### Changed

- **OTA version compare is pre-release-aware** — the updater now follows SemVer
  pre-release precedence (`0.1.9-beta1 < 0.1.9-beta2 < … < 0.1.9`), so a device on
  one beta is offered the next beta and the final stable release supersedes every
  beta of that version. Beta builds carry the pre-release tag in their version
  string. Previously the suffix was ignored, so successive betas of the same
  `x.y.z` were never offered.

## [0.1.8] — 2026-06-03

### Added

- **OTA beta channel** — an opt-in toggle (System → Firmware update) that makes
  the updater also offer GitHub **pre-releases** (highest semver wins), so test
  devices can pull beta builds before they go stable. Off by default, so
  production devices keep tracking only stable releases (`/releases/latest`).
  Promote a beta by publishing it as a full (non-pre-release) release.

## [0.1.7] — 2026-06-03

Reliability-focused early-access update: over-the-air updates are now
dependable, crash reports are actionable, and the Tailscale client (microlink)
is consolidated. No breaking changes — settings and tailnet identity are
preserved across the update.

### Fixed

- **OTA updates are now durable** — the running image marks itself valid after a
  healthy boot, so the bootloader rollback no longer reverts a fresh update on
  the next reboot.
- **OTA downloads from GitHub Releases succeed** — the updater's HTTP buffer was
  too small to hold the release-asset redirect header (it failed with
  `ESP_FAIL`).
- **A failed OTA no longer crashes the device** — the install path used to panic
  and reboot on any download error (risking a loop with auto-install enabled); it
  now surfaces the error and keeps running.
- **Crash signatures are debuggable** — panic reports captured only the generic
  `abort()` frames; they now record the backtrace through to the real fault
  (Diagnostics → Reset history, and telemetry).

### Changed

- **AP channel is read-only**, auto-following the uplink channel (single radio).
  Removed the non-functional manual channel control and the dead "Disable web
  interface" placeholder.
- **microlink** consolidated onto a single maintained line.
- **Telemetry hardened** — the anonymous device hash now carries an integrity
  check so the collector can drop spoofed/garbage events; the collector keeps
  only coarse country/region and never stores a raw IP.

### Known limitations

- **Headscale does not currently work** — the ts2021 control-plane Noise
  handshake fails (tracked in
  [#7](https://github.com/Csontikka/esp32-tailscale-subnet-router/issues/7));
  supersedes the "untested" note in 0.1.0. Hosted Tailscale is unaffected.
- Carried over from 0.1.0: single 2.4 GHz radio (channel realign after a roam),
  MCU-class throughput, tailnet lock unsupported.

## [0.1.0] — 2026-05-31

First public early-access release. The firmware turns one ESP32-S3 into
a WiFi NAT router and a Tailscale subnet router, configured entirely
from a built-in web UI.

### Added

- **Dual role**: simultaneous WiFi STA uplink + AP (NAPT, DHCP, DNS
  forwarder) + Tailscale subnet router.
- **Tailscale stack** via [microlink](https://github.com/Csontikka/microlink):
  DISCO discovery, direct paths with DERP relay fallback, NAT traversal,
  exit-node client and gateway, DERP-aware automatic MTU.
- **Exit-node aware routing** with fail-closed behaviour, and DERP
  re-handshake on the WireGuard data plane so an exit-node session
  survives a direct↔DERP transition.
- **ACL firewall**: four hook points (Internet↔ESP, Clients↔ESP),
  first-match-wins rules by protocol / CIDR / port / action, per-rule
  hit counters.
- **DNS forwarder** for AP clients with a PSRAM-backed response cache,
  worker pool, and cache stats.
- **Diagnostics**: on-device ping, traceroute, route-explain, a 1 MB
  download/upload speed test (cancellable), live WiFi scan, and a
  microSD "flight recorder" for control-plane stalls.
- **DHCP**: reservations, live lease table with per-client signal, MAC
  denylist; **port forwarding**.
- **Web UI**: responsive single-page admin served from the device —
  first-run password setup, WiFi join, tailnet enrolment, firewall,
  diagnostics, system.
- **Operations**: encrypted config backup/restore, OTA updates, per-sink
  (console + SD) log levels with an INFO ceiling, pre-crash log capture,
  auto AP-channel realign on STA roam.
- **Anonymous telemetry** (on by default, one-toggle opt-out): daily
  salted one-way device hash + boot/flash counters + reboot/crash cause +
  firmware/chip/uptime; no SSIDs/IPs/MACs/tailnet/peers. Fully inspectable
  in `main/telemetry.c`.
- **Project hardening**: SECURITY policy, CodeQL (C/C++ + Python),
  Dependabot, secret scanning, and a custom Sensitive Data Check.

### Known limitations

- Single 2.4 GHz radio shared by STA and AP (channel realign needed
  after an upstream roam).
- MCU-class throughput (a few Mbit/s through the tunnel).
- **Headscale is untested**; only hosted Tailscale has been validated.
- Tailnet lock is unsupported.

[Unreleased]: https://github.com/Csontikka/esp32-tailscale-subnet-router/compare/v0.1.8...HEAD
[0.1.8]: https://github.com/Csontikka/esp32-tailscale-subnet-router/releases/tag/v0.1.8
[0.1.7]: https://github.com/Csontikka/esp32-tailscale-subnet-router/releases/tag/v0.1.7
[0.1.0]: https://github.com/Csontikka/esp32-tailscale-subnet-router/releases/tag/v0.1.0
