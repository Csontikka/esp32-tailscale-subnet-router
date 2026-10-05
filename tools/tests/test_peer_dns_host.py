#!/usr/bin/env python3
"""Compile and test selected PeerAPI parsing helpers on the host.

The C functions are extracted from ``main/peer_dns.c`` at runtime, so this
test exercises the checked-in implementation rather than a copied Python or C
translation.
"""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile


REPO_ROOT = Path(__file__).resolve().parents[2]
SOURCE = REPO_ROOT / "main" / "peer_dns.c"


def extract_function(source: str, signature: str) -> str:
    start = source.find(signature)
    if start < 0:
        raise RuntimeError(f"function signature not found: {signature}")
    opening = source.find("{", start)
    if opening < 0:
        raise RuntimeError(f"function body not found: {signature}")
    depth = 0
    for index in range(opening, len(source)):
        char = source[index]
        if char == "{":
            depth += 1
        elif char == "}":
            depth -= 1
            if depth == 0:
                return source[start:index + 1]
    raise RuntimeError(f"unterminated function body: {signature}")


def harness_source(peer_dns_source: str) -> str:
    functions = "\n\n".join([
        extract_function(peer_dns_source, "static bool sockaddr_ipv4_host"),
        extract_function(peer_dns_source, "static int base64url_value"),
        extract_function(peer_dns_source, "static bool decode_base64url"),
    ])
    return f"""\
#define _DEFAULT_SOURCE 1
#include <arpa/inet.h>
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>

#define PEER_DNS_MAX_MESSAGE 4096U
#define PEER_DNS_MAX_B64 (((PEER_DNS_MAX_MESSAGE + 2U) / 3U) * 4U)

{functions}

int main(void)
{{
    struct sockaddr_storage storage = {{0}};
    struct sockaddr_in *direct = (struct sockaddr_in *)&storage;
    direct->sin_family = AF_INET;
    assert(inet_pton(AF_INET, "192.0.2.1", &direct->sin_addr) == 1);
    uint32_t address = 0;
    assert(sockaddr_ipv4_host(&storage, &address));
    assert(address == UINT32_C(0xc0000201));

    memset(&storage, 0, sizeof(storage));
    struct sockaddr_in6 *mapped = (struct sockaddr_in6 *)&storage;
    mapped->sin6_family = AF_INET6;
    assert(inet_pton(AF_INET6, "::ffff:192.0.2.1", &mapped->sin6_addr) == 1);
    assert(sockaddr_ipv4_host(&storage, &address));
    assert(address == UINT32_C(0xc0000201));

    memset(&storage, 0, sizeof(storage));
    struct sockaddr_in6 *native = (struct sockaddr_in6 *)&storage;
    native->sin6_family = AF_INET6;
    assert(inet_pton(AF_INET6, "2001:db8::1", &native->sin6_addr) == 1);
    assert(!sockaddr_ipv4_host(&storage, &address));

    uint8_t decoded[8] = {{0}};
    size_t decoded_len = 0;
    assert(decode_base64url("AQIDBA", 6, decoded, sizeof(decoded), &decoded_len));
    const uint8_t expected[] = {{1, 2, 3, 4}};
    assert(decoded_len == sizeof(expected));
    assert(memcmp(decoded, expected, sizeof(expected)) == 0);
    assert(!decode_base64url("A", 1, decoded, sizeof(decoded), &decoded_len));
    assert(!decode_base64url("Zg==", 4, decoded, sizeof(decoded), &decoded_len));
    assert(!decode_base64url("Zh", 2, decoded, sizeof(decoded), &decoded_len));

    puts("PASS peer_dns host helpers");
    return 0;
}}
"""


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--cc",
        default=os.environ.get("CC", "cc"),
        help="host C compiler (default: $CC or cc)",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    compiler = shutil.which(args.cc)
    if not compiler:
        print(f"FAIL: C compiler not found: {args.cc}", file=sys.stderr)
        return 1

    try:
        source = SOURCE.read_text(encoding="utf-8")
        harness = harness_source(source)
        with tempfile.TemporaryDirectory(prefix="peer-dns-host-") as temp:
            temp_dir = Path(temp)
            c_file = temp_dir / "peer_dns_host.c"
            executable = temp_dir / "peer_dns_host"
            c_file.write_text(harness, encoding="utf-8")
            subprocess.run(
                [compiler, "-std=c11", "-Wall", "-Wextra", "-Werror",
                 str(c_file), "-o", str(executable)],
                check=True,
            )
            subprocess.run([str(executable)], check=True)
    except (OSError, RuntimeError, subprocess.CalledProcessError) as error:
        print(f"FAIL: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
