# Third-party notices

This project (`esp32-tailscale-subnet-router`) is licensed under the
[MIT License](LICENSE), Copyright (c) 2026 Csontikka.

It incorporates the following third-party components, each under its own
license:

## microlink

- **Repository:** https://github.com/Csontikka/microlink
- **Based on:** https://github.com/CamM2325/microlink (the original
  microlink project)
- **License:** MIT
- **Use:** Tailscale-compatible `ts2021` control-plane + userspace
  WireGuard data-plane stack. Attached as a git submodule under
  `external/microlink`.

## wireguard_lwip

- **License:** BSD-3-Clause
- **Use:** Userspace WireGuard implementation for the lwIP TCP/IP stack.
  Vendored inside microlink at
  `external/microlink/components/microlink/components/wireguard_lwip`.
  See that directory's `LICENSE` for the full text and copyright.

## ESP-IDF

- **Repository:** https://github.com/espressif/esp-idf
- **License:** Apache-2.0
- **Use:** Espressif's RTOS, WiFi/networking stack, and build system.

## Managed components

Additional ESP-IDF managed components pulled at build time keep their
original licenses as declared in their respective `idf_component.yml` /
`LICENSE` files under `managed_components/`.

---

## Trademarks

*Tailscale* and *Headscale* are trademarks of their respective owners.
This project is an independent, community-built client and is **not**
affiliated with, endorsed by, or sponsored by Tailscale Inc. or the
Headscale project.
