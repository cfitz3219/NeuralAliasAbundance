"""Required jobs per route, and the state of each from one run's measurements.

States: verified, failed, untested, not_applicable, unknown. Missing evidence is
unknown, never a pass. A state from a log says what the program counted; a claim
about the picture needs the owner's verdict (what Cort saw), which is recorded
beside the measurements and never invented.
"""
from measure import value

STATES = ("verified", "failed", "untested", "not_applicable", "unknown")
DEFAULT_FPS_MIN = 40  # Cort, 2026-11-09: the bare minimum with the program on. A case may carry its own budget.

# id, title, routes
JOBS = [
    ("home_menu_opens", "Home menu opens", "AB"),
    ("home_controls_take_effect", "Home controls take effect live", "AB"),
    ("status_truthful", "The status says truthfully what runs", "AB"),
    ("performance_budget", "The case's performance budget is met", "AB"),
    ("presentation_modes", "Windowed, borderless and fullscreen", "AB"),
    ("no_crash_or_hang", "No crash or hang", "AB"),
    ("mode_changes_render_size", "A requested mode changes the game's render size", "A"),
    ("features_do_what_status_claims", "Each enabled feature does what its status claims", "A"),
    ("api_chosen", "Graphics API chosen automatically", "B"),
    ("camera_found", "Camera found", "B"),
    ("depth_found", "Depth found", "B"),
    ("motion_found", "Motion found", "B"),
    ("jitter_applied", "Jitter applied", "B"),
    ("dlaa_delivered", "DLAA delivered by the program's count", "B"),
    ("dlaa_reaches_picture", "DLAA reaches the presented picture", "B"),
    ("continuity", "Continuity", "B"),
    ("cost", "Cost of the program", "B"),
]
JOB_TITLES = {j[0]: j[1] for j in JOBS}
# Jobs only the person at the screen can settle.
# Values in `observed` that are settings a person changes in the Home menu.
SETTINGS = ("preset", "sharpness")
NEEDS_OWNER = {"dlaa_reaches_picture", "home_controls_take_effect"}


def required_jobs(route):
    return [j[0] for j in JOBS if route in j[2]]


def _job(state, note, evidence=()):
    return {"state": state, "note": note, "evidence": list(evidence)}


def score(route, m, observed, case=None, owner_verdicts=None):
    """-> {job id: {"state", "note", "evidence": [measurement names]}} for the route's jobs."""
    case = case or {}
    jobs = {}
    v = lambda name: value(m, name)
    delivered, presented = v("frames_delivered"), v("frames_presented")
    opportunities = v("frames_scene") if v("frames_scene") is not None else presented

    menu = v("menu_draws")
    if menu is None:
        jobs["home_menu_opens"] = _job("unknown", "The log has no menu draw counter.")
    elif menu > 0:
        jobs["home_menu_opens"] = _job("verified", f"The menu was drawn {menu} times.", ["menu_draws"])
    else:
        jobs["home_menu_opens"] = _job("untested", "The menu was not opened in this run.", ["menu_draws"])

    changed = sorted(k for k in SETTINGS if isinstance(observed.get(k), dict) and len(observed[k].get("distinct", [])) > 1)
    if not changed:
        jobs["home_controls_take_effect"] = _job("untested", "No setting changed during this run.")
    elif not delivered:
        jobs["home_controls_take_effect"] = _job(
            "untested", "A setting changed (" + ", ".join(changed) + "), but DLSS was not delivering, so nothing could take effect.")
    else:
        jobs["home_controls_take_effect"] = _job(
            "unknown", "The program took the change without a restart: " + ", ".join(changed) +
            ". Whether the picture changed is for the person at the screen.", ["frames_delivered"])

    jobs["status_truthful"] = _job("unknown", "No cross-check of the status text against the counters exists yet.")

    budget = (case.get("budget") or {}).get("fps_min", DEFAULT_FPS_MIN)
    fps = v("fps_play_median")
    evidence = ["fps_play_median", "fps_median_while_delivering", "not_delivering_share_of_play"]
    if fps is None:
        jobs["performance_budget"] = _job("unknown", "No fps samples during play.")
    elif fps < budget:
        jobs["performance_budget"] = _job("failed", f"Median {fps} fps over all play samples against a budget of {budget}.", evidence)
    elif route == "B" and v("fps_median_while_delivering") is None:
        # The budget is for the program doing its work; fps without DLSS running does not test it.
        jobs["performance_budget"] = _job("untested", f"Median {fps} fps, but no sample had DLSS delivering, so the budget "
                                          f"of {budget} was not tested.", evidence)
    else:
        jobs["performance_budget"] = _job("verified", f"Median {fps} fps over all play samples against a budget of {budget}.", evidence)

    jobs["presentation_modes"] = _job("unknown", "This log carries no evidence of the presentation mode.")
    jobs["no_crash_or_hang"] = _job("unknown", "A log without a crash line does not prove a clean exit.")

    if route == "A":
        jobs["mode_changes_render_size"] = _job("unknown", "Route A scoring has not been checked against a saved run yet.")
        jobs["features_do_what_status_claims"] = _job("unknown", "Route A scoring has not been checked against a saved run yet.")
    if route == "B":
        if presented:
            jobs["api_chosen"] = _job("verified", "The provider for the game's own API saw its frames.", ["frames_presented"])
        else:
            jobs["api_chosen"] = _job("unknown", "No presented frames were counted.")
        if delivered is None:
            for job in ("camera_found", "depth_found", "motion_found", "dlaa_delivered"):
                jobs[job] = _job("unknown", "The log has no delivered-frame counter.")
        elif delivered > 0:
            found = v("camera_found_by")
            how = (" Found by: " + ", ".join(f"{k} {n}" for k, n in sorted(found.items())) + ".") if found else ""
            jobs["camera_found"] = _job("verified", f"A camera was admitted on the {delivered} delivered frames." + how +
                                        " Whether it is the right camera shows in the picture.", ["frames_delivered", "camera_found_by"])
            for job, what in (("depth_found", "Depth"), ("motion_found", "Motion")):
                jobs[job] = _job("verified", f"{what} was supplied on the {delivered} delivered frames. "
                                 "Its source and correctness are not in this log.", ["frames_delivered"])
            jobs["dlaa_delivered"] = _job("verified", f"{delivered} frames by the program's count.",
                                          ["frames_delivered", "delivered_share_of_scene", "delivered_share_of_presented"])
        elif opportunities:
            why = v("missed_because")
            note = "Nothing was delivered." + ((" Reasons: " + ", ".join(f"{k} {n}" for k, n in sorted(why.items())) + ".") if why else
                                              " The log gives no reason (a telemetry gap).")
            for job in ("camera_found", "depth_found", "motion_found"):
                jobs[job] = _job("unknown", note + " Which input was missing is not settled by this log.", ["missed_because"])
            no_camera = v("frames_without_camera")
            if no_camera is not None and no_camera >= 0.5 * opportunities:
                lost = v("camera_scan_losses")
                how = (" Scans came back empty for: " + ", ".join(f"{k} {n}" for k, n in sorted(lost.items())) + ".") if lost else ""
                jobs["camera_found"] = _job("failed", f"No camera on {no_camera} of {opportunities} scene frames." + how,
                                            ["frames_without_camera", "camera_scan_losses"])
                for job in ("depth_found", "motion_found"):
                    jobs[job] = _job("unknown", "Nothing was delivered because the camera was missing. "
                                     "This log does not show whether this input was ready.")
            jobs["dlaa_delivered"] = _job("failed", note, ["frames_delivered", "missed_because"])
        else:
            for job in ("camera_found", "depth_found", "motion_found", "dlaa_delivered"):
                jobs[job] = _job("untested", "The run had no delivery opportunities.")
        jittered = v("frames_jittered")
        if jittered is None:
            jobs["jitter_applied"] = _job("unknown", "The log has no jitter counter.")
        elif jittered > 0:
            jobs["jitter_applied"] = _job("verified", f"{jittered} frames carried jitter.", ["frames_jittered"])
        elif delivered:
            jobs["jitter_applied"] = _job("failed", "DLAA was delivered with no jittered frame. DLAA without jitter does not count as working.",
                                          ["frames_jittered", "frames_delivered"])
        else:
            jobs["jitter_applied"] = _job("untested", "Nothing was delivered, so jitter was not exercised.")
        if delivered:
            jobs["dlaa_reaches_picture"] = _job(
                "unknown", "The program's count shows DLSS ran and its output was handed back. Only the person at the screen "
                "can say it reached the picture (for example the DLSS indicator showing).", ["frames_delivered"])
        elif delivered == 0 and opportunities:
            jobs["dlaa_reaches_picture"] = _job("failed", "Nothing was delivered, so nothing reached the picture.", ["frames_delivered"])
        else:
            jobs["dlaa_reaches_picture"] = _job("unknown", "The log does not show a delivery to judge.")
        gap = v("longest_gap_seconds")
        limit = (case.get("budget") or {}).get("longest_gap_seconds_max")
        if delivered == 0 and opportunities:
            jobs["continuity"] = _job("untested", "Nothing was delivered, so there was no continuity to measure.", ["frames_delivered"])
        elif gap is None:
            jobs["continuity"] = _job("unknown", "No gap could be measured.")
        elif limit is None:
            jobs["continuity"] = _job("unknown", f"Longest gap {gap} s (sampled). The case sets no limit; the floor comparison judges it.",
                                      ["longest_gap_seconds", "not_delivering_share_of_play"])
        elif m["longest_gap_seconds"].get("basis") == "sampled" and gap <= limit:
            jobs["continuity"] = _job("unknown", f"The sampled gap estimate is {gap} s. Periodic totals cannot establish visible continuity.",
                                     ["longest_gap_seconds", "not_delivering_share_of_play"])
        else:
            jobs["continuity"] = _job("verified" if gap <= limit else "failed", f"Longest gap {gap} s (sampled) against a limit of {limit} s.",
                                      ["longest_gap_seconds", "not_delivering_share_of_play"])
        jobs["cost"] = _job("unknown", "The run has no interval with the program switched off to compare against.")

    for verdict in owner_verdicts or []:
        job = verdict.get("job")
        if job in jobs and verdict.get("state") in STATES:
            jobs[job] = _job(verdict["state"], f"{verdict.get('by', 'Owner')}: \"{verdict.get('said', '')}\"", jobs[job]["evidence"])
            jobs[job]["source"] = "owner_verdict"
    return {j: jobs[j] for j in required_jobs(route) if j in jobs}
