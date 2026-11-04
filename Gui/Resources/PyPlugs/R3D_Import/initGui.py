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


PROBE_VERSION = 2   # bump to discard codes cached by an older probe


def _option_block(text, option):
    """Help text of one option: from '--res' up to the next '--option'."""
    m = re.search(r"(?m)^\s*" + re.escape(option) + r"\b(.*?)(?=^\s*--[A-Za-z]|\Z)", text, re.S)
    return m.group(1) if m else ""


def probe_codes(redline, cfg):
    """Format / resolution codes from `REDline --help`, read only inside the
    --format and --res blocks (the numbers differ between versions). RED's
    documented values are the defaults: --format 2 = OpenEXR, --res 1 full,
    2 half high, 4 quarter, 8 eighth."""
    cached = cfg.get("codes", {})
    if cached.get("_redline") == redline and cached.get("_v") == PROBE_VERSION:
        return cached
    codes = dict(DEFAULT_CODES)
    text = ""
    try:
        out = subprocess.run([redline, "--help"], capture_output=True, text=True, timeout=30,
                             creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
        text = (out.stdout or "") + "\n" + (out.stderr or "")
    except Exception:
        _log("REDline --help failed, using default codes\n" + traceback.format_exc())

    def code(block, word):
        for pat in (r"(\d+)\s*[=:\-]\s*" + word, word + r"\s*[=:\-]\s*(\d+)"):
            m = re.search(pat, block, re.I)
            if m:
                return m.group(1)
        return None
    fmt = _option_block(text, "--format")
    c = code(fmt, r"(?:open\s*)?exr")
    if c:
        codes["exr"] = c
    res = _option_block(text, "--res")
    for key, word in (("full", r"full"), ("half", r"half\s*(?:res\s*)?(?:high|premium)"),
                      ("quarter", r"quarter"), ("eighth", r"eighth")):
        c = code(res, word)
        if c and c != "0":
            codes[key] = c
    codes["_redline"] = redline
    codes["_v"] = PROBE_VERSION
    cfg["codes"] = codes
    _save(cfg)
    _log("REDline codes: " + json.dumps(codes))
    return codes


def _cmdline(args):
    return " ".join('"%s"' % a if " " in a else a for a in args)


def _failure_text(name, rc, logpath):
    """Error text: exit code (crash codes named) + the end of REDline's output."""
    code = rc & 0xFFFFFFFF
    known = {0xC0000005: "REDline crashed (access violation)",
             0xC0000135: "a DLL of REDline is missing",
             0xC0000409: "REDline crashed (stack buffer overrun)"}
    text = "REDline failed on %s (exit code %d / 0x%08X%s): no Read created." % (
        name, rc, code, (", " + known[code]) if code in known else "")
    tail = ""
    try:
        with open(logpath, "r", errors="replace") as fh:
            lines = [l.rstrip() for l in fh.readlines() if l.strip()]
        tail = "\n".join(lines[-12:])
    except Exception:
        pass
    if tail:
        text += "\n\nREDline output (%s):\n%s" % (logpath, tail)
    if code == 0xC0000005:
        text += ("\n\nIf it crashes on any clip: update REDCINE-X PRO (KOMODO / V-RAPTOR clips need "
                 "a recent REDline), or try another decode resolution.")
    return text


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


# ----------------------------------------------------------------------------- progress on the Read

_FRAME_COUNT_RX = re.compile(
    r"(?:total\s*frames?|frame\s*count|number\s*of\s*frames|frames?\s*total|clip\s*frames|num\s*frames|"
    r"total\s*frame\s*count|frames)\s*[:=]?\s*(\d+)", re.I)


def _parse_frame_count(text):
    # "Total Frames: 120" style
    for m in _FRAME_COUNT_RX.finditer(text):
        n = int(m.group(1))
        if n > 0:
            return n
    # CSV style (--printMeta 2/3): header row + value row
    lines = [l for l in text.splitlines() if "," in l]
    for i, line in enumerate(lines[:-1]):
        cols = [c.strip().strip('"').lower() for c in line.split(",")]
        for k, c in enumerate(cols):
            if re.search(r"(total\s*frames|frame\s*count|number\s*of\s*frames)", c):
                vals = lines[i + 1].split(",")
                if k < len(vals) and vals[k].strip().strip('"').isdigit():
                    return int(vals[k].strip().strip('"'))
    # first / last frame numbers
    f = re.search(r"first\s*frame\D{0,10}(\d+)", text, re.I)
    l = re.search(r"last\s*frame\D{0,10}(\d+)", text, re.I)
    if f and l and int(l.group(1)) >= int(f.group(1)):
        return int(l.group(1)) - int(f.group(1)) + 1
    return None


def clip_frame_count(redline, clip):
    """Frames in the clip, from `REDline --printMeta` (None if not found).
    The output is kept in ~/.Natron/r3d_printmeta.txt to check the format."""
    text = ""
    for mode in ("1", "2"):
        try:
            out = subprocess.run([redline, "--i", clip, "--printMeta", mode], capture_output=True, text=True,
                                 timeout=60, creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
            t = (out.stdout or "") + "\n" + (out.stderr or "")
        except Exception:
            _log("REDline --printMeta failed\n" + traceback.format_exc())
            continue
        text += "===== --printMeta %s =====\n%s\n" % (mode, t)
        n = _parse_frame_count(t)
        if n:
            break
    else:
        n = None
    try:
        with open(os.path.join(os.path.dirname(_CFG), "r3d_printmeta.txt"), "w") as fh:
            fh.write(text)
    except Exception:
        pass
    _log("clip frame count: %s" % n)
    return n


def _frames_written(job):
    if not os.path.isdir(job["outdir"]):
        return 0
    rx = re.compile(r"^" + re.escape(job["name"]) + r".*?\d+\.exr$", re.I)
    return sum(1 for f in os.listdir(job["outdir"]) if rx.match(f))


def placeholder_read(name):
    """The Read is created right away; it shows the conversion progress and
    gets the EXR sequence when the conversion ends."""
    node = _app().createNode("fr.inria.built-in.Read")
    if node is None:
        return None
    try:
        node.setLabel(name + " (converting)")
    except Exception:
        pass
    return node.getScriptName()


def _set_read_state(job, progress, label):
    app = _app()
    for name in job.get("reads") or []:
        node = app.getNode(name)
        if node is None:
            continue
        try:
            if hasattr(node, "setProgress"):     # this fork: bar drawn on the node
                node.setProgress(progress)
            node.setLabel(label)
        except Exception:
            pass


def update_progress(job):
    done = _frames_written(job)
    total = job.get("total") or 0
    if total > 0:
        p = min(0.99, done / float(total))
        _set_read_state(job, p, "%s  %d%%" % (job["name"], int(p * 100)))
    else:
        # Total unknown: a bar sweeping every 4 s shows it is working
        import time
        p = (time.time() % 4.0) / 4.0
        _set_read_state(job, p, "%s  %d frames" % (job["name"], done))


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
                update_progress(job)
                continue
            _RUNNING.pop(job["outdir"], None)
            if rc != 0 and not _wrote_frames(job):
                _set_read_state(job, -1, job["name"] + " (conversion failed)")
                _message(_failure_text(job["name"], rc, job.get("log")), error=True)
                continue
            if rc != 0:
                _log("REDline exited with code %d after writing the frames (crash on exit): loading them" % rc)
            if not _finish_job(job):
                _message("REDline finished %s but no EXR was found in %s." % (job["name"], job["outdir"]), error=True)
            else:
                _log("Conversion finished: %s is loaded from its EXR sequence." % job["name"])
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


def _wrote_frames(job):
    """REDline (recent builds) can crash while quitting, after 'Batch Job
    Complete': the frames are there, so the conversion counts as done."""
    if not _sequence_pattern(job["outdir"], job["name"]):
        return False
    log = job.get("log")
    if log and os.path.isfile(log):
        try:
            with open(log, "r", errors="replace") as fh:
                return "batch job complete" in fh.read().lower()
        except Exception:
            pass
    return True   # no log (REDline window mode): the EXR are there


def _finish_job(job):
    """Conversion done: point the Reads that asked for it to the EXR, else create one."""
    if _sequence_pattern(job["outdir"], job["name"]):
        _set_read_state(job, -1, job["name"])
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
    # Frame count for the progress bar (clip range, or start/end when set)
    total = clip_frame_count(redline, opts["clip"])
    if opts["end"] > 0:
        total = opts["end"] - max(0, opts["start"]) + 1
    elif total and opts["start"] > 0:
        total = max(1, total - opts["start"])
    job["total"] = total or 0
    if not opts["wait"] and not job["reads"]:
        ph = placeholder_read(name)
        if ph:
            job["reads"] = [ph]
    logpath = os.path.join(outdir, "redline.log")
    job["log"] = logpath
    if opts["wait"]:
        with open(logpath, "w") as logf:
            logf.write(_cmdline(args) + "\n\n")
            logf.flush()
            res = subprocess.run(args, stdout=logf, stderr=subprocess.STDOUT,
                                 creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
        if res.returncode != 0 and not _wrote_frames(job):
            _message(_failure_text(job["name"], res.returncode, logpath), error=True)
            return
        if not _finish_job(job):
            _message("REDline finished but no EXR was found in " + outdir, error=True)
        return
    # Background, no window: the Read shows the progress (bar on the node,
    # percentage in its label); REDline's output goes to <outdir>/redline.log.
    try:
        logf = open(logpath, "w")
        logf.write(_cmdline(args) + "\n\n")
        logf.flush()
        proc = subprocess.Popen(args, stdout=logf, stderr=subprocess.STDOUT,
                                creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
        logf.close()   # the child keeps its own handle
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
    where = "log: " + logpath
    update_progress(job)
    if _schedule_poll():
        _log("Conversion started (%s). %s automatically when it is finished." % (where, what))
    else:
        _message("Conversion started (%s).\n" % where +
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


def r3d_on_read(path, read_name=""):
    """Called by Natron's Read when its file is a .R3D (Engine/ReadNode.cpp).
    Runs from inside the Read's parameter change: defer the dialog."""
    _READ_REQUESTS.append((path, read_name))
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
            path, read_name = _READ_REQUESTS.pop(0)
            reads = []
            if read_name:                   # the Read that was given the file
                for cand in (read_name, read_name.split(".")[-1]):
                    if app.getNode(cand) is not None:
                        reads.append(cand)
                        break
            for node in app.getChildren():
                try:
                    if node.getPluginID() != "fr.inria.built-in.Read":
                        continue
                    p = node.getParam("filename")
                    if p is not None and _norm(p.getValue()) == _norm(path) \
                            and node.getScriptName() not in [r.split(".")[-1] for r in reads]:
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
