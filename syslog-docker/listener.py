import socketserver, sys, datetime
class H(socketserver.BaseRequestHandler):
    def handle(self):
        data, _ = self.request
        msg = data.decode('utf-8', 'replace').rstrip()
        ts = datetime.datetime.now().strftime('%H:%M:%S.%f')[:-3]
        print(f"[{ts}] {msg}", flush=True)
with socketserver.UDPServer(('0.0.0.0', 5140), H) as srv:
    print("[esp-syslog listener] up on UDP 5140", flush=True)
    srv.serve_forever()
