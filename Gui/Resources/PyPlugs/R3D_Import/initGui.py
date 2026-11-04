# -*- coding: utf-8 -*-
"""R3D import for Natron through REDline - menu "R3D".

Natron cannot decode .R3D (RED's format is closed). RED's free command-line
tool REDline (installed with REDCINE-X PRO, under RED's own licence) converts
a clip to an OpenEXR sequence that Natron reads natively. This PyPlug only
*calls* REDline: no RED SDK code, headers or libraries are used or shipped.

Menu:
  R3D > Import R3D (REDline)...            convert a clip, then create its Read
  R3D > Create Reads (finished conversions) Reads for conversions run in the background
  R3D > Set REDline location...

Settings (REDline path, codes read from `REDline --help`, pending jobs):
~/.Natron/r3d_import.json
"""
from __future__ import print_function

import json
import os
import re
import shlex
import shutil
import subprocess
import sys
import traceback

import NatronEngine

_CFG = os.path.join(os.path.expanduser("~"), ".Natron", "r3d_import.json")
_RUNNING = {}   # outdir -> Popen, for this session

REDLINE_CANDIDATES = [
    r"C:\Program Files\REDCINE-X PRO 64-bit\REDline.exe",
    r"C:\Program Files\REDCINE-X PRO\REDline.exe",
    r"C:\Program Files (x86)\REDCINE-X PRO\REDline.exe",
    "/Applications/REDCINE-X Professional/REDCINE-X PRO.app/Contents/MacOS/REDline",
    "/usr/local/bin/REDline",
    "/opt/REDCINE-X PRO/REDline",
]

# Output colour: label, REDline arguments, OCIO space for the Read (first one the config has)
COLORS = [
    ("ACES2065-1 linear (AP0)", ["--exrACES", "1"], ["ACES2065-1", "ACES - ACES2065-1", "aces"]),
    ("REDWideGamutRGB linear", ["--colorSpace", "25", "--gammaCurve", "-1"], []),
    ("REDWideGamutRGB / Log3G10", ["--colorSpace", "25", "--gammaCurve", "34"],
     ["RED Log3G10 REDWideGamutRGB", "Log3G10 REDWideGamutRGB", "REDLog3G10 REDWideGamutRGB"]),
    ("Rec.709 linear", ["--colorSpace", "13", "--gammaCurve", "-1"],
     ["Linear Rec.709 (sRGB)", "Linear Rec.709", "lin_rec709", "Utility - Linear - sRGB", "linear"]),
]
# EXR compression: label, value (OpenEXR's enum order, as REDline numbers them)
COMPRESSIONS = [
    ("PIZ (lossless, best for grain)", 4),
    ("ZIP (lossless)", 3),
    ("ZIPS (lossless, scanline)", 2),
    ("None", 0),
    ("DWAA (lossy, small: not for keying)", 8),
]
RESOLUTIONS = [("Full", "full"), ("1/2", "half"), ("1/4", "quarter"), ("1/8", "eighth")]
DEFAULT_CODES = {"exr": "2", "full": "1", "half": "2", "quarter": "4", "eighth": "8"}


def _log(msg):
    print("[R3D Import] " + str(msg))


def _message(text, error=False):
    _log(text)
    try:
        import NatronGui
        if error:
            NatronGui.natron.errorDialog("R3D Import", text)
        else:
            NatronGui.natron.informationDialog("R3D Import", text)
    except Exception:
        pass


def _load():
    try:
        with open(_CFG, "r") as fh:
            return json.load(fh)
    except Exception:
        return {}


def _save(cfg):
    try:
        os.makedirs(os.path.dirname(_CFG), exist_ok=True)
        with open(_CFG, "w") as fh:
            json.dump(cfg, fh, indent=2)
    except Exception:
        _log("cannot save " + _CFG + "\n" + traceback.format_exc())


def _app():
    import NatronGui
    try:
        n = NatronGui.natron.getNumInstances()
    except Exception:
        n = 1
    for i in range(max(1, n)):
        a = NatronGui.natron.getGuiInstance(i)
        if a is not None:
            return a
    return NatronEngine.natron.getActiveInstance()


# ----------------------------------------------------------------------------- REDline

def find_redline(cfg):
    p = cfg.get("redline")
    if p and os.path.isfile(p):
        return p
    for c in REDLINE_CANDIDATES:
        if os.path.isfile(c):
            return c
    w = shutil.which("REDline") or shutil.which("REDline.exe")
    return w


def ask_redline(cfg):
    app = _app()
    path = ""
    try:
        path = app.getFilenameDialog(["exe"] if os.name == "nt" else [], "C:/Program Files")
    except Exception:
        pass
    if path and os.path.isfile(path):
        cfg["redline"] = path
        cfg.pop("codes", None)
        _save(cfg)
        return path
    return None


def probe_codes(redline, cfg):
    """Format / resolution codes from `REDline --help` (they differ between versions)."""
    if cfg.get("codes", {}).get("_redline") == redline:
        return cfg["codes"]
    codes = dict(DEFAULT_CODES)
    try:
        out = subprocess.run([redline, "--help"], capture_output=True, text=True, timeout=30,
                             creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
        text = (out.stdout or "") + "\n" + (out.stderr or "")

        def code(patterns):
            for pat in patterns:
                m = re.search(pat, text, re.I)
                if m:
                    return m.group(1)
            return None
        c = code([r"(\d+)\s*[=:\-]\s*(?:open\s*)?exr", r"(?:open\s*)?exr\s*[=:\-]\s*(\d+)"])
        if c:
            codes["exr"] = c
        for key, words in (("full", r"full"), ("half", r"half(?:\s*res)?\s*(?:high|premium)"),
                           ("quarter", r"quarter"), ("eighth", r"eighth")):
            c = code([r"(\d+)\s*[=:\-]\s*" + words, words + r"\s*[=:\-]\s*(\d+)"])
            if c:
                codes[key] = c
    except Exception:
        _log("REDline --help failed, using default codes\n" + traceback.format_exc())
    codes["_redline"] = redline
    cfg["codes"] = codes
    _save(cfg)
    return codes


def build_args(redline, codes, clip, outdir, name, res_key, color_idx, comp_val,
               renum, start, end, use_rmd, extra):
    args = [redline, "--i", clip, "--outDir", outdir, "--o", name,
            "--format", codes["exr"], "--res", codes[res_key],
            "--exrCompression", str(comp_val), "--pad", "4"]
    args += COLORS[color_idx][1]
    if renum >= 0:
        args += ["--renum", str(renum)]
    if start > 0:
        args += ["--start", str(start)]
    if end > 0:
        args += ["--end", str(end)]
    if use_rmd:
        args += ["--useRMD", "1"]
    if extra.strip():
        args += shlex.split(extra.replace("\\", "/"))   # paths: forward slashes
    return args


# ----------------------------------------------------------------------------- Reads

def _sequence_pattern(outdir, name):
    """'<outdir>/<name>.####.exr'-like pattern from the files REDline wrote."""
    if not os.path.isdir(outdir):
        return None
    rx = re.compile(r"^(" + re.escape(name) + r".*?)(\d+)(\.exr)$", re.I)
    for f in sorted(os.listdir(outdir)):
        m = rx.match(f)
        if m:
            return os.path.join(outdir, m.group(1) + "#" * len(m.group(2)) + m.group(3)).replace("\\", "/")
    return None


def create_read(job):
    pattern = _sequence_pattern(job["outdir"], job["name"])
    if not pattern:
        return None
    app = _app()
    node = app.createNode("fr.inria.built-in.Read")
    if node is None:
        return None
    try:
        node.setLabel(job["name"])
    except Exception:
        pass
    p = node.getParam("filename")
    if p is not None:
        p.setValue(pattern)
    # Read's input colour space (OCIO) when the config has a matching name
    space = node.getParam("ocioInputSpace")
    if space is not None:
        for cand in job.get("ocio", []):
            try:
                if hasattr(space, "setValue"):
                    space.setValue(cand)
                    break
            except Exception:
                continue
    _log("Read created: " + pattern)
    return node


def create_pending_reads():
    cfg = _load()
    jobs = cfg.get("pending", [])
    left, made, running = [], [], []
    for job in jobs:
        proc = _RUNNING.get(job["outdir"])
        if proc is not None and proc.poll() is None:
            running.append(job["name"])
            left.append(job)
            continue
        if create_read(job) is not None:
            made.append(job["name"])
            _RUNNING.pop(job["outdir"], None)
        else:
            left.append(job)
    cfg["pending"] = left
    _save(cfg)
    msg = []
    if made:
        msg.append("Reads created: " + ", ".join(made))
    if running:
        msg.append("Still converting: " + ", ".join(running))
    if not msg:
        msg.append("No finished conversion to load." if not left else
                   "No EXR found yet for: " + ", ".join(j["name"] for j in left))
    _message("\n".join(msg))


# ----------------------------------------------------------------------------- dialog

def _dialog(app, clip):
    d = app.createModalDialog()
    f = d.createStringParam("clip", "R3D clip")
    f.setValue(clip)
    r = d.createChoiceParam("res", "Decode resolution")
    for lab, _ in RESOLUTIONS:
        r.addOption(lab, "")
    c = d.createChoiceParam("color", "Output colour")
    for lab, _, _ in COLORS:
        c.addOption(lab, "")
    z = d.createChoiceParam("comp", "EXR compression")
    for lab, _ in COMPRESSIONS:
        z.addOption(lab, "")
    o = d.createStringParam("outdir", "Output folder")
    base = os.path.splitext(os.path.basename(clip))[0]
    o.setValue(os.path.join(os.path.dirname(clip), base + "_EXR").replace("\\", "/"))
    n = d.createIntParam("renum", "First frame number")
    n.setValue(1001)
    s = d.createIntParam("start", "Clip start frame (0 = first)")
    s.setValue(0)
    e = d.createIntParam("end", "Clip end frame (0 = last)")
    e.setValue(0)
    m = d.createBooleanParam("rmd", "Use the clip's RMD sidecar (look from REDCINE-X)")
    m.setValue(False)
    x = d.createStringParam("extra", "Extra REDline arguments")
    x.setValue("")
    w = d.createBooleanParam("wait", "Wait for the conversion (Natron is blocked)")
    w.setValue(False)
    d.refreshUserParamsGUI()
    ok = d.exec() if hasattr(d, "exec") else d.exec_()
    if not ok:
        return None
    return {
        "clip": d.getParam("clip").getValue(),
        "res": RESOLUTIONS[d.getParam("res").getValue()][1],
        "color": d.getParam("color").getValue(),
        "comp": COMPRESSIONS[d.getParam("comp").getValue()][1],
        "outdir": d.getParam("outdir").getValue(),
        "renum": d.getParam("renum").getValue(),
        "start": d.getParam("start").getValue(),
        "end": d.getParam("end").getValue(),
        "rmd": d.getParam("rmd").getValue(),
        "extra": d.getParam("extra").getValue(),
        "wait": d.getParam("wait").getValue(),
    }


def import_r3d():
    try:
        app = _app()
        cfg = _load()
        redline = find_redline(cfg)
        if not redline:
            _message("REDline was not found. It comes with REDCINE-X PRO (free, red.com). "
                     "Install it, or point to REDline.exe in the next dialog.")
            redline = ask_redline(cfg)
            if not redline:
                return
        clip = app.getFilenameDialog(["R3D", "r3d"])
        if not clip:
            return
        opts = _dialog(app, clip)
        if not opts:
            return
        codes = probe_codes(redline, cfg)
        name = os.path.splitext(os.path.basename(opts["clip"]))[0]
        outdir = opts["outdir"]
        os.makedirs(outdir, exist_ok=True)
        args = build_args(redline, codes, opts["clip"], outdir, name, opts["res"], opts["color"],
                          opts["comp"], opts["renum"], opts["start"], opts["end"],
                          opts["rmd"], opts["extra"])
        _log(" ".join('"%s"' % a if " " in a else a for a in args))
        job = {"name": name, "outdir": outdir, "clip": opts["clip"], "ocio": COLORS[opts["color"]][2]}
        if opts["wait"]:
            res = subprocess.run(args, creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
            if res.returncode != 0:
                _message("REDline failed (exit %d). Command:\n%s" % (res.returncode, " ".join(args)), error=True)
                return
            if create_read(job) is None:
                _message("REDline finished but no EXR was found in " + outdir, error=True)
            return
        # Background: REDline's own console shows the progress.
        proc = subprocess.Popen(args, creationflags=getattr(subprocess, "CREATE_NEW_CONSOLE", 0))
        _RUNNING[outdir] = proc
        cfg = _load()
        cfg.setdefault("pending", [])
        cfg["pending"] = [j for j in cfg["pending"] if j["outdir"] != outdir] + [job]
        _save(cfg)
        _message("Conversion started (see the REDline window).\n"
                 "When it is finished: R3D > Create Reads (finished conversions).")
    except Exception:
        _message("R3D import failed:\n" + traceback.format_exc(), error=True)


def r3d_import():
    import_r3d()


def r3d_create_reads():
    try:
        create_pending_reads()
    except Exception:
        _message("Could not create the Reads:\n" + traceback.format_exc(), error=True)


def r3d_set_redline():
    cfg = _load()
    p = ask_redline(cfg)
    if p:
        _message("REDline: " + p)


def _r3d_register():
    import __main__
    import NatronGui
    __main__.NatronGui = NatronGui
    __main__.NatronEngine = NatronEngine
    __main__.r3d_import = r3d_import
    __main__.r3d_create_reads = r3d_create_reads
    __main__.r3d_set_redline = r3d_set_redline
    if getattr(NatronGui, "_r3d_menu_done", False):
        return
    NatronGui.natron.addMenuCommand("R3D/Import R3D (REDline)...", "r3d_import")
    NatronGui.natron.addMenuCommand("R3D/Create Reads (finished conversions)", "r3d_create_reads")
    NatronGui.natron.addMenuCommand("R3D/Set REDline location...", "r3d_set_redline")
    NatronGui._r3d_menu_done = True


try:
    _r3d_register()
except Exception:
    _log("menu registration failed\n" + traceback.format_exc())
