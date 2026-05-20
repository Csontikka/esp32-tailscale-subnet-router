# esp32-tailscale-subnet-router

ESP32-S3 firmware that acts as a WiFi NAT router **and** a Tailscale
subnet router. Devices on its 2.4 GHz access point can reach the
internet via the upstream WiFi, and any peer on your tailnet can reach
those AP-side devices through CGNAT routing.

> **Status: early work-in-progress.** The repo is being scaffolded
> from scratch on the Espressif ESP-IDF NAPT example, with components
> migrated in from an older private fork. Expect things to land in
> small chunks over the next several days.

## Goals

- Pocket-sized Tailscale subnet router for a smart-home / IoT LAN
- Single ESP32-S3 board, no extra hardware
- Web UI for first-time setup and day-to-day admin
- Anonymous opt-in usage telemetry (boot/flash counters, build date)
  reported to a Cloudflare Worker so the maintainer can tell how the
  project is used in the wild without seeing anything identifying

## Hardware

- Target: ESP32-S3 with 8 MB PSRAM (octal, 80 MHz)
- Reference board: ESP32-S3 DevKitC-1 N16R8
- Other ESP-IDF targets (esp32, esp32-c3, wt32-eth01) are kept in the
  PlatformIO matrix but only S3 sees regular testing.

## Components in use

- **wireguard_lwip** (BSD-3-Clause) — vendored, the userspace
  WireGuard stack used by microlink
- **[Csontikka/microlink](https://github.com/Csontikka/microlink)**
  (MIT) — the Tailscale `ts2021` protocol client, attached as a git
  submodule on the `esp32-router-integration` branch
- **ESP-IDF** (Apache-2.0) — Espressif's RTOS + networking stack

## Build

Documentation will land alongside the code. Until then:

```bash
git clone --recurse-submodules https://github.com/Csontikka/esp32-tailscale-subnet-router
cd esp32-tailscale-subnet-router
# ESP-IDF >= 5.5.3, idf.py set-target esp32s3, idf.py build
# or PlatformIO: pio run -e esp32-s3
```

## License

[MIT](./LICENSE). Other components keep their original licenses (see
each subdirectory for details and `NOTICE.md` for the third-party
attribution list once it's written).
