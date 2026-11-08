#include <3ds.h>

#include "battery.h"

static bool ptmu_ok, mcu_ok;

void battery_init(void)
{
    ptmu_ok = R_SUCCEEDED(ptmuInit());
    mcu_ok = R_SUCCEEDED(mcuHwcInit());
}

void battery_exit(void)
{
    if (mcu_ok)
        mcuHwcExit();
    if (ptmu_ok)
        ptmuExit();
}

bool battery_read(int *percent, bool *charging)
{
    u8 level = 0;
    if (mcu_ok && R_SUCCEEDED(MCUHWC_GetBatteryLevel(&level))) {
        *percent = level > 100 ? 100 : level;
    } else if (ptmu_ok && R_SUCCEEDED(PTMU_GetBatteryLevel(&level))) {
        *percent = (level > 5 ? 5 : level) * 20;
    } else {
        return false;
    }

    u8 state = 0;
    *charging = ptmu_ok && R_SUCCEEDED(PTMU_GetBatteryChargeState(&state)) && state;
    return true;
}
