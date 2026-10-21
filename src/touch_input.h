#pragma once
#include <stdint.h>

void touchBegin(uint8_t rotation, int width, int height);
bool touchRead(int& x, int& y);

// Relative gestures need no tap targets or precise pixel calibration.
class GainSwipe {
public:
    int update(bool pressed, int x, int y, uint32_t now) {
        if (!pressed) {
            // Tolerate brief contact loss on a resistive panel.
            if (active_ && (uint32_t)(now - lastContact_) > 80) active_ = false;
            return 0;
        }
        lastContact_ = now;
        if (!active_) {
            active_ = true;
            horizontal_ = blocked_ = false;
            startX_ = stepX_ = x;
            startY_ = y;
            return 0;
        }
        if (blocked_) return 0;
        if (!horizontal_) {
            const int dx = absolute(x - startX_), dy = absolute(y - startY_);
            if (dy >= 20 && dy > dx) { blocked_ = true; return 0; }
            if (dx < 20 || dx < 2 * dy) return 0;
            horizontal_ = true;
        }
        const int steps = (x - stepX_) / 16;
        stepX_ += steps * 16;
        return steps * 2;  // Two gain indices per ~16 pixels of travel.
    }

private:
    static int absolute(int n) { return n < 0 ? -n : n; }
    bool active_ = false, horizontal_ = false, blocked_ = false;
    int startX_ = 0, startY_ = 0, stepX_ = 0;
    uint32_t lastContact_ = 0;
};
