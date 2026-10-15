"""Read flycast state hash logs (gdxsv:hashlog=yes).

One line per saved frame: "frame state_hash state_len". A rollback saves a
frame again, so a frame number can repeat; its last line is the state the
session settled on.

The hash covers what GGPO saves (CPU and peripherals, not memory) and a few
per-peer values, so it can differ between healthy peers: compare frame
numbers across peers, hashes only within one peer or between runs.
"""
from typing import Dict, List, Tuple


def read(path: str) -> List[Tuple[int, str]]:
    """(frame, state_hash) per line, in file order; [] if the file is missing."""
    rows = []
    try:
        with open(path, encoding="utf-8", errors="replace") as f:
            for line in f:
                parts = line.split()
                if parts and parts[0].lstrip("-").isdigit():
                    rows.append((int(parts[0]), parts[1] if len(parts) > 1 else ""))
    except OSError:
        pass
    return rows


def frames(path: str) -> List[int]:
    return [fr for fr, _ in read(path)]


def last_frame(path: str) -> int:
    fr = frames(path)
    return fr[-1] if fr else -1


def rolled_back(path: str) -> int:
    """Number of distinct frames saved more than once."""
    seen, dup = set(), set()
    for fr in frames(path):
        (dup if fr in seen else seen).add(fr)
    return len(dup)


def settled(path: str) -> Dict[int, str]:
    """frame -> the last state_hash logged for it."""
    return dict(read(path))
