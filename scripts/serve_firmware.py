#!/usr/bin/env python3
"""
serve_firmware.py - Serve a single firmware .bin file over plain HTTP.

Intended to bridge v1.14 ESP8266 devices that cannot perform HTTPS OTA
updates during migration to newer firmware.  Run this script alongside the
migration script so the device can pull the binary from a local URL.

Usage:
    python3 scripts/serve_firmware.py --file Client.ino.bin [--port 8080]
"""

import argparse
import http.server
import os
import socket
import socketserver
import sys
from datetime import datetime, timezone


# ---------------------------------------------------------------------------
# Shared state collected across requests
# ---------------------------------------------------------------------------

_stats = {
    "requests": 0,
    "bytes": 0,
}


# ---------------------------------------------------------------------------
# Request handler
# ---------------------------------------------------------------------------

class FirmwareHandler(http.server.BaseHTTPRequestHandler):
    """Serve a single firmware file; return 404 for everything else."""

    # Set by the caller before the server is started.
    firmware_filename: str = ""
    firmware_path: str = ""
    firmware_size: int = 0

    # Silence the default BaseHTTPRequestHandler log_message so we can
    # control formatting ourselves.
    def log_message(self, fmt, *args):  # noqa: D401
        pass

    def _log(self, code: int, nbytes: int) -> None:
        ts = datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
        client_ip = self.client_address[0]
        print(
            f"{ts}  {client_ip}  {self.command}  {self.path}  {code}  {nbytes} bytes",
            flush=True,
        )

    def _send_firmware_headers(self, send_body: bool) -> None:
        self.send_response(200)
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("Content-Length", str(self.firmware_size))
        self.send_header(
            "Content-Disposition",
            f'attachment; filename="{self.firmware_filename}"',
        )
        self.end_headers()

        if send_body:
            with open(self.firmware_path, "rb") as fh:
                self.wfile.write(fh.read())

        _stats["requests"] += 1
        _stats["bytes"] += self.firmware_size if send_body else 0
        self._log(200, self.firmware_size if send_body else 0)

    def _send_404(self) -> None:
        body = b"404 Not Found\n"
        self.send_response(404)
        self.send_header("Content-Type", "text/plain")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)
        _stats["requests"] += 1
        self._log(404, len(body))

    def do_GET(self) -> None:  # noqa: N802
        if self.path.lstrip("/") == self.firmware_filename:
            self._send_firmware_headers(send_body=True)
        else:
            self._send_404()

    def do_HEAD(self) -> None:  # noqa: N802
        if self.path.lstrip("/") == self.firmware_filename:
            self._send_firmware_headers(send_body=False)
        else:
            self._send_404()


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def local_ipv4_addresses() -> list[str]:
    """Return all non-loopback local IPv4 addresses."""
    addrs = []
    try:
        # getaddrinfo with a dummy host resolves only the local stack; using
        # a UDP connect trick is more reliable for getting the preferred
        # outbound interface, but enumerating all interfaces gives us every
        # address the user might reach us on.
        hostname = socket.gethostname()
        for info in socket.getaddrinfo(hostname, None, socket.AF_INET):
            ip = info[4][0]
            if not ip.startswith("127."):
                if ip not in addrs:
                    addrs.append(ip)
    except socket.gaierror:
        pass

    # Fall back / supplement with the UDP trick for the primary outbound IP.
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
            s.connect(("8.8.8.8", 80))
            primary = s.getsockname()[0]
        if primary not in addrs:
            addrs.append(primary)
    except OSError:
        pass

    return addrs or ["<unknown>"]


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------

def main() -> None:
    parser = argparse.ArgumentParser(
        description=(
            "Serve a single firmware .bin file over plain HTTP for ESP8266 OTA."
        )
    )
    parser.add_argument(
        "--file",
        required=True,
        metavar="PATH",
        help="Path to the firmware .bin file to serve.",
    )
    parser.add_argument(
        "--port",
        type=int,
        default=8080,
        metavar="PORT",
        help="TCP port to listen on (default: 8080).",
    )
    args = parser.parse_args()

    # Validate the file exists before we bind any socket.
    if not os.path.isfile(args.file):
        print(f"Error: file not found: {args.file}", file=sys.stderr)
        sys.exit(1)

    firmware_path = os.path.abspath(args.file)
    firmware_filename = os.path.basename(firmware_path)
    firmware_size = os.path.getsize(firmware_path)

    # Inject file info into the handler class (class-level attributes shared
    # across all request instances).
    FirmwareHandler.firmware_filename = firmware_filename
    FirmwareHandler.firmware_path = firmware_path
    FirmwareHandler.firmware_size = firmware_size

    # Allow rapid restarts without TIME_WAIT blocking the port.
    socketserver.TCPServer.allow_reuse_address = True

    print(f"Serving firmware : {firmware_filename}")
    print(f"File size        : {firmware_size:,} bytes")
    print(f"Port             : {args.port}")
    print()
    for ip in local_ipv4_addresses():
        print(f"  http://{ip}:{args.port}/{firmware_filename}")
    print()
    print("Press Ctrl+C to stop.")
    print()

    with socketserver.TCPServer(("0.0.0.0", args.port), FirmwareHandler) as httpd:
        try:
            httpd.serve_forever()
        except KeyboardInterrupt:
            pass

    print()
    print("--- Summary ---")
    print(f"Total requests   : {_stats['requests']}")
    print(f"Total bytes sent : {_stats['bytes']:,}")
    print("Server stopped.")


if __name__ == "__main__":
    main()
