# -*- coding: utf-8 -*-
# ARRIRAW (art-cmd) and Blackmagic RAW (braw2exr) import - see natron_camraw.py.
# Natron runs this file in __main__: the code lives in its own module so its
# names do not mix with the other PyPlugs' (R3D_Import, 3DE_Lens_Import...).
try:
    import natron_camraw as _natron_camraw
    _natron_camraw.register()
except Exception:
    import traceback as _tb
    print("[Camera RAW Import] registration failed\n" + _tb.format_exc())
