# Functional Test Catalog

A complete end-to-end test suite for the running device. Drive it from a
machine that has:

* IP reachability to the ESP's STA-side IP (the web UI)
* USB-Ethernet reachability to the test Pi (or any other LAN-attached
  Pi that can SSH-in and run `ping` / `curl`)
* SSH key auth to a read-only diagnostic node inside the tailnet (e.g.
  a separate Tailscale-running Linux box that advertises a subnet
  route — used as the "tailnet → Pi LAN" source for the routing matrix)

Tests live under `tools/test_lib/`, the driver is `tools/run_tests.py`.

## Prerequisites

```bash
pip install playwright paramiko
playwright install chromium
```

## Configuration

Everything is env-var driven so no IPs or credentials get committed.

| Variable           | Purpose                                                    | Example          |
|--------------------|------------------------------------------------------------|------------------|
| `ESP_HOST`         | base URL of the ESP web UI                                 | `http://192.0.2.10` |
| `ESP_WEB_PW`       | web-UI password                                            | `<your-pw>`      |
| `PI_HOST`          | IP of the test Pi over its always-on side-channel          | `192.0.2.50`     |
| `PI_USER`          | Pi SSH user (default `pi`)                                 | `pi`             |
| `PI_PW`            | Pi SSH password                                            | `<your-pw>`      |
| `PI_WIFI_IP`       | IP the Pi gets from the ESP's DHCP                         | `192.0.2.101`    |
| `DK_HOST`          | tailnet/LAN IP of the read-only diag node                  | `192.0.2.200`    |
| `DK_USER`          | SSH user (default `root`, key auth)                        | `root`           |
| `DK_TAILNET_IP`    | that node's `100.x` tailnet IP (Pi → tailnet target)       | `100.x.x.x`      |
| `DK_SUBNET_TARGET` | a host inside the LAN advertised by `DK_HOST`              | `192.0.2.250`    |
| `EXIT_NODE_IP`     | (optional) tailnet IP to use as exit node in stage B       | `100.x.x.x`      |
| `TEST_QUICK`       | `1` skips the slow exit-node stage of the routing module   | `0`              |

Drop those into a local `.env` (gitignored) and source it before running:

```bash
set -a; source .env; set +a
```

## Running

```bash
python tools/run_tests.py                      # everything
python tools/run_tests.py --only spa,network   # subset
python tools/run_tests.py --list               # available modules
TEST_QUICK=1 python tools/run_tests.py         # skip the exit-node toggle
```

Exit code is 0 only when every check passes; failures are summarised at
the bottom by module.

## Modules

### `spa` — Web UI smoke
Logs in, walks every tab (Status / Network / Tailscale / Firewall /
System / Tools), asserts that each tab renders at least one `.card`,
no input is flagged red on first render (the pristine-validator bug),
the IDs `#ts-routes` and `#ts-hostname` are unique (a previous trap
because of duplicate IDs between the Status preview and the editor),
and the JavaScript console is clean.

### `network` — REST endpoints + short-list round-trip
Checks `/api/status`, `/api/network`, `/api/dhcp/leases`,
`/api/dhcp/reservations`, then does a save → readback → restore on
`/api/portmap` and `/api/mac/denylist`. Verifies that `uptime_s`
grows between two reads (a dead httpd would return a frozen value).

### `firewall` — ACL CRUD
Asserts the four ACL lists are present with the documented names,
adds a probe rule (in `from_ap`, with src/dest in `10.255.255.0/24`
so it can never match real traffic), verifies it appears, deletes
it, verifies it's gone. Also sanity-checks that hit counters are
non-negative.

### `tailscale` — settings + runtime + routes round-trip
Inspects `/api/tailscale` for `settings`, `runtime`, and `peers`,
asserts `runtime.connected`, `tunnel_ip` starts with `100.`,
identity is `persistent`, and `advertise_routes` includes the AP
CIDR. Does a save → readback → restore of `advertise_routes`.
Exit-node behaviour lives in `routing`.

### `pi` — Pi-side WiFi state + watchdog hardening
SSHes into the Pi, asserts:
* `wlan0` is in NetworkManager state `100 (connected)`
* the `Csontikka-ESP32-Dev` profile has `autoconnect-retries=0`
  (infinite, the hardening we landed after the 6-day silent
  disconnect)
* `wlan-watchdog.timer` is enabled+active
* `/etc/systemd/journald.conf` has `Storage=persistent`
* `wlan0` carries the IP we expect (matches `PI_WIFI_IP`)

Then cross-checks: the ESP's `/api/dhcp/leases` includes the Pi at
that IP with a sane hostname and RSSI.

### `routing` — the 4×2 scenario matrix
The headline test. Runs every direction twice (once with no exit
node configured on the ESP, once with one). The directions are:

| direction (label)            | what it proves                                        |
|------------------------------|-------------------------------------------------------|
| Pi → external (`curl ipify`) | egress works; captures the public IP for delta check  |
| Pi → external (`ping 8.8.8.8`)| L3 egress + ICMP all the way out                     |
| Pi → tailnet peer            | Pi-LAN → microlink → CGNAT route → tailnet            |
| Pi → tailnet subnet          | Pi-LAN → microlink → tailnet → peer's subnet router   |
| Tailnet → Pi LAN (dk-lxc)    | inbound: ESP `advertise_routes` is honoured upstream  |

The exit-node delta check: the public IP collected at stage A must
differ from stage B (otherwise the exit-routing didn't take effect
even though the setting was accepted).

The exit node is auto-discovered if `EXIT_NODE_IP` is unset — we
walk `peers[]` and pick the first online, `direct_path=true`,
`is_exit_node=true` entry. Set `EXIT_NODE_IP` explicitly to pin it.

The original `exit_node_ip` is always restored at the end.

## Result codes

```
[PASS] module: <check>        # the check ran and succeeded
[FAIL] module: <check> — …    # the check ran and the assertion failed
[SKIP] module: <check> — …    # the check was bypassed (missing dep,
                              # TEST_QUICK=1, or an optional prereq
                              # like dk-lxc unreachable)
```

## Troubleshooting

* **All `spa` checks fail with `playwright not installed`** —
  `pip install playwright && playwright install chromium`.
* **`ssh to Pi` fails** — make sure the Pi is reachable on the
  side-channel IP you set in `PI_HOST`. The watchdog and re-flash
  matter only over WiFi; SSH over USB-Ethernet is the test-runner's
  out-of-band path.
* **Pi → external fails with no-exit** — usually means the ESP's STA
  uplink is down (check `/api/status` STA fields), not a routing bug.
* **Pi → tailnet peer fails** — check that microlink is actually
  connected (`tailscale.runtime.connected` must be true) and that
  the peer is online in the tailnet (`peers[]`).
* **Tailnet → Pi LAN fails** — the upstream peer must accept the ESP's
  advertised route. If the test node is `dk-lxc` and we changed the
  ESP's AP CIDR recently, the operator must approve the new route in
  the Tailscale admin (Headscale: `headscale nodes update --route`).
* **Public-IP delta is the same off/on** — exit-node routing didn't
  take effect. Look for "exit node" in the device's log: the chosen
  node may not advertise itself as an exit, or it's offline.
* **Routing matrix leaves a bad `exit_node_ip` behind on crash** —
  the runner restores it in a `finally:`, but if the runner is killed
  hard, manually `POST /api/tailscale {"settings":{"exit_node_ip":""}}`.

## Adding a test

1. Create `tools/test_lib/<new>.py` exporting `MODULE_ID`, `MODULE_DESC`,
   and `def run(ctx: Context) -> list[Result]`.
2. Use `check(results, MODULE_ID, name, ok, detail)` so the runner can
   render a uniform pass/fail line.
3. Add the module to `MODULES` in `tools/run_tests.py`.
4. Document the module in the appropriate section above.
