#include <stdio.h>
#include <stdlib.h>
#include "../aoa_reenumeration.h"

static void require(int condition, const char* message)
{
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        exit(1);
    }
}

int main()
{
    require(aoa_switch_action(1, 0) == AOA_SWITCH_WAIT,
            "confirmed protocol waits even without a START outcome");
    require(aoa_switch_action(0, 1) == AOA_SWITCH_UNSUPPORTED, "unsupported protocol remains ignored");
    require(aoa_switch_action(0, 0) == AOA_SWITCH_RETRY, "transient pre-protocol failure retries");
    require(aoa_switch_action(1, 1) == AOA_SWITCH_WAIT, "confirmed protocol takes precedence");
    const int normal = 1;
    const int accessory = 2;
    const long long deadline = 5000;
    require(aoa_reenumeration_action(0, accessory, accessory, 500, deadline) == AOA_REENUMERATION_READY,
            "accessory appearing before deadline succeeds");
    require(aoa_reenumeration_action(0, normal, accessory, 500, deadline) == AOA_REENUMERATION_WAIT,
            "old normal-mode device keeps waiting");
    require(aoa_reenumeration_action(0, 0, accessory, 4999, deadline) == AOA_REENUMERATION_WAIT,
            "temporary empty inventory keeps waiting");
    require(aoa_reenumeration_action(0, 0, accessory, deadline, deadline) == AOA_REENUMERATION_FAILED,
            "deadline expires without reset loop-side effects");
    require(aoa_reenumeration_action(0, normal, accessory, deadline + 1, deadline) == AOA_REENUMERATION_FAILED,
            "late normal device is not accepted");
    require(aoa_reenumeration_action(1, accessory, accessory, 100, deadline) == AOA_REENUMERATION_STOPPED,
            "service stop wins over inventory");
    require(aoa_reenumeration_action(1, 0, accessory, deadline, deadline) == AOA_REENUMERATION_STOPPED,
            "service stop wins at deadline");
    puts("AOA_REENUMERATION_POLICY_OK");
    return 0;
}
