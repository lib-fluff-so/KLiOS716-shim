#ifndef KLIOS716_BOOT_MENU_H
#define KLIOS716_BOOT_MENU_H

enum boot_choice {
    BOOT_CHOICE_ANDROID = 1,
    BOOT_CHOICE_IOS = 2
};

/* Display the boot selector and wait for a fresh FTS3519 tap. */
int boot_menu_select(enum boot_choice *choice);

#endif
