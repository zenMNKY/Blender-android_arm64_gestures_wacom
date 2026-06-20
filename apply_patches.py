#!/usr/bin/env python3
"""
apply_patches.py
Applies touch gesture + Wacom stylus patches to dshawshank Blender Android source.
Run from the repo root after copying patch files there.
"""
import re, sys, os

def read(p):
    with open(p) as f: return f.read()

def write(p, c):
    with open(p, 'w') as f: f.write(c)

def patch_cmake():
    path = "intern/ghost/CMakeLists.txt"
    c = read(path)
    old = 'intern/GHOST_SystemAndroid.cpp'
    new = old + '\n      intern/GHOST_TouchGestureAndroid.cpp'
    if 'GHOST_TouchGestureAndroid' not in c and old in c:
        c = c.replace(old, new, 1)
        write(path, c)
        print("  CMakeLists.txt patched.")
    else:
        print("  CMakeLists.txt already patched or entry not found.")

def patch_system_h():
    path = "intern/ghost/intern/GHOST_SystemAndroid.h"
    c = read(path)
    if 'GHOST_StylusAndroid' in c:
        print("  GHOST_SystemAndroid.h already patched.")
        return
    # Add includes before first existing #include
    first = c.find('#include')
    if first >= 0:
        ins = '#include "GHOST_TouchGestureAndroid.h"\n#include "GHOST_StylusAndroid.h"\n'
        c = c[:first] + ins + c[first:]
    # Add handler inheritance
    c = re.sub(
        r'(class\s+GHOST_SystemAndroid\s*:\s*public\s+GHOST_System)',
        r'\1, public GHOST_ITouchGestureHandler',
        c)
    # Add members before closing brace
    members = """
  // Touch gesture members
  GHOST_TouchGestureRecognizer *m_gestureRecognizer = nullptr;
  float m_zoomAccum = 0.0f, m_scrollAccumY = 0.0f;
  float m_orbitX = 0.0f, m_orbitY = 0.0f;
  float m_panLastX = 0.0f, m_panLastY = 0.0f;
  bool  m_panActive = false, m_orbitActive = false;
  void onTouchZoom(float d, float x, float y) override;
  void onTouchPan(float dx, float dy, float x, float y) override;
  void onTouchOrbit(float dx, float dy, float x, float y) override;
  void onTouchGestureEnd() override;
  void onTouchTap(float x, float y) override;
  void onTouchLongPress(float x, float y) override;
  void onTouchScroll(float dx, float dy, float x, float y) override;
  // Stylus members
  GHOST_StylusState m_stylusState;
  bool m_emulate3btn = true;
  bool handleStylusEvent(const AInputEvent *event);
"""
    lb = c.rfind('};')
    c = c[:lb] + members + '\n' + c[lb:]
    write(path, c)
    print("  GHOST_SystemAndroid.h patched.")

def patch_system_cpp():
    path = "intern/ghost/intern/GHOST_SystemAndroid.cpp"
    c = read(path)

    # Constructor: add gesture recognizer + stylus emulate pref
    ctor = re.search(r'(GHOST_SystemAndroid::GHOST_SystemAndroid\(\)[^{]*\{)', c)
    if ctor and 'm_gestureRecognizer' not in c:
        ins = '\n  m_gestureRecognizer = new GHOST_TouchGestureRecognizer(this);\n  U.flag |= (1 << 11); // USER_TWOBUTTONMOUSE\n'
        c = c[:ctor.end()] + ins + c[ctor.end():]

    # Destructor
    dtor = re.search(r'(GHOST_SystemAndroid::~GHOST_SystemAndroid\(\)[^{]*\{)', c)
    if dtor and 'delete m_gestureRecognizer' not in c:
        c = c[:dtor.end()] + '\n  delete m_gestureRecognizer;\n' + c[dtor.end():]

    # Motion event pre-filters
    motion = re.search(r'(AINPUT_EVENT_TYPE_MOTION[^\{]*\{)', c)
    if motion and 'handleStylusEvent' not in c:
        prefilter = """
    // Stylus pre-filter
    {
        int32_t _tt = AMotionEvent_getToolType(event, 0);
        if (_tt == AMOTION_EVENT_TOOL_TYPE_STYLUS ||
            _tt == AMOTION_EVENT_TOOL_TYPE_ERASER)
            return handleStylusEvent(event) ? 1 : 0;
    }
    // Gesture recognizer pre-filter
    if (m_gestureRecognizer && m_gestureRecognizer->onMotionEvent(event))
        return 1;
"""
        c = c[:motion.end()] + prefilter + c[motion.end():]

    # Append implementations
    if 'handleStylusEvent' not in c:
        c += STYLUS_IMPL
    if 'onTouchZoom' not in c:
        c += TOUCH_IMPL

    write(path, c)
    print("  GHOST_SystemAndroid.cpp patched.")

# ---------------------------------------------------------------------------
# C++ implementation blocks (plain strings, no special chars that break YAML)
# ---------------------------------------------------------------------------

STYLUS_IMPL = r"""
// ---- Wacom stylus implementation (stylus patch) ---------------------------
#include "GHOST_StylusAndroid.h"

static void pushTMove(GHOST_IWindow *w, GHOST_IEventConsumer *c,
                      int x, int y, const GHOST_TabletData &td) {
    auto *ev = new GHOST_EventCursor(GHOST_System::getMilliSeconds(),
                                     GHOST_kEventCursorMove, w, x, y, td);
    c->processEvent(ev); delete ev;
}
static void pushTBtn(GHOST_IWindow *w, GHOST_IEventConsumer *c,
                     GHOST_TEventType t, GHOST_TButtonMask b,
                     const GHOST_TabletData &td) {
    auto *ev = new GHOST_EventButton(GHOST_System::getMilliSeconds(), t, w, b, td);
    c->processEvent(ev); delete ev;
}

bool GHOST_SystemAndroid::handleStylusEvent(const AInputEvent *event) {
    GHOST_IWindow *win = getActiveWindow();
    GHOST_IEventConsumer *cons = m_windowManager ? getEventConsumer() : nullptr;
    if (!win || !cons) return false;
    int32_t action = AMotionEvent_getAction(event) & AMOTION_EVENT_ACTION_MASK;
    float x = AMotionEvent_getX(event, 0);
    float y = AMotionEvent_getY(event, 0);
    m_stylusState.x = x; m_stylusState.y = y;
    int32_t tt = AMotionEvent_getToolType(event, 0);
    m_stylusState.isEraser = (tt == AMOTION_EVENT_TOOL_TYPE_ERASER);
    m_stylusState.isStylus = true;
    GHOST_TabletData td = GHOST_TABLET_DATA_NONE;
    GHOST_Stylus_FillTabletData(event, 0, td);
    m_stylusState.pressure = td.Pressure;
    bool bc = GHOST_Stylus_UpdateButtons(event, m_stylusState);
    GHOST_TButtonMask eb = GHOST_Stylus_EmulatedButton(m_stylusState);
    pushTMove(win, cons, (int)x, (int)y, td);
    switch (action) {
        case AMOTION_EVENT_ACTION_DOWN:
            if (td.Pressure >= GHOST_STYLUS_PRESSURE_THRESHOLD) {
                m_stylusState.tipDown = true;
                m_stylusState.activeSynthBtn = (int)eb;
                pushTBtn(win, cons, GHOST_kEventButtonDown, eb, td);
            }
            break;
        case AMOTION_EVENT_ACTION_MOVE:
            if (m_stylusState.tipDown && bc) {
                if (m_stylusState.activeSynthBtn >= 0)
                    pushTBtn(win, cons, GHOST_kEventButtonUp,
                             (GHOST_TButtonMask)m_stylusState.activeSynthBtn, td);
                m_stylusState.activeSynthBtn = (int)eb;
                pushTBtn(win, cons, GHOST_kEventButtonDown, eb, td);
            }
            break;
        case AMOTION_EVENT_ACTION_UP:
            if (m_stylusState.tipDown) {
                m_stylusState.tipDown = false;
                if (m_stylusState.activeSynthBtn >= 0) {
                    pushTBtn(win, cons, GHOST_kEventButtonUp,
                             (GHOST_TButtonMask)m_stylusState.activeSynthBtn, td);
                    m_stylusState.activeSynthBtn = -1;
                }
            }
            break;
        case AMOTION_EVENT_ACTION_HOVER_MOVE:
            m_stylusState.hovering = true;
            // Lower barrel (closer to nib) = MMB, upper barrel = RMB
            // btn2 takes priority if both held
            if (bc && !m_stylusState.tipDown) {
                GHOST_TButtonMask hb = m_stylusState.btn2Down
                                       ? GHOST_kButtonMaskRight
                                       : GHOST_kButtonMaskMiddle;
                pushTBtn(win, cons, GHOST_kEventButtonDown, hb, td);
                pushTBtn(win, cons, GHOST_kEventButtonUp,   hb, td);
            }
            break;
        case AMOTION_EVENT_ACTION_HOVER_ENTER:
            m_stylusState.hovering = true; break;
        case AMOTION_EVENT_ACTION_HOVER_EXIT:
        case AMOTION_EVENT_ACTION_CANCEL:
            if (m_stylusState.tipDown && m_stylusState.activeSynthBtn >= 0)
                pushTBtn(win, cons, GHOST_kEventButtonUp,
                         (GHOST_TButtonMask)m_stylusState.activeSynthBtn, td);
            m_stylusState = GHOST_StylusState{};
            break;
        default: break;
    }
    return true;
}
"""

TOUCH_IMPL = r"""
// ---- Touch gesture handler implementations (touch patch) ------------------
static void ghostMv(GHOST_IWindow *w, GHOST_IEventConsumer *c, int x, int y) {
    auto *ev = new GHOST_EventCursor(GHOST_System::getMilliSeconds(),
                                     GHOST_kEventCursorMove, w, x, y,
                                     GHOST_TABLET_DATA_NONE);
    c->processEvent(ev); delete ev;
}
static void ghostBtn(GHOST_IWindow *w, GHOST_IEventConsumer *c,
                     GHOST_TEventType t, GHOST_TButtonMask b) {
    auto *ev = new GHOST_EventButton(GHOST_System::getMilliSeconds(), t, w, b,
                                     GHOST_TABLET_DATA_NONE);
    c->processEvent(ev); delete ev;
}
static void ghostWhl(GHOST_IWindow *w, GHOST_IEventConsumer *c, int d) {
    auto *ev = new GHOST_EventWheel(GHOST_System::getMilliSeconds(), w, d);
    c->processEvent(ev); delete ev;
}

void GHOST_SystemAndroid::onTouchZoom(float delta, float x, float y) {
    auto *win = getActiveWindow(); auto *cons = getEventConsumer();
    if (!win || !cons) return;
    ghostMv(win, cons, (int)x, (int)y);
    m_zoomAccum += delta;
    while (m_zoomAccum >= 1.0f)  { ghostWhl(win, cons, +1); m_zoomAccum -= 1.0f; }
    while (m_zoomAccum <= -1.0f) { ghostWhl(win, cons, -1); m_zoomAccum += 1.0f; }
}
void GHOST_SystemAndroid::onTouchPan(float dx, float dy, float x, float y) {
    auto *win = getActiveWindow(); auto *cons = getEventConsumer();
    if (!win || !cons) return;
    if (!m_panActive) {
        ghostMv(win, cons, (int)x, (int)y);
        ghostBtn(win, cons, GHOST_kEventButtonDown, GHOST_kButtonMaskMiddle);
        m_panActive = true; m_panLastX = x; m_panLastY = y;
    }
    m_panLastX += dx; m_panLastY += dy;
    ghostMv(win, cons, (int)m_panLastX, (int)m_panLastY);
}
void GHOST_SystemAndroid::onTouchOrbit(float dx, float dy, float x, float y) {
    auto *win = getActiveWindow(); auto *cons = getEventConsumer();
    if (!win || !cons) return;
    m_orbitX += dx; m_orbitY += dy;
    ghostMv(win, cons, (int)m_orbitX, (int)m_orbitY);
}
void GHOST_SystemAndroid::onTouchGestureEnd() {
    auto *win = getActiveWindow(); auto *cons = getEventConsumer();
    if (!win || !cons) return;
    if (m_panActive) {
        ghostBtn(win, cons, GHOST_kEventButtonUp, GHOST_kButtonMaskMiddle);
        m_panActive = false;
    }
    m_zoomAccum = 0.0f; m_scrollAccumY = 0.0f;
}
void GHOST_SystemAndroid::onTouchTap(float x, float y) {
    auto *win = getActiveWindow(); auto *cons = getEventConsumer();
    if (!win || !cons) return;
    m_orbitX = x; m_orbitY = y;
    ghostMv(win, cons, (int)x, (int)y);
    ghostBtn(win, cons, GHOST_kEventButtonDown, GHOST_kButtonMaskLeft);
    ghostBtn(win, cons, GHOST_kEventButtonUp,   GHOST_kButtonMaskLeft);
}
void GHOST_SystemAndroid::onTouchLongPress(float x, float y) {
    auto *win = getActiveWindow(); auto *cons = getEventConsumer();
    if (!win || !cons) return;
    ghostMv(win, cons, (int)x, (int)y);
    ghostBtn(win, cons, GHOST_kEventButtonDown, GHOST_kButtonMaskRight);
    ghostBtn(win, cons, GHOST_kEventButtonUp,   GHOST_kButtonMaskRight);
}
void GHOST_SystemAndroid::onTouchScroll(float dx, float dy, float x, float y) {
    auto *win = getActiveWindow(); auto *cons = getEventConsumer();
    if (!win || !cons) return;
    ghostMv(win, cons, (int)x, (int)y);
    m_scrollAccumY += dy;
    while (m_scrollAccumY >  120.0f) { ghostWhl(win, cons, -1); m_scrollAccumY -= 120.0f; }
    while (m_scrollAccumY < -120.0f) { ghostWhl(win, cons, +1); m_scrollAccumY += 120.0f; }
}
"""

if __name__ == '__main__':
    print("Applying patches...")
    patch_cmake()
    patch_system_h()
    patch_system_cpp()
    print("All patches applied.")
