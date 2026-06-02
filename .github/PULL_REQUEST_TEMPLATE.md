<!--
⚠️ Do not commit secrets: no Tailscale auth keys (tskey-…), real tailnet
names (…​.ts.net), public IPs, WiFi passwords, or sdkconfig. The
Sensitive Data Check workflow will flag these.
-->

## What & why

Short description of the change and the problem it solves.

Closes #

## Type

- [ ] Bug fix
- [ ] New feature
- [ ] Refactor / cleanup
- [ ] Docs

## Testing

How was this verified? (board, build, on-device steps)

- Board:
- Build: PlatformIO / ESP-IDF
- On-device check:

## Checklist

- [ ] Builds (`pio run -e esp32-s3` or `idf.py build`)
- [ ] No secrets / real IPs / tailnet names in the diff
- [ ] Docs updated if behaviour or config changed
