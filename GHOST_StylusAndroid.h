/* SPDX-License-Identifier: GPL-2.0-or-later
 * GHOST_StylusAndroid.h
 *
 * Drop into: intern/ghost/intern/GHOST_StylusAndroid.h
 *
 * Full Wacom CTL-672 stylus support for Blender Android port.
 * Handles:
 *   - Pen tip pressure (0.0–1.0 → GHOST_TabletData)
 *   - Pen tip down / up
 *   - Pen hover (AMOTION_EVENT_ACTION_HOVER_MOVE)
 *   - Side button 1  (lower barrel) → GHOST_kButtonMaskMiddle  (= MMB orbit)
 *   - Side button 2  (upper barrel) → GHOST_kButtonMaskRight   (= RMB menu)
 *   - Eraser tip     → dedicated eraser flag in GHOST_TabletData
 *   - Tilt X/Y       → GHOST_TabletData.Xtilt / Ytilt
 *   - Emulate-3-button-mouse logic so Blender's built-in
 *     USER_TWOBUTTONMOUSE preference works out-of-the-box:
 *       Pen tip alone          → LMB
 *       Pen tip + side btn 1   → MMB  (orbit in 3-D viewport)
 *       Pen tip + side btn 2   → RMB  (context menu)
 *     This mirrors exactly what Blender does on desktop when
 *     "Emulate 3 Button Mouse" is ON.
 *
 * Android API constants used (all available from android/input.h, API 26+):
 *   AMOTION_EVENT_TOOL_TYPE_STYLUS
 *   AMOTION_EVENT_TOOL_TYPE_ERASER
 *   AMOTION_EVENT_BUTTON_STYLUS_PRIMARY    (lower barrel button)
 *   AMOTION_EVENT_BUTTON_STYLUS_SECONDARY  (upper barrel button)
 *   AMOTION_EVENT_AXIS_PRESSURE
 *   AMOTION_EVENT_AXIS_TILT
 *   AMOTION_EVENT_AXIS_ORIENTATION
 */

#pragma once

#include <android/input.h>
#include <cmath>

/* Forward-declared GHOST types used here */
struct GHOST_TabletData;

/* ── Tunable constants ─────────────────────────────────────────────────────── */
/* Minimum pressure to register as a "click" (avoids accidental touches) */
#ifndef GHOST_STYLUS_PRESSURE_THRESHOLD
#  define GHOST_STYLUS_PRESSURE_THRESHOLD  0.02f
#endif

/* ── Pen state ──────────────────────────────────────────────────────────────── */
struct GHOST_StylusState {
    bool  isStylus      = false;   /* current event is from a stylus tool */
    bool  isEraser      = false;   /* tool type is TOOL_TYPE_ERASER        */
    bool  tipDown       = false;   /* pen tip is currently touching tablet */
    bool  btn1Down      = false;   /* lower barrel button held             */
    bool  btn2Down      = false;   /* upper barrel button held             */
    bool  hovering      = false;   /* pen in range but not touching        */
    float pressure      = 0.0f;
    float tiltX         = 0.0f;   /* radians, -π/2..π/2                   */
    float tiltY         = 0.0f;
    float x             = 0.0f;
    float y             = 0.0f;

    /* Which GHOST button is currently synthesized as "down"
     * (used to send a matching UP when the button changes) */
    int   activeSynthBtn = -1;     /* -1 = none, 0=LMB, 1=MMB, 2=RMB      */
};

/* ── Helper functions ───────────────────────────────────────────────────────── */

/**
 * Fill a GHOST_TabletData struct from an Android MotionEvent.
 * Call this whenever you have a stylus event.
 */
inline void GHOST_Stylus_FillTabletData(const AInputEvent *event,
                                        int pointerIdx,
                                        GHOST_TabletData &out)
{
    /* pressure 0..1 */
    out.Pressure = AMotionEvent_getPressure(event, pointerIdx);

    /* tilt: Android AXIS_TILT gives the angle from vertical (0 = perpendicular
     * to surface). AXIS_ORIENTATION gives the azimuth (compass angle).
     * Convert to Blender's Xtilt/Ytilt (signed, -1..1 range). */
    float tilt   = AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_TILT, pointerIdx);
    float orient = AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_ORIENTATION, pointerIdx);

    out.Xtilt = std::sin(tilt) * std::sin(orient);
    out.Ytilt = std::sin(tilt) * std::cos(orient);

    /* Active = stylus or eraser */
    int32_t toolType = AMotionEvent_getToolType(event, pointerIdx);
    out.Active = (toolType == AMOTION_EVENT_TOOL_TYPE_ERASER)
                     ? GHOST_kTabletModeEraser
                     : GHOST_kTabletModeStylus;
}

/**
 * Read the stylus button state from a MotionEvent and update GHOST_StylusState.
 * Returns true if anything changed (so caller knows whether to synthesize events).
 */
inline bool GHOST_Stylus_UpdateButtons(const AInputEvent *event,
                                       GHOST_StylusState &state)
{
    int32_t btns = AMotionEvent_getButtonState(event);
    bool b1 = (btns & AMOTION_EVENT_BUTTON_STYLUS_PRIMARY)   != 0;
    bool b2 = (btns & AMOTION_EVENT_BUTTON_STYLUS_SECONDARY) != 0;

    bool changed = (b1 != state.btn1Down) || (b2 != state.btn2Down);
    state.btn1Down = b1;
    state.btn2Down = b2;
    return changed;
}

/**
 * Determine which GHOST button the pen tip should act as, implementing
 * Blender's "Emulate 3 Button Mouse" logic:
 *
 *   No button held  → LMB  (select, confirm)
 *   Side btn 1 held → MMB  (orbit / pan — same as real middle mouse)
 *   Side btn 2 held → RMB  (context menu)
 *   Eraser tip      → RMB  (erase / context menu, consistent with desktop)
 */
inline GHOST_TButtonMask GHOST_Stylus_EmulatedButton(const GHOST_StylusState &state)
{
    if (state.isEraser)  return GHOST_kButtonMaskRight;
    if (state.btn2Down)  return GHOST_kButtonMaskRight;
    if (state.btn1Down)  return GHOST_kButtonMaskMiddle;
    return GHOST_kButtonMaskLeft;
}
