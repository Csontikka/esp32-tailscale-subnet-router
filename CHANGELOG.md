# Changelog

All notable changes to this project are documented here. The format is
based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and
this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

Three fixes ported from [@gszigethy](https://github.com/gszigethy)'s fork ([gszigethy/esp32-tailscale-subnet-router](https://github.com/gszigethy/esp32-tailscale-subnet-router), commits `82aa72b`, `3919040`, `d1f81a6`), found while reviewing his Ethernet-uplink work; the Ethernet parts stay in the fork until there is hardware here to test them on.

### Fixed
- **Two Tailscale connect tasks could tear down and rebuild the same instance at once.** The connect task is spawned from the STA got-IP handler with nothing serialising it, so a WiFi flap inside the up-to-30 s SNTP wait started a second one; both end in `tailscale_connect()`, which destroys and re-creates the microlink instance — one destroying the handle the other was initialising through (use-after-free), or two instances with the first one's tasks and sockets leaked. A lifecycle mutex now makes connect and disconnect mutually exclusive, and redundant requests collapse to "one in flight, one queued". Reproduced and verified here with a deliberate burst of three connect requests (new test hook `POST /api/debug/ts-reconnect {"burst": N}`).
- **Long Cookie headers logged the operator out.** The session lookup read the header into 160 bytes and treated truncation as an error, so a browser that also held a couple of unrelated cookies for the same origin (reverse proxy, shared hostname) was silently unauthenticated. Now 512 bytes, truncation tolerated, and the token comparison is bounded to the cookie value and constant-time.

### Changed
- `ap_connect` / `connect_count` are `volatile`: written by WiFi event handlers, read from the web server and spin-waited on by the telemetry sender; it only worked because `vTaskDelay()` is opaque to the compiler.

Three fixes ported from [@gszigethy](https://github.com/gszigethy)'s fork ([gszigethy/esp32-tailscale-subnet-router](https://github.com/gszigethy/esp32-tailscale-subnet-router), commits `82aa72b`, `3919040`, `d1f81a6`), found while reviewing his Ethernet-uplink work; the Ethernet parts stay in the fork until there is hardware here to test them on.

### Fixed
- **Two Tailscale connect tasks could tear down and rebuild the same instance at once.** The connect task is spawned from the STA got-IP handler with nothing serialising it, so a WiFi flap inside the up-to-30 s SNTP wait started a second one; both end in `tailscale_connect()`, which destroys and re-creates the microlink instance — one destroying the handle the other was initialising through (use-after-free), or two instances with the first one's tasks and sockets leaked. A lifecycle mutex now makes connect and disconnect mutually exclusive, and redundant requests collapse to "one in flight, one queued". Reproduced and verified here with a deliberate burst of three connect requests (new test hook `POST /api/debug/ts-reconnect {"burst": N}`).
- **A reconnect could crash the device, and every reconnect leaked ~650 KB of PSRAM** (microlink). Reproduced with the new burst hook: `microlink_stop()` slept a fixed 3 s and then `microlink_destroy()` freed the instance under a coord task still inside the map long-poll (PANIC in `poll_map_update`); and `destroy` never released the long-poll accumulators nor the DERP TLS state — 5.12 → 4.27 → 3.62 → 2.97 MB free over three clean reconnects, about eight WiFi flaps from an out-of-memory router. Every microlink task now signs off before exiting, `stop` waits for all of them (bounded at 15 s, leaking the instance deliberately if one is stuck), and `destroy` frees the buffers and the TLS state. Five bursts of three and three single reconnects on the reference router: no reset, heap steady.
- **Long Cookie headers logged the operator out.** The session lookup read the header into 160 bytes and treated truncation as an error, so a browser that also held a couple of unrelated cookies for the same origin (reverse proxy, shared hostname) was silently unauthenticated. Now 512 bytes, truncation tolerated, and the token comparison is bounded to the cookie value and constant-time.

### Changed
- `ap_connect` / `connect_count` are `volatile`: written by WiFi event handlers, read from the web server and spin-waited on by the telemetry sender; it only worked because `vTaskDelay()` is opaque to the compiler.

## [0.1.24] — 2026-09-10

A subnet router that advertises its subnet out of the box, and a build-reproducibility fix. Device-tested before tagging: manual OTA, the route switch off and on with the advertised set read back from the admin API, six peers direct, an AP client through the router.

### Changed
- **The AP subnet is advertised by default.** A subnet router's reason to exist is its AP subnet, but the route list only ever held what the operator typed in — the UI merely *offered* the AP CIDR — so a freshly flashed device announced nothing until someone filled the field (the reference router itself had run that way for months, found while checking the renewed admin-API token). New *Advertise the AP subnet* switch on the Tailscale card, on by default: the AP CIDR is announced as a subnet route, computed live from the AP settings (changing the AP address needs no route edit any more), and the free-text list becomes *additional* routes. Advertising on its own moves no traffic — peers use the route only after it is approved in the admin console. Turn the switch off to announce only the listed routes.

### Build
- **`sdkconfig.defaults` now pins `CONFIG_LWIP_MAX_ACTIVE_TCP=24` and `CONFIG_LWIP_TCP_OOSEQ_MAX_PBUFS=4`.** Every release since 0.1.17 was built from a local sdkconfig that carried these two values; the tracked defaults never had them, so a fresh clone would have compiled IDF's 16 active PCBs and a derived out-of-order limit — a different firmware from the one measured on the reference router. Pinned with the rationale next to them; nothing changes for the published binaries.

## [0.1.23] — 2026-09-10

One microlink fix, found while checking the renewed admin-API token: the *Client version* setting added in 0.1.20 did not actually stick. Device-tested before tagging: manual OTA, the version visible in the admin console with the setting on and gone again with it cleared, six peers direct.

### Fixed
- **The admin console lost the client version after every reconnect.** `Hostinfo.IPNVersion` was only put into two of the four Hostinfo messages microlink sends (register, initial map); the long-poll MapRequest and the endpoint update carried a Hostinfo without it, and the control plane keeps the last Hostinfo it receives — so the value set under *Client version* showed up for a moment after registration and vanished with the first endpoint update, re-arming the console's "Device is too old" gate. All four now carry it.

## [0.1.22] — 2026-09-10

Two fixes from a morning of measuring the reference router: the exit-node download black-hole that had been on the list since the first exit-node tests, and two more places where microlink talked more than the reference client does. Device-tested before tagging: manual OTA, exit node via a peer with eight bulk downloads, an AP client through the router, six peers direct.

### Changed
- **Endpoint updates reach the control plane only when the endpoints changed** (microlink; reference client: `setEndpoints` gates the update with `endpointSetsEqual`). The 23 s re-STUN used to re-send an identical update every time — a fresh HTTP/2 stream for the node and a peer-change patch pushed to every peer on the tailnet — with nothing new in it. Still sent once per (re)connect.
- **One PONG per PING, back to where it came from** (microlink; reference client: `handlePingLocked`). The fan-out — the source, every LAN endpoint of the peer, plus always a copy via DERP — cost the pinger an unmatched PONG per extra copy and a DERP round trip per PING for nothing. DERP is used only when the direct send itself fails.

### Fixed
- **Exit-node bulk downloads no longer black-hole.** With an exit node on a direct UDP path the automatic tunnel MTU was 1420, so AP clients were MSS-clamped to 1380 and servers sent 1368-byte segments — which the exit node cannot forward into its own tunnel (every Tailscale peer's tun device is 1280). The exit node answered with ICMP "fragmentation needed"; servers that honour it recovered, servers that ignore it retransmitted the same oversized segment until the client gave up: measured through the reference router, one 5 MB download in four never delivered a byte in 90 s, deterministically per server. Auto MTU is now 1280 unconditionally (the Tailscale tunnel MTU, direct or relayed alike), so the MSS clamp is 1240 and nothing on the return path ever needs fragmenting. The *Fixed* MTU mode is unchanged.

## [0.1.21] — 2026-09-10

microlink brought up to the esphome-tailscale line again ([esphome-tailscale#46](https://github.com/Csontikka/esphome-tailscale/issues/46), the protocol direction): the DISCO manager backs off the way `tailscaled` does, the per-packet crypto cost is gone, and a dual-homed peer no longer re-handshakes every 3 s. Device-tested on the reference router before tagging: manual OTA, exit node via a peer, an AP client through the router, six peers direct in every phase, and the night's soak on this code.

### Changed
- **DISCO probing backs off the way `tailscaled` does.** A CallMeMaybe is no longer answered with a CallMeMaybe — the reference client never does that; the echo kept two microlink nodes without a WireGuard session bouncing CallMeMaybe → ping burst → CallMeMaybe indefinitely, the storm behind esphome-tailscale#46. The 3 s heartbeat runs only behind a live WireGuard session (a session-less peer is re-probed once a minute); peers the netmap marks offline get neither the 15 s upgrade probe nor the 30 s DERP handshake retry; CallMeMaybe-triggered bursts are spaced at least 2.5 s apart per peer. On this router (13 peers, 8 of them offline) the probes to offline peers alone had kept every 1 s manager tick above its 30 ms `SLOW` mark.
- **One X25519 per peer, not per DISCO packet.** Every DISCO ping, pong and CallMeMaybe — sent or received — recomputed the NaCl box shared secret, ~16 ms each on an ESP32-S3. It is now derived once per peer and cached (reference `discoInfo.sharedKey`), and packets from an unknown disco key are dropped before any crypto. `disco_periodic_probes SLOW` warnings on the reference router: 46–62 per minute → 0.

### Fixed
- **A dual-homed peer no longer forces a WireGuard handshake every 3 s.** A peer reachable both on its LAN address and through a NAT port-forward had DISCO's best path on one address while its WireGuard packets arrived from the other; wireguardif roamed the endpoint back on every reply, the next heartbeat "switched" it again and, with no recent data, forced a handshake — one per 3 s, indefinitely (13 switches per 45 s measured with the console at INFO). The best is now kept while it answered within the last 6.5 s (`trustUDPAddrDuration`), and an endpoint change never forces a handshake: WireGuard authenticates by key and roams by design. After the fix: 0 switches, only the normal 120 s rekeys remain.

## [0.1.20] — 2026-09-10

microlink brought up to the esphome-tailscale line (four ports plus this
week's fixes), the standalone firmware's own diagnostics tightened, and a
long-standing exit-node foot-gun closed. Device-tested on the reference
router before tagging: manual OTA, an AP client through the router, exit
node via a peer, and a same-window A/B against 0.1.19.

### Added
- **Client version (`Hostinfo.IPNVersion`)** — new *Client version* field on the Tailscale card (NVS `ts_ipn_ver`, API `settings.ipn_version`). The Tailscale admin console gates some device operations on the reported client version and shows "Device is too old" when it is empty; set e.g. `1.98.9` to clear that (the `version.Long()` hash suffix is appended automatically). Empty by default — a public client should not claim a version it is not. Port of esphome-tailscale#39 by @timmills.
- **Netmap-buffer allocation failures are explicit.** All four control-plane allocation sites log the requested size against free PSRAM/internal memory and the largest free block instead of failing silently into "MapRequest failed, will retry" (found via esphome-tailscale#45 on a 2 MB-PSRAM board).
- **One-line DISCO summary** every 10 s while inbound discovery traffic flows (`DISCO: N packets in 10 s, M iterations budget-capped`), replacing per-packet chatter.

### Changed
- **The DISCO/WireGuard manager budgets its work per iteration** (8 DISCO / 40 ms; each WireGuard drain 64 packets / 30 ms in its own window) and always reaches its 10 ms sleep, so a peer that keeps probing can no longer monopolise the core. Per-packet DISCO lines moved to DEBUG; `DISCO PONG unmatched` / `probe table full` rate-limited to one line per 10 s. In the same-window A/B under a live DISCO loop this gave a 3× better on-device download and half the AP-client latency versus 0.1.19 (esphome-tailscale#46).
- **`/key` sends the real capability version** (`ML_CTRL_PROTOCOL_VER`, 131) instead of a hardcoded 88. Headscale ≥ 0.29 rejects 88 as an unsupported client at the very first step; SaaS accepted both.

### Fixed
- **Refused registrations now say why** — `RegisterResponse.Error`, `NodeKeyExpired` and `AuthURL` are acted on like the reference client does; `User.ID`/`DisplayName` are display-only (auth-key and tag-owned nodes legitimately have no user). Port of esphome-tailscale#38 by @timmills.
- **Peers removed by the control plane disappear immediately** — `PeersRemoved` carries NodeIDs; the handler expected nodekey strings, so a deleted node lingered (probed, DISCO-pinged) until the next full netmap. Port of esphome-tailscale#42.
- **Selecting an exit node no longer black-holes AP clients until the next restart.** Saving `exit_node_ip` flipped the live default route to the tunnel within seconds, while microlink only learns the exit node when it (re)starts — so every AP client lost the internet until the operator restarted (measured on 0.1.19: public IP unreachable, a 5 MB download stuck at 0 bytes for 90 s). The save now persists the choice only, as its `restart_required` response always implied; *Settings* shows the saved value (plus `exit_node_restart_pending`), *Status* the live one, and the route follows on restart.
- **Stale crash signatures are no longer re-sent.** A BROWNOUT or watchdog reset produces no coredump, yet the telemetry gate treated it as a crash and re-sent the *previous* panic's signature from NVS, so the fleet view showed phantom repeat crashes. The signature is now erased on any boot without a fresh coredump — it lives for exactly the one boot after the panic.

## [0.1.19] — 2026-08-15

Stable release. Promotes the 0.1.19-beta1 DERP-liveness work to a stable
build after an eight-day beta bake; **no functional changes versus
0.1.19-beta1** (the only delta is the version string).

Bake evidence: the reference device ran 168 hours without a single reboot
or crash, during which the new control-plane map-stream watchdog caught and
self-healed **eight** silently dead map sessions — roughly one per day, each
one an outage that earlier firmware would have ridden out as a wedged
control plane until someone power-cycled the board. A second device on a
different continent ran the same build with no faults reported.

## [0.1.19-beta1] — 2026-08-07

**Beta pre-release** — DERP relay liveness. Ports the esphome-tailscale
v0.5.5 fix bundle and the HTTP/2 reassembly series into the microlink
submodule (reported and analyzed by timmills in esphome-tailscale#31/#32/#33,
bench-verified there), plus two earlier fixes those trees had and ours
didn't.

### Fixed

- **A dead DERP relay connection now recovers on its own.** Previously 3
  failed connect attempts parked the relay task forever with nothing left to
  re-arm it — the device stayed "connected" while relay-only peers could
  never reach it again. Now: endless exponential backoff (5 s → 60 s cap),
  a 90 s RX-liveness watchdog on a "connected" relay, and every connect
  failure path tears the TLS context down completely (each leaked ~17 KB
  before, eventually killing all later handshakes). (microlink submodule.)
- **ACL revocation reaches the device.** A full `Peers` list in a
  MapResponse is now authoritative — table entries omitted from it are
  swept, and peer removal also drops the NVS boot-cache entry, so revoked
  peers stay gone across reboots instead of resurrecting.
- **Large MapResponses no longer corrupt the long-poll stream.** HTTP/2
  frames that span a read boundary are reassembled instead of dropped
  (a big netmap deterministically desynced the stream parser).
- **Control-plane map-stream watchdog** (silent mapSession death behind a
  live HTTP/2 front end → automatic re-register within ~5 min) and the
  register-after-stream-read ordering fix — catch-up ports of
  esphome-tailscale v0.5.4/v0.5.2 fixes that predate this repo's submodule
  pin.
- `DISCO decrypt failed` now names the claimed sender and arrival path.

### Added

- **Per-cause reconnect counters in telemetry** (`rcs`/`rct`/`rcd`/`rcr`):
  map-stream watchdog fires, coord transport deaths, DERP RX-watchdog
  fires, and failed DERP connect attempts — so the fleet can distinguish
  watchdog saves from ordinary transport flaps (a single combined connect
  count overcounts).

### Known limitation

- The opt-in netcheck home-DERP override (`tailscale_netcheck_override`,
  default OFF) still announces the *configured* region in
  `NetInfo.PreferredDERP` while connecting to the measured one — peers
  would dial the announced region. Leave the override OFF (the default)
  until the announce-before-move rework (esphome-tailscale#36) lands
  upstream and gets ported.

## [0.1.18] — 2026-08-06

Stable release. Promotes the 0.1.18-beta1 changes (end-to-end Headscale
support, netmap via the streaming long-poll, discoverable first-boot AP) to a
stable build after a five-week beta bake with no regressions reported; no
functional changes versus 0.1.18-beta1. Also folds in the GCC 14 build fix
(#10): the false-positive `-Wmaybe-uninitialized` in ESP-IDF's
`esp_driver_i2c` is downgraded to non-fatal for that one component, so fresh
builds succeed on hosts whose toolchain trips it.

## [0.1.18-beta1] — 2026-07-03

### Added

- **Headscale support works end-to-end.** (#7) The login-server field now
  accepts `host`, `host:port`, `http://host[:port]` and `https://host[:port]`
  (TLS via the ESP-IDF public-CA bundle — Let's Encrypt works, self-signed
  does not), and the device fetches the control plane's Noise public key from
  `/key?v=88` instead of assuming the hosted-Tailscale key, so the ts2021
  machine-key handshake succeeds against Headscale. Validated live against
  Headscale v0.28.0 over both plain HTTP (`http://host:8080`) and HTTPS
  behind a Let's Encrypt proxy; the hosted-Tailscale path re-validated over
  both plain TCP and `https://controlplane.tailscale.com`. (microlink
  submodule; ports esphome-tailscale `5ecc5ee` + `9be7d81`.)

### Fixed

- **Netmap arrives via the streaming long-poll (Headscale ≥ 0.26).**
  Headscale only delivers the full netmap on a streaming map request — the
  old one-shot fetch got an empty response and reconnect-looped ("Empty
  MapResponse"). The initial netmap is now consumed from the long-poll
  stream; the hosted-Tailscale flow is unchanged.
- **First-boot AP is discoverable.** (#9) Default SSID renamed `myssid` →
  `ESP32-TSR-Setup`, the README now documents the default credentials and
  `http://192.168.4.1`, and boot prints the AP name + web-UI URL on serial
  at the default log level.

## [0.1.17] — 2026-06-11

Stable release. Promotes the 0.1.16 fixes (exit-node TAI64N handshake timestamps,
DNS-relay task-stack hardening, OTA beta-channel JSON buffer) to a stable build;
no functional changes versus 0.1.16.

## [0.1.16] — 2026-06-11

This release folds in the fixes that had only been serial-flashed since 0.1.12
(0.1.13–0.1.15 were test builds, never published).

### Fixed

- **Exit node now establishes itself reliably (WireGuard handshake timestamps).**
  The handshake's TAI64N timestamp is now sampled from the SNTP wall clock on every
  emit, instead of a per-boot counter that could fall behind the value a peer had
  already stored and get every initiation silently dropped as a replay. The ESP now
  brings up an exit-node tunnel on its own and self-heals across reboots, so AP
  clients keep their internet through the exit node. (microlink submodule.)
- **DNS relay no longer crash-loops at boot under verbose logging.** The relay
  listener/worker task stacks were enlarged (to 8 KB) so enabling INFO-level SD
  recording at boot can no longer overflow them into a panic loop.
- **Firmware-update check on the beta channel no longer fails to parse.** The
  `/releases` JSON the beta channel scans had grown past the 32 KB download buffer
  and was being truncated, so "Check now" reported `parse failed (no firmware.bin
  asset?)` even though every release ships the asset. The buffer is now 128 KB.

## [0.1.9] — 2026-06-04

### Added

- **Source-NAT advertised routes** — an opt-in toggle (Tailscale settings, off by
  default) that masquerades tunnel→LAN forwarded traffic to this device's uplink
  IP, the way Tailscale's `--snat-subnet-routes` does. With it on, advertising the
  uplink subnet makes that LAN reachable from the tailnet out of the box (the
  upstream router no longer needs a route back to the tailnet). Enabling it offers
  to add the live uplink subnet to the advertised routes.
- **Subnet prefix in the Status cards** — the Uplink and Access Point cards now
  show the address with its CIDR prefix (e.g. `192.168.4.1/24`, `192.168.32.1/24`).
- **Factory-reset dialog recommends a backup** — the confirmation now highlights
  that you should download an encrypted config backup first, with a one-click
  "Back up first" link into the backup flow.
- **Project logo in the header** — the web UI nav brand icon now uses an inline
  chip + WiFi mark (crisp vector, stays sharp at small sizes) instead of the
  generic placeholder icon.
- **Mascot in the Support card** — a small hand-drawn mascot next to the
  "Buy me a coffee" button, centred with it.

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

[Unreleased]: https://github.com/Csontikka/esp32-tailscale-subnet-router/compare/v0.1.9...HEAD
[0.1.9]: https://github.com/Csontikka/esp32-tailscale-subnet-router/releases/tag/v0.1.9
[0.1.8]: https://github.com/Csontikka/esp32-tailscale-subnet-router/releases/tag/v0.1.8
[0.1.7]: https://github.com/Csontikka/esp32-tailscale-subnet-router/releases/tag/v0.1.7
[0.1.0]: https://github.com/Csontikka/esp32-tailscale-subnet-router/releases/tag/v0.1.0
