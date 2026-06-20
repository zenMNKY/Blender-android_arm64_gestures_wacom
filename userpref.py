import bpy
import bpy.app.handlers


def _setup():
    try:
        inp = bpy.context.preferences.inputs
        inp.use_mouse_emulate_3_button = True
        inp.use_numpad_as_hotkeys = True
        inp.tablet_api = 'NONE'
        for b in bpy.data.brushes:
            b.use_pressure_strength = True
        print("[Wacom Android] Prefs applied.")
    except Exception as e:
        print("[Wacom Android] " + str(e))


if _setup not in bpy.app.handlers.load_post:
    bpy.app.handlers.load_post.append(lambda _: _setup())
try:
    _setup()
except Exception:
    pass
