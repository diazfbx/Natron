# -*- coding: utf-8 -*-
"""Themes menu for Natron: "Themes > Nuketron" and "Themes > Natron default".

Nuketron = Nuke-like look: Nuke greys, orange/yellow accents. It sets the
colours of Preferences > Appearance (panels, text, selection, timeline,
keyframes...) and points Preferences > Appearance > Stylesheet file to
<Natron>/Themes/nuketron/nuketron.qss. Natron reloads the look live and
saves it in the preferences, so it stays after a restart.

"Natron default" puts every colour touched here back to its default and
clears the stylesheet file.

Install: this folder goes into <Natron>/Plugins/PyPlugs (done by the fork's
build), the theme itself into <Natron>/Themes/nuketron/.
Natron runs initGui.py in __main__ at startup; no PySide needed.
"""
from __future__ import print_function

import os
import sys
import traceback

import NatronEngine

# Preferences > Appearance colours (settings knob name -> r, g, b in 0..1)
NUKETRON_COLORS = {
    # UI greys measured on Nuke: panels #323232, tab bars #262626, fields #3a3a3a
    "sunken":           (0.149, 0.149, 0.149),
    "base":             (0.196, 0.196, 0.196),
    "raised":           (0.227, 0.227, 0.227),
    "text":             (0.784, 0.784, 0.784),
    "unmodifiedText":   (0.620, 0.620, 0.620),
    # Nuke orange #f79323
    "selection":        (0.969, 0.576, 0.137),
    # Animated / linked knobs (Nuke: keyed = blue, interpolated = light blue, expression = purple)
    "keyframe":         (0.200, 0.420, 0.660),
    "interpValue":      (0.330, 0.450, 0.560),
    "exprColor":        (0.520, 0.380, 0.700),
    "slider":           (0.969, 0.576, 0.137),
    # Timeline: Nuke's #2d2d2d frame bar, orange playhead, yellow in/out
    # bounds, green cached frames
    "timelineBG":       (0.176, 0.176, 0.176),
    "timelinePlayhead": (0.969, 0.576, 0.137),
    "timelineBound":    (0.950, 0.760, 0.300),
    "cachedFrame":      (0.330, 0.700, 0.330),
    "diskCachedFrame":  (0.300, 0.450, 0.700),
    # Curve editor / dope sheet on the same #323232 as the panels
    "curveEditorBG":    (0.196, 0.196, 0.196),
    "dopesheetBackground": (0.196, 0.196, 0.196),
}

THEME_REL = os.path.join("Themes", "nuketron", "nuketron.qss")
_LOG_PREFIX = "[Themes] "


def _log(msg):
    print(_LOG_PREFIX + str(msg))


def _message(text, error=False):
    _log(text)
    try:
        import NatronGui
        if error:
            NatronGui.natron.errorDialog("Themes", text)
        else:
            NatronGui.natron.informationDialog("Themes", text)
    except Exception:
        pass


def _candidate_roots():
    """Natron install roots to look for Themes/ in (first match wins)."""
    roots = []
    try:
        for p in NatronEngine.natron.getNatronPath():
            p = os.path.normpath(str(p))
            # <root>/Plugins/PyPlugs  ->  <root>
            if os.path.basename(p).lower() == "pyplugs":
                roots.append(os.path.dirname(os.path.dirname(p)))
            roots.append(p)
    except Exception:
        pass
    for exe in (getattr(sys, "executable", "") or "", sys.argv[0] if sys.argv else ""):
        if exe:
            d = os.path.dirname(os.path.abspath(exe))   # <root>/bin  (or build-qt6/App)
            roots += [os.path.dirname(d), d]
    roots.append(os.path.join(os.path.expanduser("~"), ".Natron"))
    seen, out = set(), []
    for r in roots:
        k = os.path.normcase(r)
        if r and k not in seen:
            seen.add(k)
            out.append(r)
    return out


def find_theme_file():
    for r in _candidate_roots():
        f = os.path.join(r, THEME_REL)
        if os.path.isfile(f):
            return f.replace("\\", "/")
    return None


def _settings():
    return NatronEngine.natron.getSettings()


def _set_color(settings, name, rgb):
    p = settings.getParam(name)
    if p is None:
        _log("no setting named " + name + " (skipped)")
        return
    for dim, v in enumerate(rgb):
        p.setValue(float(v), dim)


def _reset_color(settings, name):
    p = settings.getParam(name)
    if p is None:
        return
    for dim in range(3):
        try:
            p.restoreDefaultValue(dim)
        except Exception:
            pass


def apply_nuketron():
    try:
        qss = find_theme_file()
        if qss is None:
            _message("nuketron.qss not found. Expected in <Natron>/" + THEME_REL.replace("\\", "/")
                     + " (searched: " + ", ".join(_candidate_roots()) + ")", error=True)
            return
        s = _settings()
        for name, rgb in NUKETRON_COLORS.items():
            _set_color(s, name, rgb)
        f = s.getParam("stylesheetFile")
        if f is not None:
            f.setValue(qss)
        s.saveSettings()
        _log("Nuketron applied (" + qss + ")")
    except Exception:
        _message("Could not apply Nuketron:\n" + traceback.format_exc(), error=True)


def apply_natron_default():
    try:
        s = _settings()
        for name in NUKETRON_COLORS:
            _reset_color(s, name)
        f = s.getParam("stylesheetFile")
        if f is not None:
            f.setValue("")
        s.saveSettings()
        _log("Natron default theme restored")
    except Exception:
        _message("Could not restore the default theme:\n" + traceback.format_exc(), error=True)


def themes_nuketron():
    apply_nuketron()


def themes_natron_default():
    apply_natron_default()


def _themes_register():
    import __main__
    import NatronGui
    # Natron evaluates menu commands in __main__: publish the functions there
    __main__.NatronGui = NatronGui
    __main__.NatronEngine = NatronEngine
    __main__.themes_nuketron = themes_nuketron
    __main__.themes_natron_default = themes_natron_default
    if getattr(NatronGui, "_themes_menu_done", False):
        return
    # no shortcut: it would need PySide, which crashes some Natron builds
    NatronGui.natron.addMenuCommand("Themes/Nuketron", "themes_nuketron")
    NatronGui.natron.addMenuCommand("Themes/Natron default", "themes_natron_default")
    NatronGui._themes_menu_done = True


try:
    _themes_register()
except Exception:
    _log("menu registration failed\n" + traceback.format_exc())
