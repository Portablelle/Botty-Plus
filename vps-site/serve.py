#!/usr/bin/env python3
"""Preview the static portal locally. Console installation requires trusted HTTPS."""
import argparse
import functools
import http.server
from pathlib import Path

ROOT = Path(__file__).resolve().parent


class Handler(http.server.SimpleHTTPRequestHandler):
    def end_headers(self):
        self.send_header('Cache-Control', 'no-store')
        super().end_headers()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--bind', default='127.0.0.1')
    parser.add_argument('--port', type=int, default=8000)
    args = parser.parse_args()
    handler = functools.partial(Handler, directory=str(ROOT))
    with http.server.ThreadingHTTPServer((args.bind, args.port), handler) as server:
        print(f'Portal preview: http://{args.bind}:{args.port}/', flush=True)
        print('Console installation requires trusted HTTPS. Ctrl+C to stop.', flush=True)
        try:
            server.serve_forever()
        except KeyboardInterrupt:
            pass


if __name__ == '__main__':
    main()
