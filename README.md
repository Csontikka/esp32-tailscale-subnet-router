<div align="center">

<img src="assets/logo/banner.png" alt="Tailscale Subnet Router for ESP32-S3" width="100%">

# ESP32 Tailscale Subnet Router

**A pocket-sized WiFi NAT router *and* Tailscale subnet router on a single ESP32-S3 — built to put low-bandwidth IoT devices on your tailnet, no extra hardware, configured entirely from a built-in web UI.**

[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
[![Platform: ESP32-S3](https://img.shields.io/badge/platform-ESP32--S3-7c3aed.svg)](#hardware)
[![ESP-IDF](https://img.shields.io/badge/ESP--IDF-5.5%2B-e7352c.svg)](https://docs.espressif.com/projects/esp-idf/)
[![CodeQL](https://github.com/Csontikka/esp32-tailscale-subnet-router/actions/workflows/codeql.yml/badge.svg)](https://github.com/Csontikka/esp32-tailscale-subnet-router/actions/workflows/codeql.yml)
[![Buy Me a Coffee](https://img.shields.io/badge/Buy%20me%20a%20coffee-donate-yellow.svg?style=plastic)](https://buymeacoffee.com/csontikka)

</div>

---

> **Status — early access (`v0.1.9`).** Runs daily on the reference
> ESP32-S3 hardware and the core paths (WiFi NAT, Tailscale subnet
> routing, DERP fallback, exit nodes, firewall) are exercised
> continuously. Treat it as a capable hobby build, not a hardened
> appliance — see [Known limitations](#known-limitations).

## What it is

This firmware turns one ESP32-S3 board into two things at once:

1. **A WiFi NAT router.** It joins your existing 2.4 GHz network as a
   client (STA) and re-broadcasts its own access point (AP). The IoT
   devices on that AP reach the internet through the upstream WiFi, with
   NAPT, DHCP, and an on-board DNS forwarder.
2. **A Tailscale subnet router.** It runs a userspace WireGuard +
   Tailscale (`ts2021`) stack, so any peer on your tailnet can reach the
   devices behind its AP — and the AP-side devices can use the ESP as a
   Tailscale **exit node** gateway. No client software on the IoT
   devices, no cloud account on the LAN.

Everything — WiFi credentials, the tailnet auth key, advertised routes,
firewall rules, diagnostics — is configured from a phone or laptop
browser. There is no app and no serial console required after the first
flash.

<div align="center">
<img src="docs/images/status_v3.png" alt="Web UI — Status dashboard" width="88%">
<br><em>The Status dashboard: uplink, access point, Tailscale node and peers at a glance.</em>
</div>

## Why

Most "put a sensor on Tailscale" setups need either a Raspberry Pi
acting as a subnet router or per-device Tailscale clients. This project
collapses that into a ~$10 board you can leave plugged into a USB
charger: it bridges a whole IoT subnet onto your tailnet *and* gives
those devices internet through an exit node, while staying small enough
to ignore.

## What it's for — and what it isn't

This is a **micro-controller** doing userspace encryption and NAT on a
single shared 2.4 GHz radio. Its job is **reach, not throughput** — size
your expectations accordingly.

**✅ What it's for**

- IoT / home-automation gear: **sensors, smart switches, plugs,
  thermostats**, energy/environmental monitors, ESPHome / Zigbee / MQTT
  bridges — anything small and low-bandwidth.
- Low-rate control & telemetry: bursty, tiny payloads that are perfectly
  happy with around a megabit.
- Reaching a device stuck behind NAT/CGNAT so you (or Home Assistant) can
  poll it, flip a relay, or SSH in from anywhere on your tailnet.

**🚫 What it's not for**

- Being the everyday internet uplink for your **phone or laptop**.
- **Streaming, video calls, or watching a camera feed in high resolution.**
- Large downloads, backups, OTA images for other devices — anything
  bandwidth-heavy.
- A general-purpose VPN gateway for fast clients.

Real-world throughput through the tunnel runs **roughly 0.3–1.4 Mbit/s**,
depending on the path — plain STA routing is at the top of that range, a
**direct** exit node in the middle, and a **DERP-relayed** exit node at
the bottom. Plenty for switches and sensors, not for media. If you need
real bandwidth, put a Raspberry Pi (or similar) on that job instead. *(Configuring the device from a
phone or laptop browser is of course fine — that's just the admin UI, not
traffic you route through it.)*

## Features

- **Dual role** — simultaneous WiFi STA (uplink) + AP (NAT router) +
  Tailscale subnet router.
- **Expose the upstream LAN too** — beyond its own AP subnet, an optional
  Source-NAT (à la Tailscale `--snat-subnet-routes`) lets tailnet peers reach
  the network the device is *connected to*, with no static route needed on the
  upstream router.
- **Web UI for everything** — first-run password setup, WiFi join,
  tailnet enrolment, routes, firewall, diagnostics. Dark, responsive,
  single-page; served straight off the device.
- **Tailscale, the real protocol** — DISCO peer discovery, direct paths
  *and* DERP relay fallback, NAT traversal, MagicDNS-aware, exit-node
  client and gateway. Powered by [microlink](https://github.com/Csontikka/microlink).
- **Exit-node aware routing** — AP clients' internet traffic can be
  forced through a chosen Tailscale exit node; when the exit node is
  unreachable the firmware **fails closed** (traffic stops) rather than
  silently leaking to the local uplink.
- **Stateful-ish ACL firewall** — four hook points (Internet↔ESP,
  Clients↔ESP) with first-match-wins rules by protocol / CIDR / port /
  action, plus per-rule hit counters.
- **DNS forwarder with cache** — on-board resolver for AP clients with a
  PSRAM-backed response cache and configurable upstream.
- **Operations toolbox** — on-device ping / traceroute / route-explain,
  a 1 MB download/upload speed test, live WiFi scan, and a
  microSD "flight recorder" for catching control-plane stalls.
- **DHCP niceties** — reservations, live lease table, per-client signal,
  and a MAC denylist.
- **Robust by design** — encrypted config backup/restore, OTA updates
  (with an opt-in beta channel for pre-releases), per-sink (console + SD)
  log levels, auto AP-channel realign on STA roam, and pre-crash log capture.
- **Anonymous telemetry (on by default, one toggle to opt out)** — a tiny
  daily payload: a salted one-way device hash + boot/flash counters +
  firmware/chip/uptime + reboot/crash cause. Never SSIDs, IPs, MACs,
  tailnet, or peers — and fully inspectable in `main/telemetry.c`.

## Hardware

| | |
|---|---|
| **Target** | ESP32-S3 with PSRAM (8 MB octal, 80 MHz) |
| **Reference board** | ESP32-S3-DevKitC-1 **N16R8** (16 MB flash / 8 MB PSRAM) |
| **Radio** | Single 2.4 GHz — STA and AP share one radio (see [limitations](#known-limitations)) |
| **Storage (optional)** | microSD for the log flight-recorder |
| **Power** | USB-C; ~real-world draw of a small dev board |

**Only the ESP32-S3 is supported.** It's the board this firmware is
written for and tested on, and it's the only one I have. The PlatformIO
config still lists a few other targets (`esp32`, `esp32-c3`,
`wt32-eth01`) left over from earlier scaffolding, but I don't build or
test against them and have no idea whether they work — so I can't support
them. This is a free hobby project and I'm not planning to buy extra
boards just to validate other hardware. If you get it running elsewhere,
great — but you're on your own there, and PRs are welcome.

## Quick start

### 1. Build & flash

```bash
git clone --recurse-submodules https://github.com/Csontikka/esp32-tailscale-subnet-router
cd esp32-tailscale-subnet-router

# PlatformIO (recommended)
pio run -e esp32-s3 -t upload

# …or ESP-IDF (>= 5.5.3)
idf.py set-target esp32s3
idf.py build flash monitor
```

> The web assets are embedded into the firmware at build time from
> `main/index.html`, so a single flash carries the whole UI.

### 2. First-time setup

On first boot the device brings up its access point. Connect to it, open
the device IP in a browser, and set an admin password.

<div align="center">
<img src="docs/images/login.png" alt="First-run admin password" width="46%">
</div>

### 3. Join your WiFi

On **Network → Access Point / Uplink networks**, point the device at your
existing 2.4 GHz network and (optionally) rename the AP it broadcasts.

<div align="center">
<img src="docs/images/network-ap_v2.png" alt="Access Point configuration" width="80%">
</div>

### 4. Enrol on your tailnet

This is the one step with a couple of non-obvious Tailscale details — do
them once and the device stays on your tailnet for good.

#### 4a · Create a Tailscale auth key

Log in to the [Tailscale admin console](https://login.tailscale.com/admin/settings/keys)
→ **Settings → Keys** and click **Generate auth key…**.

<div align="center">
<img src="docs/images/tailscale-keys-page.png" alt="Tailscale admin — Keys page" width="80%">
</div>

Fill in the dialog:

<div align="center">
<img src="docs/images/tailscale-auth-key-create.png" alt="Generate auth key dialog" width="70%">
</div>

| Option | Value | Why |
|---|---|---|
| **Description** | `esp32-router` | so you can find it later |
| **Reusable** | ✅ On | re-flash without regenerating a key |
| **Ephemeral** | ❌ **Off** | ephemeral nodes get garbage-collected when offline — bad for a device that reboots |
| **Pre-approved** | ✅ On *(if your tailnet uses device approval)* | lets the device join without a manual click |
| **Tags** | `tag:esp32` *(optional)* | handy for ACL targeting |
| **Expiration** | 90 days *(max)* | Tailscale caps this — you make the node **permanent** in 4c below |

Copy the key (it starts with `tskey-auth-…`).

#### 4b · Paste it into the device

On the device's **Tailscale** tab, paste the auth key, set a **hostname**,
and list the **subnet(s) to advertise** (your AP subnet is offered
automatically). Pick an **exit node** here too if you want AP clients to
egress through it. Save — the device registers with your tailnet on its
next connect.

<div align="center">
<img src="docs/images/tailscale_v2.png" alt="Tailscale configuration and peers" width="88%">
</div>

> **Approve the route.** A newly advertised subnet shows up in the Tailscale
> admin (Machines → your device → **Edit route settings**) and must be
> **approved** before peers can use it. And if you later change the AP subnet,
> re-approve the new route there — the old approval stays but no longer matches,
> so the subnet silently becomes unreachable until you do.

> **Reaching the *uplink* LAN (not just the AP subnet).** To expose the network
> the device is connected to (its STA/uplink side), advertise that subnet too and
> turn on **Source-NAT advertised routes**. That masquerades tunnel→LAN traffic to
> the device's own uplink IP (Tailscale's `--snat-subnet-routes` default), so
> upstream hosts can reply without a route back to the tailnet. Without it, the
> upstream router would need a static route (`100.64.0.0/10 → this device`).

> **🔑 Auth key vs. node key — read this once**
>
> - The **auth key** (`tskey-auth-…`) is a *one-time ticket*: the device uses
>   it only on first registration. After that it has its own private **node
>   key** (stored in NVS) and no longer needs the auth key — so it's fine if
>   the auth key later expires.
> - The **node key** is the device's long-term identity, and Tailscale expires
>   it after ~180 days by default. When it expires the device drops off the
>   tailnet — exactly what you *don't* want on an unattended sensor.
>
> So once the device shows up in your tailnet, **disable its node-key expiry**
> (next step). Skip it and everything looks fine for months, then the device
> silently falls off and you won't know why. Do it for every device you flash.

#### 4c · Disable node-key expiry (do this — always)

1. Open the [Tailscale **Machines** page](https://login.tailscale.com/admin/machines).
2. Find the new `esp32-router` entry.
3. Click the `⋯` menu → **Disable key expiry**.

<div align="center">
<img src="docs/images/tailscale-disable-key-expiry.png" alt="Tailscale admin — Disable key expiry" width="80%">
</div>

4. **Reboot the device** (Reboot on the **System** tab, or power-cycle). The
   expiry status is only re-fetched on a fresh control-plane login, so a plain
   reconnect isn't enough.

That's it — remote tailnet peers can now reach the IoT devices on the AP
subnet, and those devices can use the tailnet (and any exit node you picked).

## Web UI tour

The single-page UI has six sections:

| Section | What's there |
|---|---|
| **Status** | Uplink, AP, Tailscale node + peer list, memory, uptime |
| **Network** | Uplink networks, AP (SSID/IP/DNS), DHCP reservations & leases, MAC denylist, port forwarding |
| **Tailscale** | Auth key, hostname, advertised routes, exit node, MTU, peer table |
| **Firewall** | The four ACL chains, rule editor, hit counters |
| **Diagnostics** | Route-explain, ping, traceroute, speed test, WiFi scan, live + SD logs |
| **System** | Device name, firmware/OTA, SD-card logging, backup, danger zone, About (telemetry toggle) |

### Firewall / ACL

Four chains, evaluated first-match-wins; an empty chain allows by
default. Rules match on protocol, source/destination CIDR, ports, and
action, with live hit counters.

<div align="center">
<img src="docs/images/firewall_v2.png" alt="Firewall — four ACL chains with example rules" width="88%">
</div>

| Chain | Direction |
|---|---|
| `TO_ESP` | Internet → ESP |
| `FROM_ESP` | ESP → Internet |
| `TO_AP` | Clients → ESP |
| `FROM_AP` | ESP → Clients |

### Diagnostics

Route-explain answers "where would a packet to *X* actually go —
uplink, WireGuard, or DERP?", which is the fastest way to reason about
exit-node and subnet routing.

<div align="center">
<img src="docs/images/tools-route.png" alt="Route explain" width="80%">
<br>
<img src="docs/images/tools-ping.png" alt="On-device ping" width="80%">
</div>

## How it works

```
        Internet
           │  (upstream 2.4 GHz WiFi, STA)
     ┌─────┴─────┐
     │  ESP32-S3 │  NAPT + DHCP + DNS forwarder
     │  ┌──────┐ │  userspace WireGuard + Tailscale (microlink)
     │  │ ACL  │ │  exit-node aware route hook
     └─────┬─────┘
       AP  │  (2.4 GHz, 192.168.x.0/24 advertised to the tailnet)
   ┌───────┼────────┐
 sensor  switch  thermostat   ←→  reachable from any tailnet peer
```

> The AP is for **IoT gear** — sensors, switches, low-bandwidth control —
> not for routing your phone's or laptop's everyday internet. See
> [What it's for](#what-its-for--and-what-it-isnt).

- **Data plane vs control plane.** WireGuard moves packets; Tailscale's
  DISCO/`ts2021` control plane decides *how* (direct UDP vs DERP relay).
  The firmware watches the WireGuard data plane and re-handshakes over
  DERP when a direct path dies, so an exit-node session survives a
  direct↔DERP transition without dropping.
- **Exit-node routing.** A route hook forces AP-client public traffic
  into the WireGuard tunnel when an exit node is set; CGNAT (`100.64/10`)
  always goes to the tunnel. If the exit node is down, traffic stops —
  it is **not** silently rerouted to the local uplink.
- **microlink.** The Tailscale-compatible stack lives in its own repo,
  [Csontikka/microlink](https://github.com/Csontikka/microlink) — itself
  based on the original [CamM2325/microlink](https://github.com/CamM2325/microlink) —
  attached here as a git submodule and pinned to the integration commit.
  Its `docs/ARCHITECTURE.md` and `docs/TAILSCALE_REFERENCE.md` go deeper.

See [`docs/CONFIGURATION.md`](docs/CONFIGURATION.md) for a field-by-field
configuration reference.

## Tailscale & Headscale

The device authenticates with a standard **Tailscale** auth key and has
been validated against the hosted Tailscale control plane.

> ⚠️ **Headscale does not currently work.** The UI accepts a custom login
> server (Headscale) URL, but testing against Headscale v0.28.0 found the
> ts2021 control-plane **Noise handshake fails** (the server rejects the
> machine-key handshake), so the device cannot register. Tracked in
> [#7](../../issues/7) — a fix is being investigated. For now use Tailscale's
> hosted control plane.

`tailnet lock` is not supported (the device cannot sign its own node
key); disable it for the tailnet or pre-authorize the node.

## Known limitations

- **Single radio.** STA and AP share one 2.4 GHz radio and channel. If
  the upstream AP is on a different channel after a roam, throughput
  collapses until the device realigns (it auto-reboots to do so).
- **Throughput.** This is an MCU doing userspace crypto + NAT; expect
  roughly **0.3–1.4 Mbit/s** through the tunnel (plain STA highest, direct
  exit node mid, DERP-relayed exit node lowest), not gigabit. Plenty for
  IoT and remote-admin traffic.
- **Exit node fails closed.** By design — when a selected exit node is
  unreachable, AP-client internet traffic stops rather than leaking to
  the local uplink. Clear the exit node to restore direct internet.
- **Headscale not working** (see above — a control-plane handshake incompatibility is under investigation). **Tailnet lock unsupported.**
- **2.4 GHz only**, single AP subnet.

## Telemetry

The device reports a tiny, fully anonymous status payload to a Cloudflare
Worker — a small JSON on boot, then a heartbeat roughly once a day. **It's
on by default**, and one toggle in the **About** section (System tab)
turns it off for good (the choice is saved in NVS). Anyone can verify exactly what it
does — the whole thing is one function in
[`main/telemetry.c`](main/telemetry.c).

### Exactly what it sends

This is the *entire* payload — nothing else leaves the device:

```json
{
  "dh": "a1b2c3d4e5f6071839",
  "v":  "0.1.9",
  "bd": "2026-05-31",
  "et": "heartbeat",
  "bc": 276,
  "fc": 158,
  "up": 90074,
  "rr": 1,
  "rw": "",
  "ch": "S3r0",
  "fh": 53707,
  "ac": 42,
  "ts": "up"
}
```

| Field | Meaning | Example |
|---|---|---|
| `dh` | anonymous device ID — 16-hex `SHA-256(WiFi MAC + fixed salt)` plus a 2-hex integrity check (18 hex total). One-way; it can't be turned back into your MAC | `a1b2c3d4e5f6071839` |
| `v`  | firmware version | `0.1.9` |
| `bd` | firmware build date | `2026-05-31` |
| `et` | event type — `boot`, `heartbeat`, or a crash report | `heartbeat` |
| `bc` | total boot count | `276` |
| `fc` | total firmware-flash count | `158` |
| `up` | uptime, seconds | `90074` |
| `rr` | reset-reason code (ESP-IDF reason, or `100` = new firmware / `101` = rollback) | `1` |
| `rw` | short reboot-reason tag, or empty | `ch-realign 11->1` |
| `ch` | chip model + silicon revision | `S3r0` |
| `fh` | free heap at send time, bytes | `53707` |
| `ac` | Tailscale (re)connect count this session | `42` |
| `ts` | Tailscale toggle — `up` or `off` (just the switch; **no peers, no tailnet name**) | `up` |
| `cr` | crash signature — **only** added to a crash report | `StoreProhibited @ ml_derp_tx` |

It **never** sends SSIDs, IP or MAC addresses, tailnet names, peer
information, or anything you typed into the UI. The device ID is a salted
one-way hash (`compute_device_hash()` in
[`main/telemetry.c`](main/telemetry.c)), so reports can be grouped per
device without ever identifying one.

### Why it's on by default (a note from the maintainer)

No hidden agenda — the JSON above is literally all of it, and the code is
right there to check. I'd genuinely appreciate you leaving it on unless
you have a specific reason not to:

- It's how I'd catch a **mass PANIC** rolling across devices after a bad
  release — the same crash signature arriving from many `dh`s at once is a
  five-alarm fire I'd otherwise never see.
- Fully anonymized, it's the *only* signal I get for **how many people
  actually run this**. This is a free hobby project; if essentially nobody
  uses it long-term, that's fair feedback that I shouldn't keep pouring
  effort in.

Either way it's your call — flip it off in **About → Anonymous telemetry**
(System tab) and the device never phones home again.

## Security

Please report vulnerabilities privately — see [`SECURITY.md`](SECURITY.md).
The repo runs CodeQL, Dependabot, secret scanning, and a custom
[Sensitive Data Check](.github/workflows/sensitive-check.yml) on every
push.

## Development

```
main/                 firmware entry, web server + embedded SPA (index.html)
components/
  acl/                the ACL firewall engine
  sdlog/              microSD flight-recorder
  …                   DNS relay, telemetry, etc.
external/microlink/   Tailscale/WireGuard stack (git submodule, MIT)
docs/                 configuration reference, images
tools/                helper scripts
```

The entire web UI is the single file `main/index.html`, embedded into
the firmware by `main/CMakeLists.txt` at build time.

See **[CONTRIBUTING.md](CONTRIBUTING.md)** for build/flash setup and pull-request
guidelines, and **[docs/TESTING.md](docs/TESTING.md)** for the test harness.

## Support

Found a bug or have an idea? Open an
[issue](https://github.com/Csontikka/esp32-tailscale-subnet-router/issues).
If this firmware saved you a router purchase or an afternoon of
debugging, you can chip in:
[buy me a coffee](https://buymeacoffee.com/csontikka) ☕

<p align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="assets/logo/bunny.png">
    <img alt="Csontikka" src="assets/logo/bunny_dark.png" width="52">
  </picture>
</p>

## Credits & license

This firmware is [MIT](LICENSE) licensed. It builds on:

- **[microlink](https://github.com/Csontikka/microlink)** — Tailscale
  `ts2021` client (MIT), based on the original
  [CamM2325/microlink](https://github.com/CamM2325/microlink)
- **wireguard_lwip** — userspace WireGuard for lwIP (BSD-3-Clause),
  vendored inside microlink
- **[ESP-IDF](https://github.com/espressif/esp-idf)** — Espressif RTOS &
  networking (Apache-2.0)

See [`NOTICE.md`](NOTICE.md) for the full third-party attribution list.

> *Tailscale* and *Headscale* are trademarks of their respective owners.
> This is an independent, unaffiliated community project.
