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


def dx12(rows, build=None, fps=120.0, start=12 * 3600, step=5, route="inline", jitter_mode=1):
    """A DX12 status log in the shape of the two saved runs the reader was checked
    against (one line per section, the sample line first). rows: dicts with running
    totals: presented, scene, delivered; optional no_camera, inline (frames delivered
    inside the scene list), jittered_inline, declared, held, lost_no_vp, menu,
    sharpness, fps."""
    lines = [f"NeuralAlias build {build}"] if build else []
    for i, r in enumerate(rows):
        f = r.get("fps", fps)
        inline = r.get("inline", r["delivered"] if route == "inline" else 0)
        lines += [
            f"{clock(start + i * step)} fps={f:.1f} frame_ms={1000.0 / f if f else 0:.2f} dlss_gpu_ms=0.00 evaluates={r['delivered']} creates=1 menu=0 | nr on=0",
            f"overlay_source=native native_presents={r['presented']} presents={r['presented']} pid=1 queue_hits=0 buffer_queue=0 queue_misses=0",
            f"j2 on=1 active={1 if r['delivered'] else 0} depth=298 color=297 submissions={r['presented'] * 4} frames={r['delivered']} no_selection=0 "
            f"no_scene_list={r['presented'] * 2} no_state=0 (colour 0 depth 0) no_camera={r.get('no_camera', 0)} busy=0 dlss_failed=0 cuts=0 "
            f"preset=13 sharpness={r.get('sharpness', 50)} inl_frames={inline} \"status text\"",
            f"viewport_jitter mode={jitter_mode} frames={r['delivered']} declared_list={r.get('declared', r['delivered'] - inline)} declared_submitted=0 declared_none=0",
            f"j2_camera contested=0 alternatives=0 lost_unread=0 lost_no_vp={r.get('lost_no_vp', 0)} lost_no_inverse=0 lost_capped=0 other_cam=0",
            f"j2_misses no_dlaa{{no_camera=0 no_state=0 no_candidate=0 other=0}} unjittered{{no_sample=0 not_applied=0 not_held=0}} "
            f"camera_held={r.get('held', 0)} extrapolated=0 depth_assumed=0 camera_from_j1=0",
            # Counts the inline route only: zero on the between-list route whatever was delivered.
            f"j2_coverage presents={r['presented'] if route == 'inline' else 0} covered={inline} later_writes=0 "
            f"jittered={r.get('jittered_inline', inline)} jitter=(0.2500,-0.1250)",
            f"j2_route mode=auto route={route} state=active matches=1 observed=1 gen=1 changes=1 scene_frames={r['scene']} "
            f"executed={r['delivered']} placement_consistent={r['delivered']} contradictions=0 unstable=0",
            f"j2_summary scene_frames={r['scene']} missed={r['scene'] - r['delivered']} placed_before_scene_work=0 route_changes=1",
            f"menu_gpu draws={r.get('menu', 0)} submitted=0 completed=0",
        ]
    return "\r\n".join(lines) + "\r\n"
