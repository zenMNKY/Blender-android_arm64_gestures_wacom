/* SPDX-License-Identifier: GPL-2.0-or-later
 * GHOST_TouchGestureAndroid.cpp
 *
 * Drop into:  intern/ghost/intern/GHOST_TouchGestureAndroid.cpp
 * Add to:     intern/ghost/CMakeLists.txt  (see bottom of this file)
 */

#include "GHOST_TouchGestureAndroid.h"

#include <algorithm>
#include <cmath>

/* ── public entry point ──────────────────────────────────────────────────── */

bool GHOST_TouchGestureRecognizer::onMotionEvent(const AInputEvent *event)
{
    const int32_t action    = AMotionEvent_getAction(event);
    const int32_t actionMask = action & AMOTION_EVENT_ACTION_MASK;
    const int32_t pointerIdx = (action & AMOTION_EVENT_ACTION_POINTER_INDEX_MASK)
                               >> AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT;

    /* ── update finger table ── */
    const int pointerCount = (int)AMotionEvent_getPointerCount(event);
    for (int i = 0; i < pointerCount && i < MAX_FINGERS; i++) {
        int32_t id = AMotionEvent_getPointerId(event, i);
        if (id < MAX_FINGERS) {
            m_fingers[id].x      = AMotionEvent_getX(event, i);
            m_fingers[id].y      = AMotionEvent_getY(event, i);
            m_fingers[id].active = true;
        }
    }

    /* Count currently active fingers */
    m_activeCount = 0;
    for (int i = 0; i < MAX_FINGERS; i++) {
        if (m_fingers[i].active) m_activeCount++;
    }

    /* Convenience: positions of first two active fingers */
    int fi[2] = {-1, -1};
    int found = 0;
    for (int i = 0; i < MAX_FINGERS && found < 2; i++) {
        if (m_fingers[i].active) fi[found++] = i;
    }

    switch (actionMask) {
        /* ── pointer down ── */
        case AMOTION_EVENT_ACTION_DOWN:
        case AMOTION_EVENT_ACTION_POINTER_DOWN:
            if (m_activeCount == 1 && fi[0] >= 0) {
                handleOneFingerDown(m_fingers[fi[0]].x, m_fingers[fi[0]].y);
                return false; /* single tap still feeds Blender normally */
            }
            if (m_activeCount == 2 && fi[0] >= 0 && fi[1] >= 0) {
                handleTwoFingerDown(m_fingers[fi[0]].x, m_fingers[fi[0]].y,
                                    m_fingers[fi[1]].x, m_fingers[fi[1]].y);
                return true;
            }
            break;

        /* ── move ── */
        case AMOTION_EVENT_ACTION_MOVE:
            if (m_activeCount == 1 && fi[0] >= 0) {
                handleOneFingerMove(m_fingers[fi[0]].x, m_fingers[fi[0]].y);
                /* In DRAG state we consume the event; otherwise pass through */
                return (m_state == State::ONE_FINGER_DRAG);
            }
            if (m_activeCount >= 2 && fi[0] >= 0 && fi[1] >= 0) {
                handleTwoFingerMove(m_fingers[fi[0]].x, m_fingers[fi[0]].y,
                                    m_fingers[fi[1]].x, m_fingers[fi[1]].y);
                return true;
            }
            break;

        /* ── pointer up ── */
        case AMOTION_EVENT_ACTION_UP:
        case AMOTION_EVENT_ACTION_POINTER_UP:
            /* Mark the lifted pointer inactive */
            {
                int32_t id = AMotionEvent_getPointerId(event, pointerIdx);
                if (id < MAX_FINGERS) m_fingers[id].active = false;
                m_activeCount = std::max(0, m_activeCount - 1);
            }
            if (m_activeCount == 0) {
                if (fi[0] >= 0) handleOneFingerUp(m_fingers[fi[0]].x, m_fingers[fi[0]].y);
                bool consumed = (m_state != State::ONE_FINGER_DOWN);
                reset();
                return consumed;
            }
            if (m_activeCount == 1) {
                /* A finger lifted during a two-finger gesture – end gesture */
                handleTwoFingerUp();
                return true;
            }
            break;

        case AMOTION_EVENT_ACTION_CANCEL:
            reset();
            m_handler->onTouchGestureEnd();
            return true;

        default:
            break;
    }
    return false;
}

/* ── one-finger handlers ──────────────────────────────────────────────────── */

void GHOST_TouchGestureRecognizer::handleOneFingerDown(float x, float y)
{
    m_state     = State::ONE_FINGER_DOWN;
    m_startX[0] = m_lastX[0] = x;
    m_startY[0] = m_lastY[0] = y;
}

void GHOST_TouchGestureRecognizer::handleOneFingerMove(float x, float y)
{
    if (m_state == State::ONE_FINGER_DOWN) {
        float dx = x - m_startX[0], dy = y - m_startY[0];
        if (std::sqrt(dx * dx + dy * dy) > GHOST_TOUCH_PAN_THRESHOLD_PX) {
            m_state = State::ONE_FINGER_DRAG;
        }
        else return; /* still deciding */
    }

    if (m_state == State::ONE_FINGER_DRAG) {
        float dx = x - m_lastX[0], dy = y - m_lastY[0];
        m_handler->onTouchOrbit(dx, dy, x, y);
        m_lastX[0] = x;
        m_lastY[0] = y;
    }
}

void GHOST_TouchGestureRecognizer::handleOneFingerUp(float x, float y)
{
    if (m_state == State::ONE_FINGER_DOWN) {
        /* No significant movement → tap */
        m_handler->onTouchTap(x, y);
    }
    else if (m_state == State::ONE_FINGER_DRAG) {
        m_handler->onTouchGestureEnd();
    }
}

/* ── two-finger handlers ──────────────────────────────────────────────────── */

void GHOST_TouchGestureRecognizer::handleTwoFingerDown(
    float ax, float ay, float bx, float by)
{
    m_state     = State::TWO_FINGER_DOWN;
    m_startX[0] = m_lastX[0] = ax;
    m_startY[0] = m_lastY[0] = ay;
    m_startX[1] = m_lastX[1] = bx;
    m_startY[1] = m_lastY[1] = by;
    m_startSpan = m_lastSpan = span(ax, ay, bx, by);
    m_startAngle = m_lastAngle = angle(ax, ay, bx, by);
}

void GHOST_TouchGestureRecognizer::handleTwoFingerMove(
    float ax, float ay, float bx, float by)
{
    if (m_state == State::TWO_FINGER_DOWN) {
        classifyTwoFinger(ax, ay, bx, by);
        if (m_state == State::TWO_FINGER_DOWN) return; /* not yet decided */
    }

    float cx = centroidX(ax, bx);
    float cy = centroidY(ay, by);
    float dcx = cx - centroidX(m_lastX[0], m_lastX[1]);
    float dcy = cy - centroidY(m_lastY[0], m_lastY[1]);

    if (m_state == State::TWO_FINGER_PINCH) {
        float currentSpan = span(ax, ay, bx, by);
        float delta = currentSpan - m_lastSpan;        /* px */
        if (std::abs(delta) > 0.5f) {
            float zoomDelta = delta / GHOST_TOUCH_SCROLL_STEP;
            m_handler->onTouchZoom(zoomDelta, cx, cy);
            m_lastSpan = currentSpan;
        }
        /* Also report small pan component during pinch */
        if (std::abs(dcx) > 1.0f || std::abs(dcy) > 1.0f) {
            m_handler->onTouchPan(dcx, dcy, cx, cy);
        }
    }
    else if (m_state == State::TWO_FINGER_PAN) {
        if (std::abs(dcx) > 0.5f || std::abs(dcy) > 0.5f) {
            m_handler->onTouchPan(dcx, dcy, cx, cy);
            m_handler->onTouchScroll(dcx, dcy, cx, cy);
        }
    }

#if GHOST_TOUCH_TWIST_ENABLED
    float currentAngle = angle(ax, ay, bx, by);
    float angleDelta = currentAngle - m_lastAngle;
    /* Wrap to [-pi, pi] */
    while (angleDelta >  M_PI) angleDelta -= 2.0f * M_PI;
    while (angleDelta < -M_PI) angleDelta += 2.0f * M_PI;
    if (std::abs(angleDelta) > 0.01f) {
        m_handler->onTouchTwist(angleDelta, cx, cy);
        m_lastAngle = currentAngle;
    }
#endif

    m_lastX[0] = ax; m_lastY[0] = ay;
    m_lastX[1] = bx; m_lastY[1] = by;
}

void GHOST_TouchGestureRecognizer::handleTwoFingerUp()
{
    m_handler->onTouchGestureEnd();
}

void GHOST_TouchGestureRecognizer::classifyTwoFinger(
    float ax, float ay, float bx, float by)
{
    float currentSpan = span(ax, ay, bx, by);
    float spanDelta   = std::abs(currentSpan - m_startSpan);

    float acx = centroidX(ax, bx);
    float acy = centroidY(ay, by);
    float scx = centroidX(m_startX[0], m_startX[1]);
    float scy = centroidY(m_startY[0], m_startY[1]);
    float panDelta = span(acx, acy, scx, scy);

    if (spanDelta > GHOST_TOUCH_PINCH_THRESHOLD_PX) {
        m_state = State::TWO_FINGER_PINCH;
        m_lastSpan = currentSpan;
    }
    else if (panDelta > GHOST_TOUCH_PAN_THRESHOLD_PX) {
        m_state = State::TWO_FINGER_PAN;
    }
}

/* ── misc ─────────────────────────────────────────────────────────────────── */

void GHOST_TouchGestureRecognizer::onLongPressTimeout()
{
    if (m_state == State::ONE_FINGER_DOWN) {
        m_state = State::LONG_PRESS;
        m_handler->onTouchLongPress(m_lastX[0], m_lastY[0]);
    }
}

void GHOST_TouchGestureRecognizer::reset()
{
    m_state = State::IDLE;
    m_activeCount = 0;
    for (auto &f : m_fingers) f.active = false;
}

/*
 * ── CMakeLists.txt snippet ──────────────────────────────────────────────────
 *
 * In intern/ghost/CMakeLists.txt, find the Android source list and add:
 *
 *   intern/GHOST_TouchGestureAndroid.cpp
 *
 * Example (already existing lines shown for context):
 *
 *   if(WITH_GHOST_X11 OR WITH_GHOST_WAYLAND OR WITH_GHOST_ANDROID)
 *     ...
 *   endif()
 *
 *   # Android-specific
 *   if(WITH_GHOST_ANDROID)
 *     list(APPEND SRC
 *       intern/GHOST_SystemAndroid.cpp      # already there
 *       intern/GHOST_TouchGestureAndroid.cpp  # ADD THIS
 *     )
 *   endif()
 */
