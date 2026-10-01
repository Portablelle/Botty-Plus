#!/usr/bin/env python3
"""Serve the unchanged Relapse checkout on the local network."""
import functools
import argparse
import http.server
from pathlib import Path
import socket

ROOT = Path(__file__).resolve().parent / "Relapse-Exploit"
PORT = 8000


class Handler(http.server.SimpleHTTPRequestHandler):
    def end_headers(self):
        self.send_header("Cache-Control", "no-store")
        super().end_headers()

    def do_GET(self):
        if any(part.startswith(".") for part in self.path.split("/") if part):
            self.send_error(403)
            return
        super().do_GET()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--bind", help="Local network IPv4 address to serve on")
    args = parser.parse_args()
    for name in ("index.html", "payloads/kexp_2026_05_25.bin", "payloads/elfldr-ps5-1360.elf"):
        if not (ROOT / name).is_file():
            raise SystemExit(f"Missing required file: {ROOT / name}")
    address = args.bind
    if not address:
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
            probe.connect(("192.0.2.1", 80))
            address = probe.getsockname()[0]
    handler = functools.partial(Handler, directory=str(ROOT))
    try:
        server = http.server.ThreadingHTTPServer((address, PORT), handler)
    except OSError as error:
        raise SystemExit(f"Cannot start server: {error}. Another server may already be running.")
    print(f"Open on PS5: http://{address}:{PORT}/", flush=True)
    print("Keep this window open. Press Ctrl+C to stop.", flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()


if __name__ == "__main__":
    main()
