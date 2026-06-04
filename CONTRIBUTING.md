# Contributing

Thanks for your interest! This is a small, single-maintainer hobby project, so
please keep changes focused and be patient with review turnaround.

## Before you start

- For anything non-trivial, **open an issue first** to discuss the idea — it
  saves wasted effort on both sides.
- Bug reports are most useful with: firmware version (Status page), the board,
  what you did, what you expected, what happened, and any relevant logs
  (Diagnostics → live log, or the SD flight recorder).

## Building & flashing

The firmware is an [ESP-IDF](https://docs.espressif.com/projects/esp-idf/) app
built with [PlatformIO](https://platformio.org/). Target board: **ESP32-S3**
(16 MB flash, octal PSRAM).

```sh
# build
pio run -e esp32-s3

# flash over USB serial (replace the port)
pio run -e esp32-s3 -t upload --upload-port COMx     # Windows
pio run -e esp32-s3 -t upload --upload-port /dev/ttyUSB0   # Linux

# serial monitor
pio device monitor -b 115200
```

The `external/microlink` submodule (the Tailscale/WireGuard stack) is required:

```sh
git clone --recurse-submodules https://github.com/Csontikka/esp32-tailscale-subnet-router
# or, in an existing clone:
git submodule update --init --recursive
```

The entire web UI is the single file `main/index.html`, embedded into the
firmware at build time by `main/CMakeLists.txt`. Edit that file directly; a
normal `pio run` re-embeds it.

## Tests

See **[docs/TESTING.md](docs/TESTING.md)** for the host-side test harness and how
to point it at a device. Please run the relevant tests before opening a PR and
mention the result.

## Pull requests

- Keep PRs small and single-purpose; describe the change and how you tested it.
- Match the surrounding code style (the codebase favours dense, explanatory
  comments around non-obvious hardware/network behaviour — keep that).
- Update `CHANGELOG.md` (under `[Unreleased]`) and any affected docs.
- Don't bump `PROJECT_VER` in a feature PR — releases are cut by the maintainer.

## Never commit secrets

This is a public repo with secret-scanning CI (`.github/`). **Do not commit**
auth keys (`tskey-…`, `hskey-…`), API tokens, passwords, real network details
(internal IPs, SSIDs, MAC addresses, your tailnet name/peers), or `sdkconfig`
with embedded credentials. Use generic placeholders in docs and screenshots.
Anything under `secrets/` is git-ignored — keep credentials there.

## License

By contributing you agree that your contributions are licensed under the
project's [MIT License](LICENSE).
