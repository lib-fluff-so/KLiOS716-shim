#!/bin/sh
# Вернуть слот A (если слот B завис/не откатился сам).
fastboot set_active a
fastboot reboot
