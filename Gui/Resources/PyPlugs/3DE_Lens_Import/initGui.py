# -*- coding: utf-8 -*-
"""3DE Lens Import pour Natron - menu "3DE > Import Lens Distortion LDE4 (.nk)".

Installation : copier le dossier 3DE_Lens_Import dans <Natron>/Plugins/PyPlugs (ou dans ~/.Natron),
puis redemarrer Natron. Natron execute automatiquement ce initGui.py au demarrage.

Import des nodes de distorsion 3DEqualizer (export Nuke .nk) dans Natron.

Lit les nodes LD_3DE4_* / LD_3DE_Classic_* d'un fichier .nk (ou du presse-papiers) et cree
pour chacun un node Natron LensDistortion (net.sf.openfx.LensDistortion) avec le bon modele,
la direction et toutes les valeurs (animations comprises : {{curve x1001 ...}}).

"""
from __future__ import print_function

import os
import re
import traceback

import NatronEngine

PLUGIN_ID = "net.sf.openfx.LensDistortion"

# classe Nuke -> (identifiant du modele Natron, libelle, index de repli)
MODELS = {
    "LD_3DE_Classic_LD_Model": ("3declassic", "3DE Classic", 2),
    "LD_3DE4_Anamorphic_Degree_6": ("3deanamorphic6", "3DE4 Anamorphic Degree 6", 3),
    "LD_3DE4_Radial_Fisheye_Degree_8": ("3defisheye8", "3DE4 Radial Fisheye Degree 8", 4),
    "LD_3DE4_Radial_Standard_Degree_4": ("3deradial4", "3DE4 Radial Standard Degree 4", 5),
    "LD_3DE4_Anamorphic_Standard_Degree_4": ("3deanamorphic4", "3DE4 Anamorphic Standard Degree 4", 6),
    # variantes "rescaled" exportees par les versions recentes de 3DE
    "LD_3DE4_Anamorphic_Standard_Degree_4_Rescaled": ("3deanamorphic4", "3DE4 Anamorphic Standard Degree 4", 6),
    "LD_3DE4_Anamorphic_Degree_6_Rescaled": ("3deanamorphic6", "3DE4 Anamorphic Degree 6", 3),
}

# parametres Nuke ignores (geres a part ou sans equivalent)
SKIP = {"name", "label", "xpos", "ypos", "selected", "inputs", "direction", "note_font",
        "tile_color", "gl_color", "disable", "help", "onCreate", "knobChanged", "filter",
        "tde4_focal_length_cm_unit", "black_outside", "cards", "output"}


# ----------------------------------------------------------------------------- parsing .nk

def _tokenize(text):
    """Decoupe le texte .nk en tokens ; un groupe {...} equilibre donne un seul token."""
    tokens, i, n = [], 0, len(text)
    while i < n:
        c = text[i]
        if c.isspace():
            i += 1
        elif c == "#":
            while i < n and text[i] != "\n":
                i += 1
        elif c == "{":
            depth, j = 0, i
            while j < n:
                if text[j] == "{":
                    depth += 1
                elif text[j] == "}":
                    depth -= 1
                    if depth == 0:
                        break
                elif text[j] == "\\":
                    j += 1
                j += 1
            tokens.append(text[i:j + 1])
            i = j + 1
        elif c == '"':
            j = i + 1
            while j < n and text[j] != '"':
                if text[j] == "\\":
                    j += 1
                j += 1
            tokens.append(text[i + 1:j])
            i = j + 1
        else:
            j = i
            while j < n and not text[j].isspace() and text[j] not in "{}":
                j += 1
            tokens.append(text[i:j])
            i = j
    return tokens


def _parse_value(tok):
    """Retourne un float, une chaine, ou {frame: valeur} pour une courbe animee."""
    t = tok.strip()
    while t.startswith("{") and t.endswith("}"):
        t = t[1:-1].strip()
    if t.startswith("curve"):
        keys, frame = {}, 1.0
        for part in t.split()[1:]:
            if part.startswith("x"):
                try:
                    frame = float(part[1:])
                except ValueError:
                    pass
                continue
            try:
                keys[frame] = float(part)
                frame += 1.0
            except ValueError:
                pass  # options de courbe (i, K, L, C, s...)
        if len(set(keys.values())) == 1:
            return list(keys.values())[0]
        return keys or None
    try:
        return float(t)
    except ValueError:
        return t


def parse_nk(text):
    """Liste de dicts {"class", "name", "direction", "params": {nom: valeur}} pour chaque node 3DE."""
    toks = _tokenize(text)
    nodes = []
    for i, tok in enumerate(toks[:-1]):
        if tok in MODELS or (tok.startswith("LD_3DE") and toks[i + 1].startswith("{")):
            body = _tokenize(toks[i + 1][1:-1])
            params, k = {}, 0
            while k < len(body) - 1:
                key, val = body[k], body[k + 1]
                params[key] = _parse_value(val)
                k += 2
            nodes.append({
                "class": tok,
                "name": str(params.pop("name", tok)),
                "direction": str(params.get("direction", "undistort")).lower(),
                "params": params,
            })
    return nodes


# ----------------------------------------------------------------------------- creation Natron

def _choice_index(param, wanted_id, wanted_label, fallback):
    try:
        for i in range(param.getNumOptions()):
            opt = param.getOption(i)
            texts = [str(x) for x in opt] if isinstance(opt, (list, tuple)) else [str(opt)]
            for t in texts:
                if t.strip().lower() in (wanted_id.lower(), wanted_label.lower()):
                    return i
    except Exception:
        pass
    return fallback


def _set(param, value):
    if isinstance(value, dict):
        for frame, v in sorted(value.items()):
            param.setValueAtTime(float(v), int(round(frame)))
    else:
        param.setValue(float(value))


def _natron_names(nuke_name):
    if nuke_name.startswith("tde4_"):
        return [nuke_name]
    return ["tde4_" + nuke_name, nuke_name]


def create_nodes(app, nodes, input_node=None, log=print):
    created = []
    for idx, nd in enumerate(nodes):
        model = MODELS.get(nd["class"])
        if model is None:
            log("Modele 3DE non supporte par Natron : %s (node ignore)" % nd["class"])
            continue
        node = app.createNode(PLUGIN_ID)
        if node is None:
            raise RuntimeError("Impossible de creer le node LensDistortion (%s)" % PLUGIN_ID)
        safe = re.sub(r"[^A-Za-z0-9_]", "_", nd["name"]) or "LD_3DE"
        try:
            node.setScriptName(safe)
        except Exception:
            pass
        try:
            node.setLabel(nd["name"])
        except Exception:
            pass

        p = node.getParam("model")
        p.setValue(_choice_index(p, model[0], model[1], model[2]))
        p = node.getParam("direction")
        undist = nd["direction"].startswith("un")
        p.setValue(_choice_index(p, "undistort" if undist else "distort",
                                 "Undistort" if undist else "Distort", 1 if undist else 0))

        missing = []
        for key, val in nd["params"].items():
            if key in SKIP or val is None or isinstance(val, str):
                continue
            for pname in _natron_names(key):
                prm = node.getParam(pname)
                if prm is not None:
                    try:
                        _set(prm, val)
                    except Exception:
                        log("Echec %s = %s\n%s" % (pname, val, traceback.format_exc()))
                    break
            else:
                missing.append(key)
        if missing:
            log("%s : parametres sans equivalent Natron : %s" % (nd["name"], ", ".join(missing)))

        if input_node is not None:
            try:
                node.connectInput(0, input_node)
            except Exception:
                pass
            try:
                x, y = input_node.getPosition()
                node.setPosition(x, y + 80 + idx * 60)
            except Exception:
                pass
        created.append(node)
        log("Cree %s (%s, %s)" % (safe, model[1], "undistort" if undist else "distort"))
    return created


# ----------------------------------------------------------------------------- commandes menu

_LOG = os.path.join(os.path.expanduser("~"), ".Natron", "tde_lens_import.log")


def _plog(msg):
    try:
        with open(_LOG, "a") as fh:
            fh.write(str(msg) + "\n")
    except Exception:
        pass
    print("[3DE Lens Import] " + str(msg))


def _app():
    """Instance GUI (GuiApp, qui a getFilenameDialog / getSelectedNodes)."""
    try:
        import NatronGui
        try:
            n = NatronGui.natron.getNumInstances()
        except Exception:
            n = 1
        for i in range(max(1, n)):
            a = NatronGui.natron.getGuiInstance(i)
            if a is not None and hasattr(a, "getFilenameDialog"):
                return a
    except Exception:
        _plog("getGuiInstance echec\n" + traceback.format_exc())
    return NatronEngine.natron.getActiveInstance()


def _win_open_dialog():
    """Explorateur Windows natif (comdlg32) appele via ctypes, sans Qt."""
    import ctypes
    from ctypes import wintypes

    class OPENFILENAMEW(ctypes.Structure):
        _fields_ = [("lStructSize", wintypes.DWORD), ("hwndOwner", wintypes.HWND),
                    ("hInstance", wintypes.HINSTANCE), ("lpstrFilter", wintypes.LPCWSTR),
                    ("lpstrCustomFilter", wintypes.LPWSTR), ("nMaxCustFilter", wintypes.DWORD),
                    ("nFilterIndex", wintypes.DWORD), ("lpstrFile", wintypes.LPWSTR),
                    ("nMaxFile", wintypes.DWORD), ("lpstrFileTitle", wintypes.LPWSTR),
                    ("nMaxFileTitle", wintypes.DWORD), ("lpstrInitialDir", wintypes.LPCWSTR),
                    ("lpstrTitle", wintypes.LPCWSTR), ("Flags", wintypes.DWORD),
                    ("nFileOffset", wintypes.WORD), ("nFileExtension", wintypes.WORD),
                    ("lpstrDefExt", wintypes.LPCWSTR), ("lCustData", wintypes.LPARAM),
                    ("lpfnHook", ctypes.c_void_p), ("lpTemplateName", wintypes.LPCWSTR),
                    ("pvReserved", ctypes.c_void_p), ("dwReserved", wintypes.DWORD),
                    ("FlagsEx", wintypes.DWORD)]

    buf = ctypes.create_unicode_buffer(4096)
    ofn = OPENFILENAMEW()
    ofn.lStructSize = ctypes.sizeof(OPENFILENAMEW)
    try:
        ofn.hwndOwner = ctypes.windll.user32.GetForegroundWindow()
    except Exception:
        ofn.hwndOwner = None
    ofn.lpstrFilter = "Nuke / 3DE (*.nk)\0*.nk\0Tous les fichiers (*.*)\0*.*\0\0"
    ofn.nFilterIndex = 1
    ofn.lpstrFile = ctypes.cast(buf, wintypes.LPWSTR)
    ofn.nMaxFile = 4096
    ofn.lpstrTitle = "Importer distorsion 3DE (.nk)"
    ofn.Flags = 0x00001000 | 0x00000800 | 0x00080000 | 0x00000008  # FILEMUSTEXIST|PATHMUSTEXIST|EXPLORER|NOCHANGEDIR
    if ctypes.windll.comdlg32.GetOpenFileNameW(ctypes.byref(ofn)):
        return buf.value
    return None


def _ask_file(app):
    path = None
    if hasattr(app, "getFilenameDialog"):
        try:
            path = app.getFilenameDialog(["nk"], "")
            _plog("dialogue Natron -> %r" % (path,))
            return path
        except Exception:
            _plog("getFilenameDialog echec\n" + traceback.format_exc())
    if os.name == "nt":
        try:
            path = _win_open_dialog()
            _plog("explorateur Windows -> %r" % (path,))
        except Exception:
            _plog("explorateur Windows echec\n" + traceback.format_exc())
    return path


def _selected(app):
    try:
        sel = app.getSelectedNodes()
        if sel and len(sel) == 1:
            return sel[0]
    except Exception:
        pass
    return None


def _message(app, text, error=False):
    _plog(text)
    try:
        import NatronGui
        if error:
            NatronGui.natron.errorDialog("Import 3DE", text)
        else:
            NatronGui.natron.informationDialog("Import 3DE", text)
    except Exception:
        pass


def import_text(text, app=None):
    app = app or _app()
    logs = []
    nodes = parse_nk(text)
    if not nodes:
        _message(app, "Aucun node LD_3DE* trouve.", error=True)
        return []
    try:
        created = create_nodes(app, nodes, _selected(app), log=logs.append)
    except Exception:
        _message(app, "Erreur :\n" + traceback.format_exc(), error=True)
        return []
    _message(app, "\n".join(logs) or "Rien a importer.")
    return created


def import_file(path=None, app=None):
    app = app or _app()
    _plog("app = %r" % (app,))
    if not path:
        path = _ask_file(app)
    _plog("fichier = %r" % (path,))
    if not path:
        return []
    if not os.path.isfile(path):
        _message(app, "Fichier introuvable : %s" % path, error=True)
        return []
    with open(path, "r") as fh:
        return import_text(fh.read(), app)


def _clipboard_text():
    """Presse-papiers Windows via ctypes (pas de Qt : PySide fait planter Natron)."""
    if os.name != "nt":
        return ""
    import ctypes
    from ctypes import wintypes
    u32, k32 = ctypes.windll.user32, ctypes.windll.kernel32
    u32.GetClipboardData.restype = wintypes.HANDLE
    k32.GlobalLock.restype = ctypes.c_void_p
    k32.GlobalLock.argtypes = [wintypes.HGLOBAL]
    k32.GlobalUnlock.argtypes = [wintypes.HGLOBAL]
    if not u32.OpenClipboard(None):
        return ""
    try:
        h = u32.GetClipboardData(13)  # CF_UNICODETEXT
        if not h:
            return ""
        ptr = k32.GlobalLock(h)
        try:
            return ctypes.wstring_at(ptr) if ptr else ""
        finally:
            k32.GlobalUnlock(h)
    finally:
        u32.CloseClipboard()


def import_clipboard(app=None):
    app = app or _app()
    text = _clipboard_text()
    if not text.strip():
        _message(app, "Presse-papiers vide ou illisible.", error=True)
        return []
    return import_text(text, app)


# ----------------------------------------------------------------------------- menu

def tde_lens_import_nk():
    try:
        import_file()
    except Exception:
        _plog("import echec\n" + traceback.format_exc())


def _tde_register():
    import __main__
    import NatronGui
    # Natron evalue les commandes de menu dans __main__ : on y publie la fonction
    __main__.NatronGui = NatronGui
    __main__.NatronEngine = NatronEngine
    __main__.tde_lens_import_nk = tde_lens_import_nk
    if getattr(NatronGui, "_tde_lens_menu_done", False):
        return
    # pas de raccourci : il demanderait PySide, qui fait planter certaines versions de Natron
    NatronGui.natron.addMenuCommand("3DE/Import Lens Distortion LDE4 (.nk)", "tde_lens_import_nk")
    NatronGui._tde_lens_menu_done = True


try:
    _tde_register()
except Exception:
    _plog("menu echec\n" + traceback.format_exc())
