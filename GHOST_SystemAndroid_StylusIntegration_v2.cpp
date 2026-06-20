/* SPDX-License-Identifier: GPL-2.0-or-later
 * GHOST_SystemAndroid_StylusIntegration.cpp
 *
 * Annotated integration guide for wiring GHOST_StylusAndroid.h into the
 * existing GHOST_SystemAndroid.cpp / .h files.
 *
 * Apply AFTER the touch-gesture patch (GHOST_SystemAndroid_TouchIntegration.cpp).
 * The two patches are fully compatible — this one adds stylus handling alongside
 * the existing multi-touch gesture recognizer.
 *
 * ── Overview of changes ────────────────────────────────────────────────────
 *
 *  GHOST_SystemAndroid.h
 *    • #include "GHOST_StylusAndroid.h"
 *    • Add m_stylusState member
 *    • Add m_emulate3btn member (flag to enable/disable at runtime)
 *
 *  GHOST_SystemAndroid.cpp  (constructor)
 *    • Enable Blender's "Emulate 3 Button Mouse" preference via BKE_preferences
 *
 *  GHOST_SystemAndroid.cpp  (handleMotionEvent)
 *    • Detect stylus tool type before the gesture recognizer
 *    • Route stylus events to new handleStylusEvent()
 *
 *  New method: GHOST_SystemAndroid::handleStylusEvent()
 *    • Reads pressure, tilt, buttons
 *    • Synthesizes correct GHOST mouse button + tablet data
 *    • Implements emulate-3-button logic
 *
 * ── Compatibility notes ─────────────────────────────────────────────────────
 *  • Requires Android API 26+ (android:minSdkVersion="26" in AndroidManifest)
 *    AMOTION_EVENT_BUTTON_STYLUS_PRIMARY/_SECONDARY are API 23, but
 *    AXIS_TILT is API 26. CTL-672 Android 14+ requirement aligns with this.
 *  • CTL-672 connects via USB-OTG. Android routes it as a stylus input device.
 *    No additional driver needed on Android 14+.
 * ────────────────────────────────────────────────────────────────────────── */

/* ═══════════════════════════════════════════════════════════════════════════
 * 1.  GHOST_SystemAndroid.h  —  additions
 * ═══════════════════════════════════════════════════════════════════════════ */

/*  At the top of the file, after existing includes: */
// #include "GHOST_StylusAndroid.h"    ← ADD

/*  Inside class GHOST_SystemAndroid, in the private section: */
//   GHOST_StylusState m_stylusState;          // pen/eraser state
//   bool              m_emulate3btn = true;    // mirrors Blender preference

/*  Add method declaration: */
//   bool handleStylusEvent(const AInputEvent *event);


/* ═══════════════════════════════════════════════════════════════════════════
 * 2.  Constructor  —  enable Blender's emulate-3-button-mouse pref
 * ═══════════════════════════════════════════════════════════════════════════
 *
 * Blender reads UserDef.flag & USER_TWOBUTTONMOUSE at startup to decide
 * whether Alt+LMB → orbit, Ctrl+LMB → zoom, Shift+LMB → pan.
 * With the stylus we handle this ourselves (barrel button → MMB/RMB),
 * so we set the preference so Blender's UI labels show the right hints.
 *
 * Paste this INSIDE GHOST_SystemAndroid::GHOST_SystemAndroid() constructor,
 * after the existing init code:
 */

/*
    // Enable "Emulate 3 Button Mouse" so Blender's keymap hints match our
    // stylus barrel-button mapping. This is equivalent to going to
    // Preferences → Input → Mouse → Emulate 3 Button Mouse.
    //
    // BKE_preferences_init() must have been called before GHOST init, which
    // it is in blender_main() before ghost system creation.
    //
    // If this call isn't available in this build, set it via Python startup
    // script instead (see Section 5 below).
    if (G_MAIN && G_MAIN->filename[0]) {
        // Full Blender context available
        U.flag |= USER_TWOBUTTONMOUSE;
    }
    // Unconditional fallback — always safe:
    // The flag will be set; Blender checks it each time it processes input.
    // (Works even before G_MAIN is fully initialised because the flag is
    //  in global UserDef struct U, which is zeroed+defaulted at process start.)
    extern UserDef U;   // declared in BKE_blender.h
    U.flag |= USER_TWOBUTTONMOUSE;
*/


/* ═══════════════════════════════════════════════════════════════════════════
 * 3.  handleMotionEvent  —  route stylus before gesture recognizer
 * ═══════════════════════════════════════════════════════════════════════════
 *
 * Find the section in GHOST_SystemAndroid.cpp that handles
 * AINPUT_EVENT_TYPE_MOTION events (where we already added the gesture
 * recognizer pre-filter) and add the stylus pre-filter BEFORE it:
 *
 *   if (AInputEvent_getType(event) == AINPUT_EVENT_TYPE_MOTION) {
 *
 *       // ── Stylus / pen pre-filter (added by stylus patch) ──────────────
 *       int32_t toolType = AMotionEvent_getToolType(event, 0);
 *       if (toolType == AMOTION_EVENT_TOOL_TYPE_STYLUS ||
 *           toolType == AMOTION_EVENT_TOOL_TYPE_ERASER)
 *       {
 *           return handleStylusEvent(event) ? 1 : 0;
 *       }
 *       // ─────────────────────────────────────────────────────────────────
 *
 *       // existing gesture recognizer pre-filter:
 *       if (m_gestureRecognizer) {
 *           if (m_gestureRecognizer->onMotionEvent(event)) return 1;
 *       }
 *   }
 */


/* ═══════════════════════════════════════════════════════════════════════════
 * 4.  New method: GHOST_SystemAndroid::handleStylusEvent()
 *     Paste this as a new function in GHOST_SystemAndroid.cpp
 * ═══════════════════════════════════════════════════════════════════════════ */

#include "GHOST_StylusAndroid.h"
#include "GHOST_EventCursor.h"
#include "GHOST_EventButton.h"

/* Internal helper — push a cursor move with tablet data */
static void pushTabletMove(GHOST_IWindow *win,
                           GHOST_IEventConsumer *cons,
                           int x, int y,
                           const GHOST_TabletData &td)
{
    GHOST_EventCursor *ev = new GHOST_EventCursor(
        GHOST_System::getMilliSeconds(),
        GHOST_kEventCursorMove,
        win, x, y, td);
    cons->processEvent(ev);
    delete ev;
}

/* Internal helper — push a mouse button event */
static void pushStylusButton(GHOST_IWindow *win,
                             GHOST_IEventConsumer *cons,
                             GHOST_TEventType type,
                             GHOST_TButtonMask btn,
                             const GHOST_TabletData &td)
{
    GHOST_EventButton *ev = new GHOST_EventButton(
        GHOST_System::getMilliSeconds(),
        type, win, btn, td);
    cons->processEvent(ev);
    delete ev;
}

bool GHOST_SystemAndroid::handleStylusEvent(const AInputEvent *event)
{
    GHOST_IWindow *win = getActiveWindow();
    GHOST_IEventConsumer *cons = m_windowManager ? getEventConsumer() : nullptr;
    if (!win || !cons) return false;

    const int32_t actionMasked = AMotionEvent_getAction(event)
                                 & AMOTION_EVENT_ACTION_MASK;

    /* ── Read current pen position ── */
    float x = AMotionEvent_getX(event, 0);
    float y = AMotionEvent_getY(event, 0);
    m_stylusState.x = x;
    m_stylusState.y = y;

    /* ── Read tool type ── */
    int32_t toolType = AMotionEvent_getToolType(event, 0);
    m_stylusState.isEraser = (toolType == AMOTION_EVENT_TOOL_TYPE_ERASER);
    m_stylusState.isStylus = true;

    /* ── Fill tablet data (pressure, tilt, active mode) ── */
    GHOST_TabletData td = GHOST_TABLET_DATA_NONE;
    GHOST_Stylus_FillTabletData(event, 0, td);
    m_stylusState.pressure = td.Pressure;

    /* ── Read barrel buttons ── */
    bool buttonsChanged = GHOST_Stylus_UpdateButtons(event, m_stylusState);

    /* ── Determine which GHOST button the tip should act as ── */
    GHOST_TButtonMask emulatedBtn = GHOST_Stylus_EmulatedButton(m_stylusState);

    /* ── Send cursor move with tablet data (always) ── */
    pushTabletMove(win, cons, (int)x, (int)y, td);

    switch (actionMasked) {

        /* ── Pen touched surface ── */
        case AMOTION_EVENT_ACTION_DOWN:
            if (td.Pressure >= GHOST_STYLUS_PRESSURE_THRESHOLD) {
                m_stylusState.tipDown = true;
                m_stylusState.activeSynthBtn = (int)emulatedBtn;
                pushStylusButton(win, cons, GHOST_kEventButtonDown, emulatedBtn, td);
            }
            break;

        /* ── Pen moving on surface ── */
        case AMOTION_EVENT_ACTION_MOVE:
            /* If a barrel button changed mid-stroke, re-synthesize:
             * release the old button, press the new one.
             * This lets you start drawing (LMB) then hold the barrel
             * to switch to orbit (MMB) without lifting the pen. */
            if (m_stylusState.tipDown && buttonsChanged) {
                /* Release old */
                if (m_stylusState.activeSynthBtn >= 0) {
                    pushStylusButton(win, cons, GHOST_kEventButtonUp,
                        (GHOST_TButtonMask)m_stylusState.activeSynthBtn, td);
                }
                /* Press new */
                m_stylusState.activeSynthBtn = (int)emulatedBtn;
                pushStylusButton(win, cons, GHOST_kEventButtonDown, emulatedBtn, td);
            }
            break;

        /* ── Pen lifted ── */
        case AMOTION_EVENT_ACTION_UP:
            if (m_stylusState.tipDown) {
                m_stylusState.tipDown = false;
                if (m_stylusState.activeSynthBtn >= 0) {
                    pushStylusButton(win, cons, GHOST_kEventButtonUp,
                        (GHOST_TButtonMask)m_stylusState.activeSynthBtn, td);
                    m_stylusState.activeSynthBtn = -1;
                }
            }
            break;

        /* ── Pen hovering above tablet (not touching) ── */
        case AMOTION_EVENT_ACTION_HOVER_MOVE:
            m_stylusState.hovering = true;
            /* Hover move already sent by pushTabletMove() above.
             * Barrel buttons while hovering (pen not touching surface):
             *   Lower barrel (btn1, closer to nib) → MMB click (orbit/pan)
             *   Upper barrel (btn2, further from nib) → RMB click (context menu)
             *   btn2 takes priority if both held simultaneously. */
            if (buttonsChanged && !m_stylusState.tipDown) {
                GHOST_TButtonMask hoverBtn = m_stylusState.btn2Down
                                             ? GHOST_kButtonMaskRight
                                             : GHOST_kButtonMaskMiddle;
                pushStylusButton(win, cons, GHOST_kEventButtonDown, hoverBtn, td);
                pushStylusButton(win, cons, GHOST_kEventButtonUp,   hoverBtn, td);
            }
            break;

        case AMOTION_EVENT_ACTION_HOVER_ENTER:
            m_stylusState.hovering = true;
            break;

        case AMOTION_EVENT_ACTION_HOVER_EXIT:
            m_stylusState.hovering = false;
            /* Ensure any stuck buttons are released */
            if (m_stylusState.tipDown) {
                m_stylusState.tipDown = false;
                if (m_stylusState.activeSynthBtn >= 0) {
                    pushStylusButton(win, cons, GHOST_kEventButtonUp,
                        (GHOST_TButtonMask)m_stylusState.activeSynthBtn, td);
                    m_stylusState.activeSynthBtn = -1;
                }
            }
            break;

        case AMOTION_EVENT_ACTION_CANCEL:
            /* Tablet pulled away or interrupted */
            if (m_stylusState.tipDown && m_stylusState.activeSynthBtn >= 0) {
                pushStylusButton(win, cons, GHOST_kEventButtonUp,
                    (GHOST_TButtonMask)m_stylusState.activeSynthBtn, td);
            }
            m_stylusState = GHOST_StylusState{};  /* reset */
            break;

        default:
            break;
    }

    return true;  /* event consumed */
}


/* ═══════════════════════════════════════════════════════════════════════════
 * 5.  Enable "Emulate 3 Button Mouse" via Python startup (alternative to §2)
 *     Use this if you can't modify BKE/G_MAIN C code easily.
 * ═══════════════════════════════════════════════════════════════════════════
 *
 * Create a file:  scripts/startup/bl_app_override/userpref.py
 * (inside Blender's data directory on the device, e.g.
 *  /sdcard/Android/data/org.blender.blender/files/scripts/startup/...)
 *
 * Contents:
 *
 *   import bpy
 *
 *   def enable_tablet_prefs():
 *       prefs = bpy.context.preferences
 *       inp   = prefs.inputs
 *
 *       # Emulate 3 Button Mouse:
 *       # pen tip alone    = LMB
 *       # pen tip + btn 1  = MMB  (orbit)  ← our GHOST patch handles this
 *       # pen tip + btn 2  = RMB  (menu)   ← our GHOST patch handles this
 *       inp.use_mouse_emulate_3_button = True
 *
 *       # Emulate Numpad (useful on tablet — no numpad on screen):
 *       inp.use_numpad_as_hotkeys = True
 *
 *       # Tablet API — force "Wintab" style so pressure always reaches draw tools:
 *       inp.tablet_api = 'NONE'  # 'NONE' = use system default (correct for Android)
 *
 *       # Make sure pressure is applied to brush strength:
 *       # (these are per-brush, set them on the default brush as a baseline)
 *       for brush in bpy.data.brushes:
 *           brush.use_pressure_strength = True
 *           brush.use_pressure_size     = False  # set True if you want pressure→size
 *
 *   # Register as a handler that fires once Blender is fully loaded:
 *   import bpy.app.handlers
 *   if enable_tablet_prefs not in bpy.app.handlers.load_post:
 *       bpy.app.handlers.load_post.append(lambda _: enable_tablet_prefs())
 *   # Also call immediately in case a file is already loaded:
 *   try:
 *       enable_tablet_prefs()
 *   except Exception:
 *       pass  # Blender not fully initialised yet — handler will catch it
 */


/* ═══════════════════════════════════════════════════════════════════════════
 * 6.  CMakeLists.txt — add GHOST_StylusAndroid.h (header only, no .cpp)
 * ═══════════════════════════════════════════════════════════════════════════
 *
 * GHOST_StylusAndroid.h is header-only (all inline/constexpr), so no .cpp
 * entry is needed. Just make sure it is in the same intern/ directory and
 * the include path is already set (it is for all files in that directory).
 *
 * Nothing to add to CMakeLists.txt for this patch. ✓
 */


/* ═══════════════════════════════════════════════════════════════════════════
 * 7.  AndroidManifest.xml — declare USB-OTG stylus feature
 * ═══════════════════════════════════════════════════════════════════════════
 *
 * In APP-android_arm64/app/src/main/AndroidManifest.xml, inside <manifest>:
 *
 *   <!-- Declare tablet/stylus input feature (not required, improves discoverability) -->
 *   <uses-feature android:name="android.hardware.touchscreen.stylus"
 *                 android:required="false" />
 *
 *   <!-- USB-OTG for wired tablet (CTL-672 connects via USB) -->
 *   <uses-feature android:name="android.hardware.usb.host"
 *                 android:required="false" />
 *
 * Also ensure minSdkVersion is at least 26:
 *   <uses-sdk android:minSdkVersion="26" android:targetSdkVersion="34" />
 *
 * Or in build.gradle:
 *   android {
 *       defaultConfig {
 *           minSdkVersion 26
 *           targetSdkVersion 34
 *       }
 *   }
 */


/* ═══════════════════════════════════════════════════════════════════════════
 * 8.  GHOST_SystemAndroid.h  — final private members list (both patches)
 * ═══════════════════════════════════════════════════════════════════════════
 *
 * After applying both the touch-gesture patch and this stylus patch, the
 * full set of new private members in GHOST_SystemAndroid should be:
 *
 *   // ── Touch gesture (from touch patch) ──
 *   GHOST_TouchGestureRecognizer *m_gestureRecognizer = nullptr;
 *   float m_zoomAccum    = 0.0f;
 *   float m_scrollAccumY = 0.0f;
 *   float m_orbitX = 0.0f, m_orbitY = 0.0f;
 *   float m_panLastX = 0.0f, m_panLastY = 0.0f;
 *   bool  m_panActive   = false;
 *   bool  m_orbitActive = false;
 *
 *   // ── Stylus / Wacom (from this patch) ──
 *   GHOST_StylusState m_stylusState;
 *   bool              m_emulate3btn = true;
 *
 *   // ── Method declarations (stylus) ──
 *   bool handleStylusEvent(const AInputEvent *event);
 */
