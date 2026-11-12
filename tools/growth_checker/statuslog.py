"""Reads a NeuralAlias status log into samples, events and sessions.

Nothing here is scored. A status log is a build line, then one block per sample
("HH:MM:SS fps=... | section ... | section ..." with further lines for the DX12
host), with "event t=... frame=..." lines in between.
"""
import re

BUILD = re.compile(r"^(?:﻿)?NeuralAlias build ([0-9a-fA-F]{7,40})\s*$", re.M)
SAMPLE = re.compile(r"^(?:\d+:)?(\d\d):(\d\d):(\d\d) fps=([\d.]+)(?: frame_ms=([\d.]+))?", re.M)
EVENT = re.compile(r"^event t=(\d+) frame=(\d+) thread=(\d+) src=(\S+) (\S+) ?(.*)$", re.M)


def clock(seconds):
    if seconds is None:
        return None
    seconds = int(seconds) % 86400
    return "%02d:%02d:%02d" % (seconds // 3600, seconds // 60 % 60, seconds % 60)


def parse(text):
    """-> {"builds": [hash, ...], "samples": [...], "events": [...]}.

    A sample is {"t": seconds of day, "fps", "frame_ms" or None, "text": its block
    without event lines}. `builds` lists every build line in the file, in order:
    more than one different hash means the file mixes runs.
    """
    builds = [m[1].lower() for m in BUILD.finditer(text)]
    events = [{"t_ms": int(m[1]), "frame": int(m[2]), "source": m[4], "name": m[5], "text": m[6].strip()}
              for m in EVENT.finditer(text)]
    marks = list(SAMPLE.finditer(text))
    samples = []
    for i, m in enumerate(marks):
        end = marks[i + 1].start() if i + 1 < len(marks) else len(text)
        block = "\n".join(line for line in text[m.start():end].splitlines() if not line.startswith("event "))
        samples.append({"t": int(m[1]) * 3600 + int(m[2]) * 60 + int(m[3]), "fps": float(m[4]),
                        "frame_ms": float(m[5]) if m[5] else None, "text": block})
    return {"builds": builds, "samples": samples, "events": events}


def series(samples, pattern):
    """One integer per sample from the first group of `pattern`, or None where absent.

    `pattern` may be a list: the counters are added, and a sample missing any of
    them is None (a partial sum is not a count). "^" matches at a line start.
    """
    rxs = [re.compile(p, re.M) for p in ([pattern] if isinstance(pattern, str) else pattern)]
    out = []
    for s in samples:
        found = [rx.search(s["text"]) for rx in rxs]
        out.append(sum(int(m[1]) for m in found) if all(found) else None)
    return out


def group(sample_text, name):
    """The counters inside `name{a=1 b=2}` as a dict, or None if the group is absent."""
    m = re.search(r"\b" + re.escape(name) + r"\{([^}]*)\}", sample_text)
    if not m:
        return None
    return {k: int(v) for k, v in re.findall(r"(\w+)=(-?\d+)\b", m[1])}


def sessions(samples, counters):
    """Split samples where a counter that only counts up goes down (the game or the
    program restarted inside one file). `counters` are lists from series().
    -> list of (first index, last index) inclusive. Counters are never subtracted
    across a split."""
    if not samples:
        return []
    cuts = [0]
    for i in range(1, len(samples)):
        for c in counters:
            if c[i] is not None and c[i - 1] is not None and c[i] < c[i - 1]:
                cuts.append(i)
                break
    cuts.append(len(samples))
    return [(a, b - 1) for a, b in zip(cuts, cuts[1:]) if b - 1 >= a]
