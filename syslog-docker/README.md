# esp-syslog — minimal UDP syslog listener (port 5140)

Local-only debug helper. Captures every line the ESP forwards via the
syslog client (System tab → Syslog) and prints it to the container log.

## Usage

```bash
docker build -t esp-syslog syslog-docker/
docker run -d --name esp-syslog -p 5140:5140/udp esp-syslog
docker logs -f esp-syslog
```

Then enable syslog on the device pointing at the host IP and port 5140
(System tab → Syslog → enable + server + port).

Port 5140 instead of 514 so the container doesn't need privileged
bind permission on the host.
