# -*- coding: utf-8 -*-
"""ARRIRAW and Blackmagic RAW import for Natron (this fork).

Natron has no decoder for these camera RAW formats. Like the R3D import
(REDline), this module only *runs* an external converter that writes an
OpenEXR sequence, then points a Read at it. No ARRI or Blackmagic SDK code,
header or library is part of Natron.

  ARRIRAW (.ari, .arx, ARRIRAW .mxf)  ->  art-cmd, the command-line ARRI
      Reference Tool (free, from arri.com: ART CMD).
  Blackmagic RAW (.braw)              ->  braw2exr, a small MIT tool in
      tools/braw2exr, built once against the free Blackmagic RAW SDK.

Entry points (registered in __main__ by initGui.py):
  camraw_import_arri()   File > Import ARI / ARX / MXF (ARRI)...
  camraw_import_braw()   File > Import BRAW (Blackmagic)...
  camraw_on_read(path)   a Read given a .ari / .arx / .braw (Engine/ReadNode.cpp)
  camraw_poll()          watches background conversions (NatronGui.natron.callLater)

Settings and pending conversions: ~/.Natron/camraw_import.json
"""
from __future__ import print_function

import glob
import json
import os
import re
import shlex
import shutil
import subprocess
import traceback

import NatronEngine

_CFG = os.path.join(os.path.expanduser("~"), ".Natron", "camraw_import.json")
_RUNNING = {}          # outdir -> Popen (this session)
_READ_REQUESTS = []    # paths handed over by Reads, processed on the next event loop turn
POLL_MS = 2000

_NO_WINDOW = getattr(subprocess, "CREATE_NO_WINDOW", 0)
_NEW_CONSOLE = getattr(subprocess, "CREATE_NEW_CONSOLE", 0)


# ============================================================================ formats

ARRI = {
    "key": "arri",
    "label": "ARRIRAW",
    "tool": "art-cmd",
    "exts": ["ari", "arx", "mxf"],
    "read_exts": ["ari", "arx"],           # Reads hand these over (.mxf may be ProRes: menu only)
    "candidates": [
        r"C:\Program Files\Arri_CMD\bin\art-cmd.exe",
        r"C:\Program Files\ARRI\ARRI Reference Tool CMD\bin\art-cmd.exe",
        r"C:\Program Files\ARRI\art-cmd\bin\art-cmd.exe",
        "/usr/Arri_CMD/bin/art-cmd",
        "/usr/local/bin/art-cmd",
        "/Applications/ARRI Reference Tool CMD/bin/art-cmd",
    ],
    "globs": [os.path.join("Arri*", "**", "art-cmd*.exe"), os.path.join("ARRI*", "**", "art-cmd*.exe")],
    "names": ["art-cmd", "art-cmd.exe"],
    "url": "https://www.arri.com/en/learn-help/learn-help-camera-system/tools/arri-reference-tool",
    "download_html": ("<p><b>art-cmd</b> (ARRI Reference Tool, command line) was not found.</p>"
                      "<p>ARRIRAW import needs <b>ART CMD</b>, ARRI's free command-line tool "
                      "(Windows, macOS, Linux). Download it here: <a href=\"%(url)s\">%(url)s</a> "
                      "(section ARRI Reference Tool, <i>CMD</i>).</p>"
                      "<p><b>Yes</b>: open the download page. <b>No</b>: ART CMD is installed, "
                      "select art-cmd.exe yourself.</p>"),
}

BRAW = {
    "key": "braw",
    "label": "Blackmagic RAW",
    "tool": "braw2exr",
    "exts": ["braw"],
    "read_exts": ["braw"],
    "candidates": [],                      # + Natron's bin folder, see _tool_candidates()
    "globs": [],
    "names": ["braw2exr", "braw2exr.exe"],
    "url": "https://www.blackmagicdesign.com/developer/products/braw",
    "download_html": ("<p><b>braw2exr</b> was not found.</p>"
                      "<p>Blackmagic RAW import uses <b>braw2exr</b>, a small tool of this Natron fork "
                      "(<i>tools/braw2exr</i>) that you build once against Blackmagic's free "
                      "<b>Blackmagic RAW SDK</b>: <a href=\"%(url)s\">%(url)s</a>.<br>"
                      "Then, in MSYS2 UCRT64: <code>bash tools/braw2exr/build.sh</code> "
                      "(copies braw2exr.exe next to Natron.exe).</p>"
                      "<p><b>Yes</b>: open the SDK page. <b>No</b>: braw2exr is built, select it yourself.</p>"),
}

FORMATS = [ARRI, BRAW]

# ARRI output colour (EXR must be scene linear): art-cmd name, label, OCIO candidates
ARRI_COLORS = [
    ("AP0/D60/linear", "ACES2065-1 linear (AP0)", ["ACES2065-1", "ACES - ACES2065-1", "aces"]),
    ("AWG4/D65/linear", "ARRI Wide Gamut 4 linear (ALEXA 35)",
     ["Linear ARRI Wide Gamut 4", "Input - ARRI - Linear - ARRI Wide Gamut 4", "lin_awg4"]),
    ("AWG3/D65/linear", "ARRI Wide Gamut 3 linear (LogC3 cameras)",
     ["Linear ARRI Wide Gamut 3", "Input - ARRI - Linear - ALEXA Wide Gamut", "lin_awg3"]),
]
ARRI_CODECS = [("exr_uncompressed/f16", "EXR half, uncompressed")]
BRAW_SCALES = [("Full", "full"), ("1/2", "half"), ("1/4", "quarter"), ("1/8", "eighth")]


# ============================================================================ utilities

def _log(msg):
    print("[Camera RAW Import] " + str(msg))


def _message(text, error=False, title="Camera RAW Import"):
    _log(text)
    try:
        import NatronGui
        if error:
            NatronGui.natron.errorDialog(title, text)
        else:
            NatronGui.natron.informationDialog(title, text)
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


def _norm(p):
    return os.path.normcase(os.path.normpath(p)) if p else ""


def _fmt_of(path):
    ext = os.path.splitext(path)[1].lower().lstrip(".")
    for f in FORMATS:
        if ext in f["exts"]:
            return f
    return None


def _quote(args):
    return " ".join('"%s"' % a if (" " in a or not a) else a for a in args)


# ============================================================================ tools

def _natron_bin():
    """Folder of Natron.exe (braw2exr is copied there by tools/braw2exr/build.sh)."""
    import sys
    cands = [os.path.dirname(getattr(sys, "executable", "") or "")]
    try:
        cands.append(os.path.dirname(NatronEngine.natron.getNatronPath()))
    except Exception:
        pass
    return [c for c in cands if c]


def _tool_candidates(fmt):
    c = list(fmt["candidates"])
    if fmt is BRAW:
        for b in _natron_bin():
            c += [os.path.join(b, "braw2exr.exe"), os.path.join(b, "braw2exr")]
    return c


def find_tool(fmt, cfg):
    p = cfg.get(fmt["key"], {}).get("tool")
    if p and os.path.isfile(p):
        return p
    for c in _tool_candidates(fmt):
        if os.path.isfile(c):
            return c
    for n in fmt["names"]:
        w = shutil.which(n)
        if w:
            return w
    roots = [os.environ.get("ProgramFiles", r"C:\Program Files"),
             os.environ.get("ProgramW6432", r"C:\Program Files"),
             os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")]
    for root in roots:
        for pat in fmt["globs"]:
            try:
                hits = glob.glob(os.path.join(root, pat), recursive=True)
            except Exception:
                hits = []
            if hits:
                return sorted(hits)[0]
    return None


def _ask_download(fmt):
    url = fmt["url"]
    _log("%s not found. See %s" % (fmt["tool"], url))
    ret = None
    try:
        import NatronGui
        ret = NatronGui.natron.questionDialog("%s import - %s needed" % (fmt["label"], fmt["tool"]),
                                              fmt["download_html"] % {"url": url})
    except Exception:
        pass
    if ret is not None and "yes" in str(ret).lower():
        try:
            import webbrowser
            webbrowser.open(url)
        except Exception:
            pass
        return True
    return False


def ask_tool(fmt, cfg):
    path = ""
    try:
        path = _app().getFilenameDialog(["exe"] if os.name == "nt" else [], "C:/Program Files")
    except Exception:
        pass
    if path and os.path.isfile(path):
        cfg.setdefault(fmt["key"], {})["tool"] = path
        cfg[fmt["key"]].pop("probe", None)
        _save(cfg)
        return path
    return None


def get_tool(fmt, cfg):
    tool = find_tool(fmt, cfg)
    if not tool:
        if _ask_download(fmt):
            return None
        tool = ask_tool(fmt, cfg)
    return tool


def _run_text(args, timeout=60):
    try:
        out = subprocess.run(args, capture_output=True, text=True, timeout=timeout, creationflags=_NO_WINDOW)
        return (out.stdout or "") + "\n" + (out.stderr or ""), out.returncode
    except Exception:
        _log("failed: " + _quote(args) + "\n" + traceback.format_exc())
        return "", -1


# ============================================================================ ARRI (art-cmd)

def arri_probe(tool, clip, cfg):
    """EXR codecs from `art-cmd process --help`, linear target colour spaces from
    `--ls-target-colorspaces` (both depend on the art-cmd version / the clip)."""
    codecs = list(ARRI_CODECS)
    probe = cfg.get("arri", {}).get("probe", {})
    if probe.get("_tool") == tool and probe.get("codecs"):
        codecs = [tuple(c) for c in probe["codecs"]]
    else:
        text, _ = _run_text([tool, "process", "--help"])
        found = sorted(set(re.findall(r"\bexr_[A-Za-z0-9]+/f(?:16|32)\b", text)))
        for c in found:
            if c not in [k for k, _ in codecs]:
                kind, bits = c[4:].split("/")
                codecs.append((c, "EXR %s, %s" % ("half" if bits == "f16" else "float", kind)))
        cfg.setdefault("arri", {})["probe"] = {"_tool": tool, "codecs": codecs}
        _save(cfg)
    colors = list(ARRI_COLORS)
    if clip and os.path.exists(_first_file(clip)):
        text, rc = _run_text([tool, "process", "--input", _arri_input(clip), "--ls-target-colorspaces"])
        spaces = re.findall(r"\b([A-Za-z0-9.\-_]+/[A-Za-z0-9.\-_]+/linear)\b", text) if rc == 0 else []
        if spaces:
            known = dict((k, (lab, oc)) for k, lab, oc in ARRI_COLORS)
            colors = []
            for s in dict.fromkeys(spaces):
                lab, oc = known.get(s, (s, []))
                colors.append((s, lab, oc))
    return codecs, colors


def _arri_input(clip):
    """art-cmd takes a .ari/.arx sequence as a printf pattern (or its folder)."""
    p = re.sub(r"#+", lambda m: "%%0%dd" % len(m.group(0)), clip)
    return p.replace("\\", "/")


def arri_dialog(app, clip, codecs, colors):
    d = app.createModalDialog()
    f = d.createStringParam("clip", "ARRIRAW clip")
    f.setValue(clip)
    c = d.createChoiceParam("color", "Output colour (linear)")
    for _, lab, _ in colors:
        c.addOption(lab, "")
    k = d.createChoiceParam("codec", "EXR type")
    for _, lab in codecs:
        k.addOption(lab, "")
    w = d.createIntParam("width", "Output width (0 = full)")
    w.setValue(0)
    o = d.createStringParam("outdir", "Output folder")
    name = _clip_name(clip)
    o.setValue(os.path.join(os.path.dirname(_first_file(clip)), name + "_EXR").replace("\\", "/"))
    n = d.createIntParam("renum", "First frame number")
    n.setValue(1001)
    s = d.createIntParam("start", "Clip start frame (0 = first)")
    s.setValue(0)
    du = d.createIntParam("duration", "Frames (0 = all)")
    du.setValue(0)
    x = d.createStringParam("extra", "Extra art-cmd arguments")
    x.setValue("")
    wt = d.createBooleanParam("wait", "Wait for the conversion (Natron is blocked)")
    wt.setValue(False)
    d.refreshUserParamsGUI()
    ok = d.exec() if hasattr(d, "exec") else d.exec_()
    if not ok:
        return None
    g = lambda n: d.getParam(n).getValue()
    return {"clip": g("clip"), "color": g("color"), "codec": g("codec"), "width": g("width"),
            "outdir": g("outdir"), "renum": g("renum"), "start": g("start"), "duration": g("duration"),
            "extra": g("extra"), "wait": g("wait")}


def arri_args(tool, opts, codecs, colors, name):
    # art-cmd's own frame counter template (manual 2.7): [####+1001] = 4 digits
    # from 1001 - no renaming afterwards. The folder must exist first.
    out = "%s.[####+%d].exr" % (name, max(0, int(opts["renum"])))
    args = [tool, "process", "--input", _arri_input(opts["clip"]),
            "--output", os.path.join(opts["outdir"], out).replace("\\", "/"),
            "--target-colorspace", colors[opts["color"]][0],
            "--video-codec", codecs[opts["codec"]][0]]
    if opts["width"] > 0:
        args += ["--output-width", str(opts["width"])]
    if opts["start"] > 0:
        args += ["--start", str(opts["start"])]
    if opts["duration"] > 0:
        args += ["--duration", str(opts["duration"])]
    if opts["extra"].strip():
        args += shlex.split(opts["extra"].replace("\\", "/"))
    return args


def _arri_renumber(job):
    """art-cmd writes 0000000.exr, 0000001.exr...: rename to <name>.1001.exr..."""
    outdir, name, first = job["outdir"], job["name"], int(job.get("renum", 1001))
    if not os.path.isdir(outdir):
        return
    raw = sorted(f for f in os.listdir(outdir) if re.match(r"^\d+\.exr$", f, re.I))
    for i, f in enumerate(raw):
        dst = os.path.join(outdir, "%s.%04d.exr" % (name, first + i))
        try:
            os.replace(os.path.join(outdir, f), dst)
        except OSError:
            _log("cannot rename " + f + "\n" + traceback.format_exc())


# ============================================================================ BRAW (braw2exr)

def braw_probe(tool, clip):
    """Clip info and the gamma / gamut names the SDK offers (braw2exr --list)."""
    info = {"gamma": [], "gamut": [], "current gamma": "", "current gamut": "", "frames": "", "size": ""}
    text, rc = _run_text([tool, "--list", clip], timeout=120)
    for line in text.splitlines():
        if ":" not in line:
            continue
        k, v = line.split(":", 1)
        k, v = k.strip().lower(), v.strip()
        if k in ("gamma", "gamut"):
            if v and v not in info[k]:
                info[k].append(v)
        elif k in info:
            info[k] = v
    info["ok"] = rc == 0
    info["error"] = text.strip() if rc != 0 else ""
    return info


def braw_dialog(app, clip, info):
    d = app.createModalDialog()
    f = d.createStringParam("clip", "Blackmagic RAW clip")
    f.setValue(clip)
    if info.get("size") or info.get("frames"):
        i = d.createStringParam("info", "Clip")
        i.setValue("%s, %s frames" % (info.get("size", "?"), info.get("frames", "?")))
    # The SDK's own names (braw2exr --list). If the list is empty, at least the
    # values its OpenEXRTranscode sample uses for scene-linear EXR.
    if not info["gamut"]:
        info["gamut"] = ["ACES AP0"]
    if not info["gamma"]:
        info["gamma"] = ["Linear"]
    gamuts = [""] + info["gamut"]
    gammas = [""] + info["gamma"]
    gu = d.createChoiceParam("gamut", "Gamut")
    gu.addOption("As shot (%s)" % (info.get("current gamut") or "clip setting"), "")
    for v in info["gamut"]:
        gu.addOption(v, "")
    ga = d.createChoiceParam("gamma", "Gamma")
    ga.addOption("As shot (%s)" % (info.get("current gamma") or "clip setting"), "")
    for v in info["gamma"]:
        ga.addOption(v, "")
    # Natron works scene linear: preselect Linear + ACES AP0 (ACES2065-1 in OCIO)
    for idx, v in enumerate(gammas):
        if v and v.lower() == "linear":
            ga.setValue(idx)
            break
    else:
        for idx, v in enumerate(gammas):
            if v and "linear" in v.lower():
                ga.setValue(idx)
                break
    for idx, v in enumerate(gamuts):
        if v and "aces" in v.lower() and "ap0" in v.lower():
            gu.setValue(idx)
            break
    lut = d.createBooleanParam("keeplut", "Keep the clip's 3D LUT (look baked into the EXR)")
    lut.setValue(False)
    sc = d.createChoiceParam("scale", "Decode resolution")
    for lab, _ in BRAW_SCALES:
        sc.addOption(lab, "")
    fl = d.createBooleanParam("float", "32-bit float EXR (default: 16-bit half)")
    fl.setValue(False)
    o = d.createStringParam("outdir", "Output folder")
    name = _clip_name(clip)
    o.setValue(os.path.join(os.path.dirname(clip), name + "_EXR").replace("\\", "/"))
    n = d.createIntParam("renum", "First frame number")
    n.setValue(1001)
    s = d.createIntParam("start", "Clip start frame (0 = first)")
    s.setValue(0)
    c = d.createIntParam("count", "Frames (0 = all)")
    c.setValue(0)
    x = d.createStringParam("extra", "Extra braw2exr arguments")
    x.setValue("")
    wt = d.createBooleanParam("wait", "Wait for the conversion (Natron is blocked)")
    wt.setValue(False)
    d.refreshUserParamsGUI()
    ok = d.exec() if hasattr(d, "exec") else d.exec_()
    if not ok:
        return None
    g = lambda n: d.getParam(n).getValue()
    return {"clip": g("clip"), "gamut": gamuts[g("gamut")], "gamma": gammas[g("gamma")],
            "scale": BRAW_SCALES[g("scale")][1], "float": g("float"), "outdir": g("outdir"),
            "renum": g("renum"), "start": g("start"), "count": g("count"), "extra": g("extra"),
            "wait": g("wait"), "keeplut": g("keeplut")}


def braw_args(tool, opts, name):
    args = [tool, "--input", opts["clip"].replace("\\", "/"),
            "--output", os.path.join(opts["outdir"], name + ".%04d.exr").replace("\\", "/"),
            "--first-number", str(opts["renum"]), "--scale", opts["scale"]]
    if opts["start"] > 0:
        args += ["--start", str(opts["start"])]
    if opts["count"] > 0:
        args += ["--count", str(opts["count"])]
    if opts["gamut"]:
        args += ["--gamut", opts["gamut"]]
    if opts["gamma"]:
        args += ["--gamma", opts["gamma"]]
    if opts["float"]:
        args.append("--float")
    if opts.get("keeplut"):
        args.append("--keep-lut")
    if opts["extra"].strip():
        args += shlex.split(opts["extra"].replace("\\", "/"))
    return args


def _braw_ocio(opts):
    gu, ga = opts["gamut"].lower(), opts["gamma"].lower()
    if "linear" in ga and "ap0" in gu:
        return ["ACES2065-1", "ACES - ACES2065-1", "aces"]
    if "linear" in ga and "ap1" in gu:
        return ["ACEScg", "ACES - ACEScg"]
    if "linear" in ga and ("709" in gu or "srgb" in gu):
        return ["Linear Rec.709 (sRGB)", "Linear Rec.709", "lin_rec709", "linear"]
    return []


# ============================================================================ clips / Reads

def _first_file(path):
    """A sequence given as a pattern (####, %07d): its first existing file."""
    if "#" not in path and "%" not in path:
        return path
    pat = re.sub(r"#+", lambda m: "[0-9]" * len(m.group(0)), path)
    pat = re.sub(r"%0?(\d*)d", lambda m: "[0-9]" * int(m.group(1) or 1), pat)
    files = sorted(glob.glob(pat))
    return files[0] if files else path


def _clip_name(path):
    """'A001C003_210101_R1AB' from A001C003_210101_R1AB.0000123.ari, .#######.ari, clip.braw..."""
    base = os.path.splitext(os.path.basename(path))[0]
    base = re.sub(r"(?:\.\d{3,}|[._-]?(?:#+|%0?\d*d))$", "", base)
    return base or "clip"


def _sequence_pattern(outdir, name):
    if not os.path.isdir(outdir):
        return None
    rx = re.compile(r"^(" + re.escape(name) + r"\.)(\d+)(\.exr)$", re.I)
    for f in sorted(os.listdir(outdir)):
        m = rx.match(f)
        if m:
            return os.path.join(outdir, m.group(1) + "#" * len(m.group(2)) + m.group(3)).replace("\\", "/")
    return None


def _set_ocio(node, cands):
    space = node.getParam("ocioInputSpace")
    if space is None:
        return
    for cand in cands or []:
        try:
            space.setValue(cand)
            return
        except Exception:
            continue


def create_read(job):
    pattern = _sequence_pattern(job["outdir"], job["name"])
    if not pattern:
        return None
    node = _app().createNode("fr.inria.built-in.Read")
    if node is None:
        return None
    try:
        node.setLabel(job["name"])
    except Exception:
        pass
    p = node.getParam("filename")
    if p is not None:
        p.setValue(pattern)
    _set_ocio(node, job.get("ocio"))
    _log("Read created: " + pattern)
    return node


# ---------------------------------------------------------------- progress on the Read

def _frames_written(job):
    if not os.path.isdir(job["outdir"]):
        return 0
    rx = re.compile(r"^" + re.escape(job["name"]) + r"\.\d+\.exr$", re.I)
    return sum(1 for f in os.listdir(job["outdir"]) if rx.match(f))


def placeholder_read(name):
    """The Read is created right away: it shows the progress, then gets the EXR."""
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
        import time
        p = (time.time() % 4.0) / 4.0     # total unknown: sweeping bar
        _set_read_state(job, p, "%s  %d frames" % (job["name"], done))


def arri_frame_count(tool, clip, start, duration):
    """Frames art-cmd will render: --duration, the .ari/.arx files, or the
    per-frame metadata rows of `art-cmd export ... .csv` (manual 4)."""
    if duration > 0:
        return duration
    total = 0
    if "#" in clip or "%" in clip:
        pat = re.sub(r"#+", lambda m: "[0-9]" * len(m.group(0)), clip)
        pat = re.sub(r"%0?(\d*)d", lambda m: "[0-9]" * int(m.group(1) or 1), pat)
        total = len(glob.glob(pat))
    else:
        try:
            import tempfile
            csv = os.path.join(tempfile.gettempdir(), "natron_camraw_meta.csv")
            _run_text([tool, "export", "--input", _arri_input(clip),
                       "--output", csv], timeout=120)
            with open(csv, "r", errors="replace") as fh:
                total = max(0, sum(1 for l in fh if l.strip()) - 1)   # header row
            os.remove(csv)
        except Exception:
            _log("frame count (art-cmd export) failed\n" + traceback.format_exc())
    return max(0, total - max(0, start)) if total else 0


def _finish_job(job):
    """Conversion done: rename if needed, point the waiting Reads at the EXR, else create one."""
    if job.get("rename"):
        _arri_renumber(job)
    pattern = _sequence_pattern(job["outdir"], job["name"])
    if not pattern:
        return False
    _set_read_state(job, -1, job["name"])
    app = _app()
    done = False
    for name in job.get("reads") or []:
        node = app.getNode(name)
        if node is None:
            continue
        p = node.getParam("filename")
        if p is not None:
            p.setValue(pattern)
            _set_ocio(node, job.get("ocio"))
            done = True
            _log("%s -> %s" % (name, pattern))
    if not done:
        return create_read(job) is not None
    return True


# ============================================================================ running

def _schedule_poll():
    try:
        import NatronGui
        if not hasattr(NatronGui.natron, "callLater"):
            return False
        NatronGui.natron.callLater(POLL_MS, "camraw_poll")
        return True
    except Exception:
        _log("callLater failed\n" + traceback.format_exc())
        return False


def camraw_poll():
    try:
        cfg = _load()
        left, still = [], False
        for job in cfg.get("pending", []):
            proc = _RUNNING.get(job["outdir"])
            if proc is None:
                left.append(job)               # started by another session
                continue
            rc = proc.poll()
            if rc is None:
                still = True
                left.append(job)
                update_progress(job)
                continue
            _RUNNING.pop(job["outdir"], None)
            if rc != 0:
                _set_read_state(job, -1, job["name"] + " (conversion failed)")
                _message(_failure_text(job, rc), error=True)
            elif not _finish_job(job):
                _message("%s finished %s but no EXR was found in %s." % (job["tool"], job["name"], job["outdir"]), error=True)
            else:
                _log("Conversion finished: %s is loaded from its EXR sequence." % job["name"])
        cfg["pending"] = left
        _save(cfg)
        if still:
            _schedule_poll()
    except Exception:
        _log("poll failed\n" + traceback.format_exc())


def _tail(path, n=12):
    try:
        with open(path, "r", errors="replace") as fh:
            lines = [l.rstrip() for l in fh.readlines() if l.strip()]
        return "\n".join(lines[-n:])
    except Exception:
        return ""


def _failure_text(job, rc):
    code = rc & 0xFFFFFFFF
    text = "%s failed on %s (exit code %d / 0x%08X): no Read created." % (job["tool"], job["name"], rc, code)
    t = _tail(job.get("log", ""))
    if t:
        text += "\n\n%s output (%s):\n%s" % (job["tool"], job["log"], t)
    return text


def _run(fmt, args, job, wait):
    """Run the converter; its output goes to <outdir>/<tool>.log (shown on failure)."""
    _log(_quote(args))
    os.makedirs(job["outdir"], exist_ok=True)
    job["log"] = os.path.join(job["outdir"], fmt["tool"] + ".log")
    try:
        logf = open(job["log"], "w")
        logf.write(_quote(args) + "\n\n")
        logf.flush()
    except OSError:
        logf = None
    if wait:
        res = subprocess.run(args, stdout=logf, stderr=subprocess.STDOUT, creationflags=_NO_WINDOW)
        if logf:
            logf.close()
        if res.returncode != 0:
            _message(_failure_text(job, res.returncode), error=True)
        elif not _finish_job(job):
            _message("%s finished but no EXR was found in %s" % (fmt["tool"], job["outdir"]), error=True)
        return
    try:
        proc = subprocess.Popen(args, stdout=logf, stderr=subprocess.STDOUT, creationflags=_NO_WINDOW)
    except OSError:
        if logf:
            logf.close()
        cfg = _load()
        cfg.get(fmt["key"], {}).pop("tool", None)
        _save(cfg)
        _message("%s could not be started (%s)." % (fmt["tool"], args[0]), error=True)
        return
    if logf:
        logf.close()   # the child keeps its own handle
    _RUNNING[job["outdir"]] = proc
    cfg = _load()
    cfg["pending"] = [j for j in cfg.get("pending", []) if j["outdir"] != job["outdir"]] + [job]
    _save(cfg)
    update_progress(job)
    if _schedule_poll():
        _log("Conversion started in the background (log: %s)" % job["log"])
    else:
        _message("Conversion started in the background (log: %s).\nLoad the EXR from %s when it is done."
                 % (job["log"], job["outdir"]))


def start_conversion(clip, reads=None):
    fmt = _fmt_of(clip)
    if fmt is None:
        _message("Not an ARRIRAW or Blackmagic RAW file:\n" + clip, error=True)
        return
    app = _app()
    cfg = _load()
    tool = get_tool(fmt, cfg)
    if not tool:
        return
    reads = list(reads or [])
    if fmt is ARRI:
        codecs, colors = arri_probe(tool, clip, cfg)
        opts = arri_dialog(app, clip, codecs, colors)
        if not opts:
            return
        name = _clip_name(opts["clip"])
        os.makedirs(opts["outdir"], exist_ok=True)
        args = arri_args(tool, opts, codecs, colors, name)
        job = {"fmt": "arri", "tool": fmt["tool"], "name": name, "outdir": opts["outdir"],
               "clip": opts["clip"], "ocio": colors[opts["color"]][2], "reads": reads,
               "renum": opts["renum"],
               "total": arri_frame_count(tool, opts["clip"], opts["start"], opts["duration"])}
    else:
        info = braw_probe(tool, clip)
        if not info["ok"]:
            _message("braw2exr cannot read this clip:\n%s\n\n%s\n\n(Is the Blackmagic RAW runtime installed?)"
                     % (clip, info["error"][-1500:]), error=True)
            return
        opts = braw_dialog(app, clip, info)
        if not opts:
            return
        name = _clip_name(opts["clip"])
        args = braw_args(tool, opts, name)
        try:
            total = max(0, int(info.get("frames") or 0) - max(0, opts["start"]))
        except ValueError:
            total = 0
        if opts["count"] > 0:
            total = min(total, opts["count"]) if total else opts["count"]
        job = {"fmt": "braw", "tool": fmt["tool"], "name": name, "outdir": opts["outdir"],
               "clip": opts["clip"], "ocio": _braw_ocio(opts), "reads": reads, "total": total}
    if not opts["wait"] and not job["reads"]:
        ph = placeholder_read(name)       # the Read exists from the start, with the progress
        if ph:
            job["reads"] = [ph]
    _run(fmt, args, job, opts["wait"])


def _import(fmt):
    try:
        cfg = _load()
        if not get_tool(fmt, cfg):
            return
        exts = []
        for e in fmt["exts"]:
            exts += [e, e.upper()]
        clip = _app().getFilenameDialog(exts)
        if clip:
            start_conversion(clip)
    except Exception:
        _message("%s import failed:\n%s" % (fmt["label"], traceback.format_exc()), error=True)


def camraw_import_arri():
    _import(ARRI)


def camraw_import_braw():
    _import(BRAW)


def camraw_on_read(path):
    """Called by a Read given a .ari / .arx / .braw (Engine/ReadNode.cpp), from inside
    its parameter change: defer the dialog to the next event loop turn."""
    _READ_REQUESTS.append(path)
    try:
        import NatronGui
        if hasattr(NatronGui.natron, "callLater"):
            NatronGui.natron.callLater(0, "camraw_process_reads")
            return
    except Exception:
        pass
    camraw_process_reads()


def camraw_process_reads():
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
            name = _clip_name(path)
            outdir = os.path.join(os.path.dirname(_first_file(path)), name + "_EXR")
            # Already converted to the default folder: use it without asking.
            if reads and _sequence_pattern(outdir, name):
                _finish_job({"name": name, "outdir": outdir, "clip": path, "reads": reads})
                continue
            if any(_norm(j["clip"]) == _norm(path) and j["outdir"] in _RUNNING
                   for j in _load().get("pending", [])):
                continue
            start_conversion(path, reads)
    except Exception:
        _message("Camera RAW Read failed:\n" + traceback.format_exc(), error=True)


def register():
    import __main__
    import NatronGui
    __main__.NatronGui = NatronGui
    __main__.NatronEngine = NatronEngine
    __main__.camraw_import_arri = camraw_import_arri
    __main__.camraw_import_braw = camraw_import_braw
    __main__.camraw_on_read = camraw_on_read
    __main__.camraw_process_reads = camraw_process_reads
    __main__.camraw_poll = camraw_poll
