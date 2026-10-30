#include "../console.h"
#include "../boot/boot_menu.h"
#include "boot_menu_test.h"

void boot_menu_test(void)
{
    enum boot_choice choice = 0;
    int rc = boot_menu_select(&choice);
    if (rc) {
        logf("BOOT MENU TEST: failed rc=%d",
             (unsigned long)(rc < 0 ? -rc : rc));
        return;
    }
    logf("BOOT MENU TEST: selected=%s",
         choice == BOOT_CHOICE_ANDROID ? "Android" : "iOS");
}
