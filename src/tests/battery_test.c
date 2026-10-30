#include "../console.h"
#include "../drivers/power/battery.h"
#include "battery_test.h"

void battery_test(void)
{
    struct battery_info info = {0};
    int rc = battery_get_info(&info);

    logf("BATTERY TEST: rc=%d valid=%x pmic=%x",
         (unsigned long long)(rc < 0 ? -rc : rc),
         (unsigned long long)info.valid,
         (unsigned long long)info.pmic_id);

    if (info.valid & BATTERY_VALID_VOLTAGE)
        logf("BATTERY: BATADC raw=%x",
             (unsigned long long)info.battery_adc_raw);
    if (info.valid & BATTERY_VALID_BAT_TEMP)
        logf("BATTERY: BAT_TEMP raw=%x",
             (unsigned long long)info.battery_temp_adc_raw);
    if (info.valid & BATTERY_VALID_CHIP_TEMP)
        logf("BATTERY: CHIP_TEMP raw=%x",
             (unsigned long long)info.chip_temp_adc_raw);
    if (info.valid & BATTERY_VALID_VCDT)
        logf("BATTERY: VCDT raw=%x",
             (unsigned long long)info.vcdt_adc_raw);
}
