#ifndef NBT_AOA_REENUMERATION_H
#define NBT_AOA_REENUMERATION_H

enum AoaReenumerationAction {
    AOA_REENUMERATION_WAIT = 0,
    AOA_REENUMERATION_READY = 1,
    AOA_REENUMERATION_FAILED = 2,
    AOA_REENUMERATION_STOPPED = 3
};

static inline int aoa_reenumeration_action(int stopping, int phone_type, int accessory_type,
                                           long long now_ms, long long deadline_ms)
{
    if (stopping)
        return AOA_REENUMERATION_STOPPED;
    if (phone_type == accessory_type)
        return AOA_REENUMERATION_READY;
    if (now_ms >= deadline_ms)
        return AOA_REENUMERATION_FAILED;
    return AOA_REENUMERATION_WAIT;
}

#endif
