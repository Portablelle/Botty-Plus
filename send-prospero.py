#!/usr/bin/env python3
"""Send Prospero only, after Relapse's ELF loader starts on port 9021."""
import ipaddress
from pathlib import Path
import socket
import sys


def main():
    host = sys.argv[1] if len(sys.argv) > 1 else input("PS5 IPv4 address: ").strip()
    try:
        ipaddress.IPv4Address(host)
    except ValueError:
        raise SystemExit("Enter the PS5 IPv4 address, for example 192.168.1.50.")
    payload = Path(__file__).resolve().parent / "payloads" / "ProsperoMgr.elf"
    try:
        with socket.create_connection((host, 9021), timeout=15) as connection:
            connection.settimeout(120)
            with payload.open("rb") as source:
                while chunk := source.read(1024 * 1024):
                    connection.sendall(chunk)
            connection.shutdown(socket.SHUT_WR)
    except OSError as error:
        raise SystemExit(f"Transfer failed: {error}. Check the IP and that Relapse's ELF loader is running.")
    print("Transfer complete. Confirm the startup notification on PS5.")
    print(f"Then open: http://{host}:7070")


if __name__ == "__main__":
    main()
