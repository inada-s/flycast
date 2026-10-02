"""Malformed GGPO packets for the D1 case.

Every packet carries CONST_MAGIC so it gets past the first check. SyncReply is
left out: a random reply matching a peer's 16-bit sync nonce would be a
spoofing test, not a malformed-packet test. None of these carry a peer's
session magic, so they show malformed input is rejected without crashing;
they do not reach the handlers behind the magic check.
"""
import os
import random
import socket
import struct
import time

CONST_MAGIC, RELAY_MAGIC = 34046, 26315


def _hdr(type_, remote_endpoint, relay_to=0, org_type=0, relay_magic=0):
    return struct.pack("<HHHBBHBB", CONST_MAGIC, random.randrange(65536), random.randrange(65536),
                       remote_endpoint, type_, relay_magic, relay_to, org_type)


def _packet():
    ep = random.randrange(4)
    kind = random.randrange(8)
    if kind == 0:  # AppData claiming a huge size
        return _hdr(8, ep) + struct.pack("<HB", random.choice([513, 4096, 65535]), 1) + os.urandom(random.randrange(40))
    if kind == 1:  # Input claiming more bits than sent / allowed, bad input sizes
        body = os.urandom(16) + struct.pack("<I", random.randrange(1 << 31)) + struct.pack("<I", random.randrange(1 << 32))
        body += struct.pack("<HB", random.choice([4097, 65535, 200, 64]), random.choice([0, 255, 18, 12]))
        return _hdr(3, ep) + body + os.urandom(random.randrange(30))
    if kind == 2:  # truncated fixed-size messages
        return _hdr(random.choice([1, 4, 5, 7]), ep) + os.urandom(random.randrange(3))
    if kind == 3:  # unknown / invalid types
        return _hdr(random.choice([0, 9, 50, 98, 200]), ep) + os.urandom(random.randrange(64))
    if kind == 4:  # relay with a bogus original type
        return _hdr(99, ep, relay_magic=RELAY_MAGIC, relay_to=random.randrange(8),
                    org_type=random.choice([0, 9, 99, 255])) + os.urandom(random.randrange(64))
    if kind == 5:  # relay of a malformed AppData to a real peer
        return _hdr(99, ep, relay_magic=RELAY_MAGIC, relay_to=random.randrange(4), org_type=8) \
            + struct.pack("<HB", 65535, 1) + os.urandom(8)
    if kind == 6:  # sync request with short / random verification
        return _hdr(1, ep) + os.urandom(random.randrange(40))
    return struct.pack("<HHHBB", CONST_MAGIC, random.randrange(65536), random.randrange(65536),
                       random.randrange(256), random.choice([t for t in range(256) if t != 2])) \
        + os.urandom(4 + random.randrange(600))


def run(seconds: float, port_base: int = 20010) -> int:
    """Send to every local test port (port_base + peer) over IPv4 and IPv6 for `seconds`. Returns the packet count."""
    s4 = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s6 = socket.socket(socket.AF_INET6, socket.SOCK_DGRAM)
    sent = 0
    end = time.time() + seconds
    while time.time() < end:
        pkt = _packet()
        for port in range(port_base, port_base + 4):
            for sock, host in ((s4, "127.0.0.1"), (s6, "::1")):
                try:
                    sock.sendto(pkt, (host, port))
                    sent += 1
                except OSError:
                    pass
        time.sleep(0.002)
    return sent
