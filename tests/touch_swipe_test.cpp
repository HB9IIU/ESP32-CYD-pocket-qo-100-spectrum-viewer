#include "touch_input.h"
#include <assert.h>
#include <cstdio>

int main() {
    GainSwipe swipe;
    // A tap and finger jitter must not change gain.
    assert(swipe.update(true, 100, 100, 100) == 0);
    assert(swipe.update(true, 103, 102, 115) == 0);
    assert(swipe.update(true, 99, 101, 130) == 0);
    assert(swipe.update(false, 0, 0, 220) == 0);
    // Rightward travel increases gain; stationary contact does not repeat it.
    assert(swipe.update(true, 100, 100, 300) == 0);
    assert(swipe.update(true, 132, 102, 315) == 4);
    assert(swipe.update(true, 132, 102, 330) == 0);
    assert(swipe.update(true, 164, 102, 345) == 4);
    // Reverse direction within the same gesture.
    assert(swipe.update(true, 132, 102, 360) == -4);
    // A brief resistive-contact dropout should not break the gesture.
    assert(swipe.update(false, 0, 0, 375) == 0);
    assert(swipe.update(true, 116, 102, 390) == -2);
    assert(swipe.update(false, 0, 0, 480) == 0);
    // A vertical swipe must remain ignored, even if it later drifts sideways.
    assert(swipe.update(true, 100, 100, 500) == 0);
    assert(swipe.update(true, 102, 132, 515) == 0);
    assert(swipe.update(true, 164, 140, 530) == 0);
    assert(swipe.update(false, 0, 0, 620) == 0);
    // A fresh left swipe decreases gain.
    assert(swipe.update(true, 200, 100, 700) == 0);
    assert(swipe.update(true, 168, 100, 715) == -4);
    std::puts("PASS: touch jitter, horizontal swipes, reversal, release, dropouts, vertical rejection");
}
