// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Adapted from NBT-AAD: Copyright (c) 2026 nanaisu
// https://github.com/Sq00ky/Supra-iDrive6-Android-Auto
#ifndef NBT_CAPTURE_KEYFRAME_ASK_HPP
#define NBT_CAPTURE_KEYFRAME_ASK_HPP

class KeyframeAsk {
public:
    enum Action { none, focus_off, focus_on };
    KeyframeAsk() : pending_(false), asked_(false), off_ms_(0) {}

    Action step(long long now, bool wanted, bool can_ask, bool can_restore) {
        if (now < 0 && pending_ && can_restore) {
            pending_ = false;
            return focus_on;
        }
        if (now < 0)
            return none;
        if (pending_) {
            if (!can_restore || now - off_ms_ < 500)
                return none;
            pending_ = false;
            return focus_on;
        }
        if (!wanted || !can_ask || (asked_ && now - off_ms_ < 10000))
            return none;
        asked_ = pending_ = true;
        off_ms_ = now;
        return focus_off;
    }

    void restored() { pending_ = false; }

private:
    bool pending_, asked_;
    long long off_ms_;
};

#endif
