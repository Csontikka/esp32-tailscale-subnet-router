# Configuration reference

Every setting below is configured from the device web UI. Changes are
written to NVS; some require a reboot to take effect (the UI flags a
pending reboot when so).

> IP addresses, SSIDs and peer names shown in screenshots throughout the
> docs are generic placeholders.

## Network → Device-wide network

| Field | Meaning |
|---|---|
| **Hostname** | Name advertised on the uplink network (DHCP/mDNS). |
| **DNS mapping TTL override** | `0` = off. Overrides the TTL the device applies to DNS-learned routes, for roaming continuity. |

## Network → Uplink networks (STA)

Up to **5** networks, tried in order. Per network:

| Field | Meaning |
|---|---|
| **SSID** | Upstream 2.4 GHz network to join. |
| **Password** | WPA2/WPA3 PSK. Leave empty to keep the stored one. |
| **Static IP** *(optional)* | `ip` / `mask` / `gw` / `dns`; empty = DHCP. |
| **WPA2-Enterprise (EAP)** *(optional)* | Method, phase-2, identity/username/password, optional CA bundle. |

## Network → Access Point (AP)

| Field | Default | Meaning |
|---|---|---|
| **AP SSID** | — | Network this device broadcasts. |
| **AP password** | — | WPA2 PSK; leave empty to keep current. |
| **Channel** | *auto* | Read-only. The single 2.4 GHz radio is shared with the STA, so the AP automatically follows (and realigns to) the uplink channel — it is not operator-settable. |
| **AP IP / Subnet mask** | `192.168.4.1` / `255.255.255.0` | The AP subnet. Changing it offers to update the advertised Tailscale route. |
| **DNS served to AP clients** | this device | Empty = the on-board forwarder; or push a specific resolver. |
| **On-board DNS forwarder** | on | Resolver + PSRAM response cache for AP clients. |
| **Forwarder upstream** | learned / `1.1.1.1` | Upstream resolver the forwarder queries. |
| **Hide SSID** | off | Stop broadcasting the SSID in beacons. |

## Network → DHCP, clients, denylist, port forwarding

- **DHCP reservations** — pin an IP to a MAC (max 16).
- **Connected clients / Active leases** — live tables with per-client signal.
- **Denied MAC addresses** — block specific clients from associating.
- **Port forwarding** — map an external port to an AP-side client.

## Tailscale

| Field | Meaning |
|---|---|
| **Enabled** | Master switch for the tailnet client. |
| **Auth key** | Tailscale auth key (`tskey-…`). Leave empty to keep current. |
| **Hostname** | Node name on the tailnet. |
| **Login server** | Custom control plane (Headscale). Empty = hosted Tailscale. Accepts `host`, `host:port`, `http://host[:port]`, `https://host[:port]` (TLS validates against public CAs only — no self-signed). Requires Headscale ≥ 0.26 (validated on v0.28.0). |
| **Client version** | `Hostinfo.IPNVersion` reported to the control plane. Empty (default) = not reported. The Tailscale admin console gates some operations (e.g. reassigning a node address) on this and shows "Device is too old" when empty; set e.g. `1.98.9` to clear it, the `version.Long()` hash suffix is added automatically. Headscale users need none of this. Applies on the next restart. |
| **Advertise the AP subnet** | On by default. The AP subnet is announced as a subnet route, computed from the AP settings (so changing the AP address needs no route edit). Peers use it only after you approve it in the admin console. Off = announce only the list below. |
| **Additional advertised subnet routes** | One CIDR per line, on top of the AP subnet — e.g. the uplink LAN (see Source-NAT). |
| **Source-NAT advertised routes** | Off by default. Masquerades traffic forwarded from the tunnel out to the uplink LAN (or out to the internet when this device is an exit node) to the device's own STA IP — the Tailscale default for subnet routers. **On** = the uplink subnet works the moment you advertise it (upstream hosts reply to this device, which un-NATs back into the tunnel). **Off** = the upstream router needs a static route for the tailnet (`100.64.0.0/10 → this device`) instead. Enabling it offers to add your uplink subnet to the advertised routes. ⚠️ Do **not** advertise *and* accept the same subnet — that loops the inbound route back into the tunnel. |
| **Exit node** | Route AP-client public traffic through this tailnet exit node. Fails closed if unreachable. |
| **Max peers** | Upper bound on tracked peers. |
| **Accept peer subnet routes** | Install routes other nodes advertise. |
| **LAN bypass when using an exit node** | RFC1918 destinations stay on the local LAN even with an exit node selected. |

### Tunnel MTU

`Auto` is recommended — 1280, the Tailscale tunnel MTU. Every peer's tun
device is 1280 whether the path is direct or DERP-relayed, so that is the
largest packet the far end carries; the AP-side TCP MSS clamp (1240) and the
ICMP path-MTU replies follow from it. A fixed MTU can be forced (576–1500)
if a path misbehaves.

## Firewall

Four chains, **first match wins**; an empty chain allows by default.

| Chain | Direction |
|---|---|
| `TO_ESP` | Internet → ESP |
| `FROM_ESP` | ESP → Internet |
| `TO_AP` | Clients → ESP |
| `FROM_AP` | ESP → Clients |

Per rule: **source** / **destination** (`any` or CIDR), **protocol**
(Any / ICMP / TCP / UDP), **source/dest port** (`0` = any, TCP/UDP only),
**action** (Allow / Deny).

## SNMP

Read-only SNMPv1/v2c agent, **off by default**. When enabled it binds
UDP `0.0.0.0:161` on every interface — including the tailnet, so a poller
anywhere on your tailnet can reach it without opening anything to the LAN.
Requests whose community string does not match are dropped without a reply,
so a wrong community is indistinguishable from the agent being off.

| Field | Meaning |
|---|---|
| **Enable SNMP** | Starts/stops the agent in place; no reboot. |
| **Community** | Shared secret for v1/v2c. There is no v3 — treat this as a password sent in clear text and prefer reaching the device over the tailnet. |
| **sysName / sysContact / sysLocation** | Served as `1.3.6.1.2.1.1.{5,4,6}.0`. Free text. |

Only GET and GETNEXT are answered; there is no SET, so nothing can be
changed over SNMP. GETBULK is accepted but returns one successor per
varbind, so bulk walks simply take more round trips.

### What it serves

| MIB | OIDs | Notes |
|---|---|---|
| MIB-II system | `1.3.6.1.2.1.1` | sysDescr carries the firmware version. |
| MIB-II interfaces | `1.3.6.1.2.1.2` | Fixed indices: **1** `eth0`, **2** `wlan0`, **3** `ts0` (Tailscale tunnel). Octet and packet counters on all three. |
| HOST-RESOURCES-MIB | `1.3.6.1.2.1.25` | `hrProcessorLoad.{1,2}` per core (0–100, 5 s window), `hrStorageTable` for internal DRAM and PSRAM, `hrSystemProcesses` for the task count. |
| ENTITY-SENSOR-MIB | `1.3.6.1.2.1.99` | Die temperature, deci-degrees Celsius (`entPhySensorPrecision` = 1, so 508 means 50.8 °C). |
| Private | `1.3.6.1.4.1.99999.1.1.4.0` | `heapMinFreeBytes`, the all-time-low free-heap watermark — the one reading with no standard equivalent. **99999 is not an IANA-assigned enterprise number**; do not grow this tree. |

Interface indices are deliberately fixed rather than derived from lwIP's
`netif_list`, so a graph pointed at ifIndex 2 keeps meaning `wlan0` even
as interfaces come and go. An interface that is down — or that this board
does not have at all, such as `eth0` on a WiFi-only build — reports
`ifOperStatus down` with zero counters rather than disappearing and
renumbering the rows after it.

Quick check from any machine with net-snmp:

```bash
snmpwalk -v2c -c <community> <device-ip> 1.3.6.1.2.1.1        # system
snmpwalk -v2c -c <community> <device-ip> 1.3.6.1.2.1.2.2.1.10 # ifInOctets
snmpget  -v2c -c <community> <device-ip> 1.3.6.1.2.1.25.3.3.1.2.1  # CPU core 0
snmpget  -v2c -c <community> <device-ip> 1.3.6.1.2.1.99.1.1.1.4.1  # temperature
```

## System

| Field | Meaning |
|---|---|
| **Device name** | Friendly name shown in the UI header and `/api`. |
| **Timezone** | Device TZ; a real change flags a pending reboot. |
| **Anonymous telemetry** | On by default; one toggle in the **About** card opts out. Daily anonymous hash + counters (see README → Telemetry). |
| **SD card logging** | Enable + per-sink (console / SD) log level; the on-device flight recorder. |
| **Firmware update (OTA)** | Manual upload, or polled auto-install window. Optional **beta channel** also offers GitHub pre-releases (off by default = stable releases only). |
| **Encrypted backup / restore** | Export/import the full config, encrypted with a passphrase. |
| **Danger zone** | Reboot, factory reset, reset Tailscale identity. |
