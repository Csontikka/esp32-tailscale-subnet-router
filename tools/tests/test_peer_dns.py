#!/usr/bin/env python3
"""Exercise the PeerAPI DNS-over-HTTP contract against a running device.

Run this from a Tailscale peer known to the ESP32::

    python tools/tests/test_peer_dns.py --peer-url http://TAILNET_IP

AP or uplink rejection is optional because those addresses are often not
reachable from the peer running the test. Repeat ``--reject-url`` for every
reachable non-tailnet address that must return 403.
"""

from __future__ import annotations

import argparse
import base64
import struct
import sys
import urllib.error
import urllib.parse
import urllib.request
from dataclasses import dataclass
from email.message import Message


DNS_MEDIA_TYPE = "application/dns-message"
DNS_ID = 0x4567


@dataclass(frozen=True)
class Response:
    status: int
    body: bytes
    headers: Message


def endpoint_url(base: str) -> str:
    parsed = urllib.parse.urlsplit(base)
    if parsed.scheme != "http" or not parsed.netloc:
        raise ValueError("URL must be an http:// URL with a host")
    if parsed.query or parsed.fragment:
        raise ValueError("URL must not contain a query or fragment")
    path = parsed.path.rstrip("/")
    if path and path != "/dns-query":
        raise ValueError("URL path must be empty or /dns-query")
    return urllib.parse.urlunsplit((parsed.scheme, parsed.netloc, "/dns-query", "", ""))


def make_query() -> bytes:
    header = struct.pack("!6H", DNS_ID, 0x0100, 1, 0, 0, 0)
    question = b"\x07example\x03com\x00" + struct.pack("!HH", 1, 1)
    return header + question


def request(url: str, timeout: float, data: bytes | None = None,
            content_type: str | None = None) -> Response:
    headers = {"Content-Type": content_type} if content_type else {}
    req = urllib.request.Request(url, data=data, headers=headers)
    try:
        with urllib.request.urlopen(req, timeout=timeout) as response:
            return Response(response.status, response.read(), response.headers)
    except urllib.error.HTTPError as error:
        return Response(error.code, error.read(), error.headers)


def require(condition: bool, detail: str) -> None:
    if not condition:
        raise AssertionError(detail)


def verify_dns_response(label: str, response: Response, query: bytes) -> None:
    require(response.status == 200,
            f"{label}: expected 200, got {response.status}: {response.body!r}")
    require(response.headers.get_content_type() == DNS_MEDIA_TYPE,
            f"{label}: unexpected Content-Type {response.headers.get('Content-Type')!r}")
    require("no-store" in response.headers.get("Cache-Control", "").lower(),
            f"{label}: Cache-Control does not contain no-store")
    require(len(response.body) >= 12, f"{label}: truncated DNS response")

    ident, flags, qdcount, ancount, _, _ = struct.unpack("!6H", response.body[:12])
    require(ident == DNS_ID, f"{label}: DNS transaction ID changed")
    require(flags & 0x8000 != 0, f"{label}: DNS QR response bit is clear")
    require(flags & 0x000F == 0, f"{label}: DNS response RCODE is {flags & 0x000F}")
    require(qdcount == 1, f"{label}: expected one echoed question, got {qdcount}")
    require(ancount > 0, f"{label}: example.com A response has no answers")
    require(response.body[12:len(query)] == query[12:],
            f"{label}: DNS question was not echoed unchanged")
    print(f"PASS {label}: {ancount} answer(s), {len(response.body)} bytes")


def expect_status(label: str, expected: int, response: Response) -> None:
    require(response.status == expected,
            f"{label}: expected {expected}, got {response.status}: {response.body!r}")
    print(f"PASS {label}: HTTP {response.status}")


def run(peer_url: str, reject_urls: list[str], timeout: float) -> None:
    endpoint = endpoint_url(peer_url)
    query = make_query()

    verify_dns_response(
        "POST",
        request(endpoint, timeout, data=query, content_type=DNS_MEDIA_TYPE),
        query,
    )
    encoded = base64.urlsafe_b64encode(query).decode("ascii").rstrip("=")
    verify_dns_response(
        "GET",
        request(f"{endpoint}?dns={encoded}", timeout),
        query,
    )

    malformed = [
        ("bad base64url", 400, request(f"{endpoint}?dns=***", timeout)),
        ("missing dns parameter", 400, request(endpoint, timeout)),
        ("short DNS message", 400,
         request(endpoint, timeout, data=b"123", content_type=DNS_MEDIA_TYPE)),
        ("wrong media type", 415,
         request(endpoint, timeout, data=query, content_type="text/plain")),
    ]
    for label, expected, response in malformed:
        expect_status(label, expected, response)

    for base in reject_urls:
        reject_endpoint = endpoint_url(base)
        response = request(reject_endpoint, timeout, data=query,
                           content_type=DNS_MEDIA_TYPE)
        expect_status(f"non-tailnet destination {reject_endpoint}", 403, response)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--peer-url",
        required=True,
        help="ESP32 tailnet base URL, for example http://TAILNET_IP",
    )
    parser.add_argument(
        "--reject-url",
        action="append",
        default=[],
        help="reachable AP/LAN base URL expected to return 403; may be repeated",
    )
    parser.add_argument(
        "--timeout",
        type=float,
        default=8.0,
        help="timeout for each HTTP request in seconds (default: 8)",
    )
    args = parser.parse_args()
    if args.timeout <= 0:
        parser.error("--timeout must be positive")
    return args


def main() -> int:
    args = parse_args()
    try:
        run(args.peer_url, args.reject_url, args.timeout)
    except (AssertionError, ValueError, urllib.error.URLError, TimeoutError) as error:
        print(f"FAIL: {error}", file=sys.stderr)
        return 1
    print("All PeerAPI DNS contract checks passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
