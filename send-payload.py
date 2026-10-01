#!/usr/bin/env python3
"""Send one local ELF payload to a PS5 ELF loader."""
import argparse
from pathlib import Path
import socket

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("host")
parser.add_argument("payload", type=Path)
args = parser.parse_args()
with args.payload.open("rb") as source:
    if source.read(4) != b"\x7fELF":
        raise SystemExit("The selected file is not an ELF payload.")
    source.seek(0)
    with socket.create_connection((args.host, 9021), timeout=15) as connection:
        connection.settimeout(120)
        while chunk := source.read(1024 * 1024):
            connection.sendall(chunk)
        connection.shutdown(socket.SHUT_WR)
print(f"Sent {args.payload.name}. Verify startup on the console.")
