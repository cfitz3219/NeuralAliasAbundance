"""Builds small status logs for the offline tests. No game data."""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


def clock(t):
    return "%02d:%02d:%02d" % (t // 3600, t // 60 % 60, t % 60)


def native11(rows, build=None, fps=100.0, jitter=True, start=12 * 3600, step=5):
    """rows: (presented, scene, delivered) totals per sample, or a dict with more keys."""
    lines = [f"NeuralAlias build {build}"] if build else []
    for i, row in enumerate(rows):
        r = row if isinstance(row, dict) else {"presented": row[0], "scene": row[1], "delivered": row[2]}
        f = r.get("fps", fps)
        text = (f"{clock(start + i * step)} fps={f:.1f} frame_ms={1000.0 / f if f else 0:.2f} | "
                f"native11_route=active installs=1 install_failures=0 menu{{draws={r.get('menu', 0)} failures=0}} | "
                f"native11 frames={r['presented']} fired={r['scene']} missed=0 camera_found={r['delivered']} camera_missing=0 "
                f"pose{{native=0 legacy=0 generic={r['delivered']} no_packet=0 origin={r['scene'] - r['delivered']} split=0}}")
        if jitter:
            text += f" viewport_jitter{{route=1 on=1 frames={r['delivered']} sets=0}}"
        text += f" | native11_dlaa delivered={r['delivered']} last_ms=5 busy=0 rejected=0 preset=13 sharpness={r.get('sharpness', 50)}"
        lines.append(text)
    return "\r\n".join(lines) + "\r\n"


def steady(samples, per_sample=500, delivered_share=1.0, scene_share=1.0, **kw):
    rows = [(i * per_sample, int(i * per_sample * scene_share), int(i * per_sample * scene_share * delivered_share))
            for i in range(samples)]
    return native11(rows, **kw)
