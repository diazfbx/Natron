# -*- coding: utf-8 -*-
"""R3D import for Natron through REDline - menu "R3D".

Natron cannot decode .R3D (RED's format is closed). RED's free command-line
tool REDline (installed with REDCINE-X PRO, under RED's own licence) converts
a clip to an OpenEXR sequence that Natron reads natively. This PyPlug only
*calls* REDline: no RED SDK code, headers or libraries are used or shipped.

A Read given a .R3D (file dialog, drag & drop) calls r3d_on_read(): same
options dialog, then the Read switches to the EXR sequence when REDline is done.

Entry points: File > Import R3D (REDline)... (r3d_import), a Read given a
.R3D (r3d_on_read). The Read is created / switched to the EXR automatically
when REDline finishes; if REDline is missing or fails to start, its location
is asked for, or its download page offered.

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
    if w:
        return w
    # Any REDCINE-X PRO / RED install folder (the name changes between versions).
    import glob
    roots = [os.environ.get("ProgramFiles", r"C:\Program Files"),
             os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)"),
             os.environ.get("ProgramW6432", r"C:\Program Files")]
    for root in roots:
        for pat in ("RED*", "REDCINE*", os.path.join("RED*", "*")):
            for d in glob.glob(os.path.join(root, pat)):
                for exe in glob.glob(os.path.join(d, "REDline*.exe")):
                    return exe
    return None


DOWNLOAD_URL = "https://www.red.com/downloads"


def _ask_download():
    """REDline missing: offer RED's download page. True if the page was opened."""
    msg = ('<p>REDline was not found.</p>'
           '<p>R3D import needs <b>REDCINE-X PRO</b>, RED\'s free software that installs REDline. '
           'Download it here: <a href="%s">%s</a> (Windows, free RED account).</p>'
           '<p><b>Yes</b>: open the download page. <b>No</b>: REDCINE-X PRO is installed, '
           'select REDline.exe yourself.</p>') % (DOWNLOAD_URL, DOWNLOAD_URL)
    _log("REDline not found. Download REDCINE-X PRO: " + DOWNLOAD_URL)
    try:
        import NatronGui
        ret = NatronGui.natron.questionDialog("R3D Import - REDCINE-X PRO needed", msg)
    except Exception:
        ret = None
    if ret is not None and "yes" in str(ret).lower():
        try:
            import webbrowser
            webbrowser.open(DOWNLOAD_URL)
        except Exception:
            pass
        return True
    return False


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


POLL_MS = 2000


def _schedule_poll():
    """Check the running conversions again in POLL_MS, on the main thread
    (NatronGui.natron.callLater, this fork). False on a Natron without it."""
    try:
        import NatronGui
        if not hasattr(NatronGui.natron, "callLater"):
            return False
        NatronGui.natron.callLater(POLL_MS, "r3d_poll")
        return True
    except Exception:
        _log("callLater failed\n" + traceback.format_exc())
        return False


def r3d_poll():
    """Create the Read of each conversion that just finished; re-arm while some run."""
    try:
        cfg = _load()
        jobs = cfg.get("pending", [])
        left, still = [], False
        for job in jobs:
            proc = _RUNNING.get(job["outdir"])
            if proc is None:
                left.append(job)            # started in another session: manual menu
                continue
            rc = proc.poll()
            if rc is None:
                still = True
                left.append(job)
                continue
            _RUNNING.pop(job["outdir"], None)
            if rc != 0:
                _message("REDline failed on %s (exit code %d): no Read created." % (job["name"], rc), error=True)
                continue
            if not _finish_job(job):
                _message("REDline finished %s but no EXR was found in %s." % (job["name"], job["outdir"]), error=True)
            else:
                _message("Conversion finished: %s is loaded from its EXR sequence." % job["name"])
        cfg["pending"] = left
        _save(cfg)
        if still:
            _schedule_poll()
    except Exception:
        _log("poll failed\n" + traceback.format_exc())


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
        if _finish_job(job):
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


def _get_redline(cfg):
    redline = find_redline(cfg)
    if not redline:
        if _ask_download():
            return None
        redline = ask_redline(cfg)
    return redline


def _finish_job(job):
    """Conversion done: point the Reads that asked for it to the EXR, else create one."""
    reads = job.get("reads") or []
    if not reads:
        return create_read(job) is not None
    pattern = _sequence_pattern(job["outdir"], job["name"])
    if not pattern:
        return False
    app = _app()
    done = False
    for name in reads:
        node = app.getNode(name)
        if node is None:
            continue
        p = node.getParam("filename")
        if p is not None:
            p.setValue(pattern)
            done = True
            _log("%s -> %s" % (name, pattern))
    if not done:   # the Read was deleted meanwhile: make a new one
        return create_read(job) is not None
    return True


def start_conversion(clip, reads=None):
    """Options dialog, then REDline. reads: script names of Reads to repoint when done."""
    app = _app()
    cfg = _load()
    redline = _get_redline(cfg)
    if not redline:
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
    job = {"name": name, "outdir": outdir, "clip": opts["clip"], "ocio": COLORS[opts["color"]][2],
           "reads": list(reads or [])}
    if opts["wait"]:
        res = subprocess.run(args, creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
        if res.returncode != 0:
            _message("REDline failed (exit %d). Command:\n%s" % (res.returncode, " ".join(args)), error=True)
            return
        if not _finish_job(job):
            _message("REDline finished but no EXR was found in " + outdir, error=True)
        return
    # Background: REDline's own console shows the progress.
    try:
        proc = subprocess.Popen(args, creationflags=getattr(subprocess, "CREATE_NEW_CONSOLE", 0))
    except OSError:
        cfg.pop("redline", None)
        _save(cfg)
        _message("REDline could not be started (%s)." % redline, error=True)
        if not _ask_download() and ask_redline(cfg):
            start_conversion(clip, reads)
        return
    _RUNNING[outdir] = proc
    cfg = _load()
    cfg.setdefault("pending", [])
    cfg["pending"] = [j for j in cfg["pending"] if j["outdir"] != outdir] + [job]
    _save(cfg)
    what = "The Read switches to the EXR sequence" if reads else "The Read is created"
    if _schedule_poll():
        _message("Conversion started (see the REDline window).\n" + what + " automatically when it is finished.")
    else:
        _message("Conversion started (see the REDline window).\n"
                 "Natron cannot watch it (needs this fork's callLater): load the EXR from " + outdir + " when it is done.")


def import_r3d():
    try:
        app = _app()
        cfg = _load()
        if not _get_redline(cfg):
            return
        clip = app.getFilenameDialog(["R3D", "r3d"])
        if clip:
            start_conversion(clip)
    except Exception:
        _message("R3D import failed:\n" + traceback.format_exc(), error=True)


# ----------------------------------------------------------------------------- Read hook

_READ_REQUESTS = []


def _first_segment(path):
    """A spanned clip picked as a sequence (A001_C002_..._###.R3D): its first file."""
    if "#" not in path and "%" not in path:
        return path
    import glob
    pat = re.sub(r"#+", lambda m: "[0-9]" * len(m.group(0)), path)
    pat = re.sub(r"%0?(\d*)d", lambda m: "[0-9]" * int(m.group(1) or 1), pat)
    files = sorted(glob.glob(pat))
    return files[0] if files else path


def _norm(p):
    return os.path.normcase(os.path.normpath(p)) if p else ""


def r3d_on_read(path):
    """Called by Natron's Read when its file is a .R3D (Engine/ReadNode.cpp).
    Runs from inside the Read's parameter change: defer the dialog."""
    _READ_REQUESTS.append(path)
    try:
        import NatronGui
        if hasattr(NatronGui.natron, "callLater"):
            NatronGui.natron.callLater(0, "r3d_process_reads")
            return
    except Exception:
        pass
    r3d_process_reads()


def r3d_process_reads():
    try:
        app = _app()
        while _READ_REQUESTS:
            path = _READ_REQUESTS.pop(0)
            reads = []
            for node in app.getChildren():
                try:
                    if node.getPluginID() != "fr.inria.built-in.Read":
                        continue
                    p = node.getParam("filename")
                    if p is not None and _norm(p.getValue()) == _norm(path):
                        reads.append(node.getScriptName())
                except Exception:
                    continue
            clip = _first_segment(path)
            # Already converted with the default folder: use it without asking.
            base = os.path.splitext(os.path.basename(clip))[0]
            outdir = os.path.join(os.path.dirname(clip), base + "_EXR")
            if reads and _sequence_pattern(outdir, base):
                _finish_job({"name": base, "outdir": outdir, "clip": clip, "reads": reads})
                continue
            if any(_norm(j["clip"]) == _norm(clip) and j["outdir"] in _RUNNING for j in _load().get("pending", [])):
                continue   # this clip is already converting
            start_conversion(clip, reads)
    except Exception:
        _message("R3D Read failed:\n" + traceback.format_exc(), error=True)


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
    __main__.r3d_poll = r3d_poll
    __main__.r3d_on_read = r3d_on_read
    __main__.r3d_process_reads = r3d_process_reads
    # No menu of its own: File > Import R3D (REDline)... (Gui.cpp) calls
    # r3d_import(); a Read given a .R3D calls r3d_on_read().


try:
    _r3d_register()
except Exception:
    _log("menu registration failed\n" + traceback.format_exc())
