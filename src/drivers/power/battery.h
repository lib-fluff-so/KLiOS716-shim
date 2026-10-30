#ifndef KLIOS716_BATTERY_H
#define KLIOS716_BATTERY_H

typedef unsigned short battery_u16;
typedef unsigned int battery_u32;

enum {
    BATTERY_VALID_VOLTAGE = 1u << 0,
    BATTERY_VALID_BAT_TEMP = 1u << 1,
    BATTERY_VALID_CHIP_TEMP = 1u << 2,
    BATTERY_VALID_VCDT = 1u << 3
};

struct battery_info {
    battery_u32 valid;
    battery_u16 pmic_id;
    battery_u32 battery_adc_raw;
    battery_u32 battery_temp_adc_raw;
    battery_u32 chip_temp_adc_raw;
    battery_u32 vcdt_adc_raw;
};

/* Read PMIC/AUXADC and SGM41516D status. No charger settings are changed. */
int battery_get_info(struct battery_info *out);

#endif
