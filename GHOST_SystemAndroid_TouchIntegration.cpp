/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * GHOST_SystemAndroid_TouchIntegration.patch
 * ==========================================
 * This file shows the CHANGES needed in the existing
 *   intern/ghost/intern/GHOST_SystemAndroid.cpp  (and .h)
 * to wire up GHOST_TouchGestureRecognizer.
 *
 * ── How to apply ──────────────────────────────────────────────────────────
 * This is not a literal .patch file – it's annotated code snippets you
 * copy-paste into the right locations in GHOST_SystemAndroid.{h,cpp}.
 * Each section is marked with the function / region it belongs to.
 * ─────────────────────────────────────────────────────────────────────────
 */

/* ═══════════════════════════════════════════════════════════════════════════
 * 1.  GHOST_SystemAndroid.h  –  add member variables & include
 * ═══════════════════════════════════════════════════════════════════════════
 *
 * At the top of the file, add:
 */
#include "GHOST_TouchGestureAndroid.h"   // NEW

/* Inside class GHOST_SystemAndroid : public GHOST_System,
 *                                    public GHOST_ITouchGestureHandler   // ADD inheritance
 * {
 * ...
 * private:                                  // ADD these members:
 *   GHOST_TouchGestureRecognizer *m_gestureRecognizer = nullptr;
 *
 *   // Last synthetic middle-mouse position (for pan end event)
 *   float m_panLastX = 0, m_panLastY = 0;
 *   bool  m_panActive = false;
 *
 *   // GHOST_ITouchGestureHandler overrides:
 *   void onTouchZoom(float delta, float x, float y) override;
 *   void onTouchPan(float dx, float dy, float x, float y) override;
 *   void onTouchOrbit(float dx, float dy, float x, float y) override;
 *   void onTouchGestureEnd() override;
 *   void onTouchTap(float x, float y) override;
 *   void onTouchLongPress(float x, float y) override;
 *   void onTouchScroll(float dx, float dy, float x, float y) override;
 * };
 */


/* ═══════════════════════════════════════════════════════════════════════════
 * 2.  GHOST_SystemAndroid.cpp  –  constructor / destructor
 * ═══════════════════════════════════════════════════════════════════════════
 *
 * In GHOST_SystemAndroid::GHOST_SystemAndroid() add:
 */
// m_gestureRecognizer = new GHOST_TouchGestureRecognizer(this);   // NEW

/* In GHOST_SystemAndroid::~GHOST_SystemAndroid() add: */
// delete m_gestureRecognizer;   // NEW


/* ═══════════════════════════════════════════════════════════════════════════
 * 3.  handleMotionEvent (or equivalent Android input dispatch)
 * ═══════════════════════════════════════════════════════════════════════════
 *
 * Wherever you currently call AInputQueue_getEvent / processEvent for touch,
 * add a pre-filter BEFORE you translate the raw event into a GHOST mouse event:
 *
 *   static int32_t handleInputEvent(android_app *app, AInputEvent *event) {
 *     GHOST_SystemAndroid *sys = (GHOST_SystemAndroid *)app->userData;
 *
 *     if (AInputEvent_getType(event) == AINPUT_EVENT_TYPE_MOTION) {
 *       // Let the gesture recognizer have first look.
 *       bool consumed = sys->m_gestureRecognizer->onMotionEvent(event);
 *       if (consumed) return 1;   // gesture handled it; skip raw processing
 *     }
 *
 *     // existing raw event handling continues below …
 *     return sys->handleMotionEvent(event);
 *   }
 */


/* ═══════════════════════════════════════════════════════════════════════════
 * 4.  GHOST_ITouchGestureHandler implementations
 *     Paste these as new methods in GHOST_SystemAndroid.cpp
 * ═══════════════════════════════════════════════════════════════════════════ */

#include "GHOST_EventCursor.h"
#include "GHOST_EventButton.h"
#include "GHOST_EventWheel.h"
#include "GHOST_EventKey.h"

/* ── Helper: push a synthetic mouse move ── */
static void pushMove(GHOST_WindowAndroid *win, GHOST_IEventConsumer *cons,
                     int x, int y)
{
    GHOST_EventCursor *ev = new GHOST_EventCursor(
        GHOST_GetEventTime(), GHOST_kEventCursorMove, win, x, y, GHOST_TABLET_DATA_NONE);
    cons->processEvent(ev);
    delete ev;
}

/* ── Helper: push a synthetic mouse button ── */
static void pushButton(GHOST_WindowAndroid *win, GHOST_IEventConsumer *cons,
                       GHOST_TEventType type, GHOST_TButtonMask btn, int x, int y)
{
    GHOST_EventButton *ev = new GHOST_EventButton(
        GHOST_GetEventTime(), type, win, btn, GHOST_TABLET_DATA_NONE);
    (void)x; (void)y;
    cons->processEvent(ev);
    delete ev;
}

/* ── Helper: push a synthetic wheel event ── */
static void pushWheel(GHOST_WindowAndroid *win, GHOST_IEventConsumer *cons, int delta)
{
    GHOST_EventWheel *ev = new GHOST_EventWheel(GHOST_GetEventTime(), win, delta);
    cons->processEvent(ev);
    delete ev;
}

/* ─────────────────────────────────────────────────────────────────────────── */

void GHOST_SystemAndroid::onTouchZoom(float delta, float x, float y)
{
    /* Map pinch to mouse-wheel scroll: Blender's numpad zoom uses wheel */
    GHOST_WindowAndroid *win = (GHOST_WindowAndroid *)getActiveWindow();
    if (!win) return;
    GHOST_IEventConsumer *cons = m_windowManager->getActiveWindow() ?
                                 getEventConsumer() : nullptr;
    if (!cons) return;

    /* Move cursor to centroid first */
    pushMove(win, cons, (int)x, (int)y);

    /* Accumulate fractional clicks */
    m_zoomAccum += delta;
    while (m_zoomAccum >= 1.0f) {
        pushWheel(win, cons, +1);    /* zoom in  */
        m_zoomAccum -= 1.0f;
    }
    while (m_zoomAccum <= -1.0f) {
        pushWheel(win, cons, -1);    /* zoom out */
        m_zoomAccum += 1.0f;
    }
}

void GHOST_SystemAndroid::onTouchPan(float dx, float dy, float x, float y)
{
    /* 2-finger pan → middle-mouse drag */
    GHOST_WindowAndroid *win = (GHOST_WindowAndroid *)getActiveWindow();
    if (!win) return;
    GHOST_IEventConsumer *cons = getEventConsumer();
    if (!cons) return;

    if (!m_panActive) {
        /* Press middle button once */
        pushMove(win, cons, (int)x, (int)y);
        pushButton(win, cons, GHOST_kEventButtonDown, GHOST_kButtonMaskMiddle,
                   (int)x, (int)y);
        m_panActive = true;
    }

    /* Move cursor by delta */
    m_panLastX += dx;
    m_panLastY += dy;
    pushMove(win, cons, (int)m_panLastX, (int)m_panLastY);
}

void GHOST_SystemAndroid::onTouchOrbit(float dx, float dy, float x, float y)
{
    /* 1-finger drag → plain cursor move (Blender orbit = middle-mouse in 3D
     * viewport, but here we just move the pointer so Blender's own keymap
     * handles it; caller should ensure MMB is emulated if needed). */
    GHOST_WindowAndroid *win = (GHOST_WindowAndroid *)getActiveWindow();
    if (!win) return;
    GHOST_IEventConsumer *cons = getEventConsumer();
    if (!cons) return;

    m_orbitX += dx;
    m_orbitY += dy;
    pushMove(win, cons, (int)m_orbitX, (int)m_orbitY);
}

void GHOST_SystemAndroid::onTouchGestureEnd()
{
    GHOST_WindowAndroid *win = (GHOST_WindowAndroid *)getActiveWindow();
    if (!win) return;
    GHOST_IEventConsumer *cons = getEventConsumer();
    if (!cons) return;

    if (m_panActive) {
        pushButton(win, cons, GHOST_kEventButtonUp, GHOST_kButtonMaskMiddle,
                   (int)m_panLastX, (int)m_panLastY);
        m_panActive = false;
    }
    m_zoomAccum = 0.0f;
}

void GHOST_SystemAndroid::onTouchTap(float x, float y)
{
    GHOST_WindowAndroid *win = (GHOST_WindowAndroid *)getActiveWindow();
    if (!win) return;
    GHOST_IEventConsumer *cons = getEventConsumer();
    if (!cons) return;

    /* Update cursor, then synthesize LMB click */
    m_orbitX = x; m_orbitY = y;
    pushMove(win, cons, (int)x, (int)y);
    pushButton(win, cons, GHOST_kEventButtonDown, GHOST_kButtonMaskLeft, (int)x, (int)y);
    pushButton(win, cons, GHOST_kEventButtonUp,   GHOST_kButtonMaskLeft, (int)x, (int)y);
}

void GHOST_SystemAndroid::onTouchLongPress(float x, float y)
{
    GHOST_WindowAndroid *win = (GHOST_WindowAndroid *)getActiveWindow();
    if (!win) return;
    GHOST_IEventConsumer *cons = getEventConsumer();
    if (!cons) return;

    pushMove(win, cons, (int)x, (int)y);
    pushButton(win, cons, GHOST_kEventButtonDown, GHOST_kButtonMaskRight, (int)x, (int)y);
    pushButton(win, cons, GHOST_kEventButtonUp,   GHOST_kButtonMaskRight, (int)x, (int)y);
}

void GHOST_SystemAndroid::onTouchScroll(float dx, float dy, float x, float y)
{
    /* 2-finger pan over UI panels → scroll wheel equivalent */
    GHOST_WindowAndroid *win = (GHOST_WindowAndroid *)getActiveWindow();
    if (!win) return;
    GHOST_IEventConsumer *cons = getEventConsumer();
    if (!cons) return;

    pushMove(win, cons, (int)x, (int)y);

    /* Accumulate and emit discrete scroll steps */
    m_scrollAccumY += dy;
    while (m_scrollAccumY >  GHOST_TOUCH_SCROLL_STEP) {
        pushWheel(win, cons, -1);   /* scroll down */
        m_scrollAccumY -= GHOST_TOUCH_SCROLL_STEP;
    }
    while (m_scrollAccumY < -GHOST_TOUCH_SCROLL_STEP) {
        pushWheel(win, cons, +1);   /* scroll up */
        m_scrollAccumY += GHOST_TOUCH_SCROLL_STEP;
    }
}


/* ═══════════════════════════════════════════════════════════════════════════
 * 5.  Additional member variables needed in GHOST_SystemAndroid.h  (private)
 * ═══════════════════════════════════════════════════════════════════════════
 *
 *   float m_zoomAccum    = 0.0f;   // fractional zoom accumulator
 *   float m_scrollAccumY = 0.0f;   // fractional scroll accumulator
 *   float m_orbitX = 0, m_orbitY = 0;  // current virtual cursor pos
 *   float m_panLastX = 0, m_panLastY = 0;
 *   bool  m_panActive = false;
 */


/* ═══════════════════════════════════════════════════════════════════════════
 * 6.  BONUS – Emulate middle-mouse-button for 1-finger orbit
 *     (Blender's default 3D viewport orbit requires MMB held down)
 * ═══════════════════════════════════════════════════════════════════════════
 *
 * Option A: Enable Blender's "Emulate 3 Button Mouse" preference at startup.
 *           Alt+LMB = orbit, Ctrl+LMB = zoom, Shift+LMB = pan.
 *           Set in your startup .py or from C via BKE_preferences:
 *
 *   UserDef *uprefs = BKE_blendfile_userdef_from_defaults();
 *   uprefs->flag |= USER_TWOBUTTONMOUSE;     // emulate 3 buttons
 *   BKE_blendfile_userdef_write_app_template(uprefs, ...);
 *
 * Option B: Synthesize MMB down on drag start (add to onTouchOrbit):
 *
 *   if (!m_orbitActive) {
 *       pushButton(win, cons, GHOST_kEventButtonDown, GHOST_kButtonMaskMiddle, ...);
 *       m_orbitActive = true;
 *   }
 *   // and MMB up in onTouchGestureEnd().
 *
 * Option A is recommended – it requires no extra event synthesis.
 */


/* ═══════════════════════════════════════════════════════════════════════════
 * 7.  CMakeLists.txt – add the new .cpp to the Android build
 * ═══════════════════════════════════════════════════════════════════════════
 *
 * In intern/ghost/CMakeLists.txt, locate the Android block (look for
 * GHOST_SystemAndroid.cpp) and add the gesture file:
 *
 *   if(WITH_GHOST_ANDROID)
 *     list(APPEND SRC
 *       intern/GHOST_SystemAndroid.cpp
 *       intern/GHOST_TouchGestureAndroid.cpp    # <── ADD THIS LINE
 *       intern/GHOST_WindowAndroid.cpp
 *     )
 *     list(APPEND INC
 *       intern
 *     )
 *   endif()
 */
