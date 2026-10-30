.DEFAULT_GOAL := all

CC      = clang --target=aarch64-none-elf
CFLAGS  = -O2 -ffreestanding -fno-builtin -fno-pic -fno-pie -fno-stack-protector \
          -fno-asynchronous-unwind-tables -ffunction-sections -fdata-sections \
          -mgeneral-regs-only -mstrict-align -Wall -Wextra -Werror -Wno-unused-parameter
LD      = ld.lld
OBJCOPY = llvm-objcopy
OUT     ?= out
RAMOOPS_LOG ?= 1
BATTERY_TEST ?= 0
TOUCH_TEST ?= 0
UFS_TEST ?= 0
MENU_TEST ?= 0
ANDROID_BOOT_TEST ?= 0
UFS_HS_GEAR ?= 3
XNU_PAYLOAD ?= 0
PSCI_RESET_TIMER ?= 0
XNU_KERNEL ?=
XNU_RAMDISK ?=
# The KL7 panel shows the iOS UI now. sweetfb_noconsole=1 still draws early
# kernel text until the first iOS frame, then stops drawing it over the GUI;
# text always goes to the persistent log, and panics are drawn.
# Remove sweetfb_noconsole=1 from XNU_CMDLINE to keep text over the GUI.
# sweetvib=300 pulses the KL7 actuator once at boot. iOS has no haptic driver
# for this linear actuator yet, so keep this pulse as the only feedback.
XNU_CMDLINE ?= debug=0xc serial=3 cpus=8 keepsyms=1 rd=ufsroot0 ufsroot=iosroot ufsrw=1 rootrw=1 unentitled_ios_sim_launch=1 ignition_level=1 launchd_unsecure_cache=1 wdt_test=no_user_checkin wdt=-1 sweetfb_noconsole=1 sweetvib=300
D22AP_ADT ?= research/ipsw-x/d22ap.adt

CFLAGS += -DRAMOOPS_LOG=$(RAMOOPS_LOG) -DXNU_PAYLOAD=$(XNU_PAYLOAD) -DPSCI_RESET_TIMER=$(PSCI_RESET_TIMER) -DBATTERY_TEST=$(BATTERY_TEST) -DTOUCH_TEST=$(TOUCH_TEST) -DUFS_TEST=$(UFS_TEST) -DUFS_HS_GEAR=$(UFS_HS_GEAR) -DMENU_TEST=$(MENU_TEST) -DANDROID_BOOT_TEST=$(ANDROID_BOOT_TEST)
ifdef WDT_DISABLE_AT_HANDOFF
CFLAGS += -DWDT_DISABLE_AT_HANDOFF=$(WDT_DISABLE_AT_HANDOFF)
endif

ifeq ($(XNU_PAYLOAD),1)
$(OUT)/ramdisk.cpio: $(OUT)/xnu_payload.bin tools/mkramdisk.py | $(OUT)
	python3 tools/mkramdisk.py $@ $(OUT)/xnu_payload.bin --xnu

$(OUT)/kl7.adt: tools/mkadt_kl7.py tools/adt.py fdt.dtb | $(OUT)
	python3 tools/mkadt_kl7.py $@ --dtb fdt.dtb --product-adt "$(D22AP_ADT)"

ifeq ($(strip $(XNU_KERNEL)),)
$(OUT)/xnu_payload.bin: | $(OUT)
	@echo "error: XNU_PAYLOAD=1 requires XNU_KERNEL=/path/to/arm64 Mach-O kernelcache" >&2
	@exit 2
else
$(OUT)/xnu_payload.bin: $(XNU_KERNEL) $(OUT)/kl7.adt tools/mkpayload_xnu.py tools/adt.py | $(OUT)
	python3 tools/mkpayload_xnu.py "$(XNU_KERNEL)" $(OUT)/kl7.adt $@ --cmdline "$(XNU_CMDLINE)" $(if $(strip $(XNU_RAMDISK)),--ramdisk "$(XNU_RAMDISK)")
endif
else
$(OUT)/ramdisk.cpio: $(OUT)/payload.bin tools/mkramdisk.py | $(OUT)
	python3 tools/mkramdisk.py $@ $(OUT)/payload.bin

$(OUT)/payload.o: src/handoff_test.S | $(OUT)
	$(CC) $(CFLAGS) -c $< -o $@
$(OUT)/payload.elf: $(OUT)/payload.o payload.ld
	$(LD) --gc-sections -nostdlib -T payload.ld -o $@ $(OUT)/payload.o
$(OUT)/payload.bin: $(OUT)/payload.elf
	$(OBJCOPY) -O binary $< $@
endif

all: $(OUT)/shim_b.img $(OUT)/shim_raw_b.img

$(OUT)/start.o: src/start.S | $(OUT)
	$(CC) $(CFLAGS) -c $< -o $@
$(OUT)/vectors.o: src/vectors.S | $(OUT)
	$(CC) $(CFLAGS) -c $< -o $@
$(OUT)/handoff.o: src/handoff.S | $(OUT)
	$(CC) $(CFLAGS) -c $< -o $@
$(OUT)/main.o: src/main.c src/console.h src/memmap.h src/loader.h src/xnu_payload.h src/tests/battery_test.h src/tests/touch_test.h src/tests/ufs_test.h src/tests/boot_menu_test.h src/tests/android_boot_test.h | $(OUT)
	$(CC) $(CFLAGS) -c $< -o $@
$(OUT)/console.o: src/console.c src/console.h src/font.h | $(OUT)
	$(CC) $(CFLAGS) -c $< -o $@
$(OUT)/memmap.o: src/memmap.c src/memmap.h | $(OUT)
	$(CC) $(CFLAGS) -c $< -o $@
$(OUT)/loader.o: src/loader.c src/loader.h | $(OUT)
	$(CC) $(CFLAGS) -c $< -o $@
$(OUT)/xnu_payload.o: src/xnu_payload.c src/xnu_payload.h src/loader.h | $(OUT)
	$(CC) $(CFLAGS) -c $< -o $@

$(OUT)/battery.o: src/drivers/power/battery.c src/drivers/power/battery.h src/console.h | $(OUT)
	$(CC) $(CFLAGS) -c $< -o $@
$(OUT)/battery_test.o: src/tests/battery_test.c src/tests/battery_test.h src/drivers/power/battery.h src/console.h | $(OUT)
	$(CC) $(CFLAGS) -c $< -o $@

$(OUT)/timer.o: src/core/timer.c src/core/timer.h | $(OUT)
	$(CC) $(CFLAGS) -c $< -o $@
$(OUT)/fts3519.o: src/drivers/input/fts3519.c src/drivers/input/fts3519.h src/core/timer.h src/console.h | $(OUT)
	$(CC) $(CFLAGS) -c $< -o $@
$(OUT)/touch_test.o: src/tests/touch_test.c src/tests/touch_test.h src/drivers/input/fts3519.h src/core/timer.h src/console.h | $(OUT)
	$(CC) $(CFLAGS) -c $< -o $@

$(OUT)/mtk_ufs.o: src/drivers/storage/mtk_ufs.c src/drivers/storage/mtk_ufs.h src/core/timer.h src/console.h | $(OUT)
	$(CC) $(CFLAGS) -c $< -o $@
$(OUT)/gpt.o: src/drivers/storage/gpt.c src/drivers/storage/gpt.h src/drivers/storage/mtk_ufs.h | $(OUT)
	$(CC) $(CFLAGS) -c $< -o $@
$(OUT)/ufs_test.o: src/tests/ufs_test.c src/tests/ufs_test.h src/drivers/storage/mtk_ufs.h src/drivers/storage/gpt.h src/console.h | $(OUT)
	$(CC) $(CFLAGS) -c $< -o $@

$(OUT)/boot_menu.o: src/boot/boot_menu.c src/boot/boot_menu.h src/drivers/input/fts3519.h src/core/timer.h src/console.h | $(OUT)
	$(CC) $(CFLAGS) -c $< -o $@
$(OUT)/boot_menu_test.o: src/tests/boot_menu_test.c src/tests/boot_menu_test.h src/boot/boot_menu.h src/console.h | $(OUT)
	$(CC) $(CFLAGS) -c $< -o $@

$(OUT)/android_bootimg.o: src/boot/android_bootimg.c src/boot/android_bootimg.h | $(OUT)
	$(CC) $(CFLAGS) -c $< -o $@
$(OUT)/android_boot_test.o: src/tests/android_boot_test.c src/tests/android_boot_test.h src/boot/android_bootimg.h src/drivers/storage/gpt.h src/drivers/storage/mtk_ufs.h src/console.h | $(OUT)
	$(CC) $(CFLAGS) -c $< -o $@
$(OUT)/shim.elf: $(OUT)/start.o $(OUT)/vectors.o $(OUT)/handoff.o $(OUT)/main.o $(OUT)/console.o $(OUT)/memmap.o $(OUT)/loader.o $(OUT)/xnu_payload.o $(OUT)/battery.o $(OUT)/battery_test.o $(OUT)/timer.o $(OUT)/fts3519.o $(OUT)/touch_test.o $(OUT)/mtk_ufs.o $(OUT)/gpt.o $(OUT)/ufs_test.o $(OUT)/boot_menu.o $(OUT)/boot_menu_test.o $(OUT)/android_bootimg.o $(OUT)/android_boot_test.o link.ld
	$(LD) --gc-sections -nostdlib -T link.ld -o $@ $(filter %.o,$^)
$(OUT)/Image: $(OUT)/shim.elf
	$(OBJCOPY) -O binary $< $@
$(OUT)/Image.gz: $(OUT)/Image
	gzip -9 -n -c $< > $@
$(OUT)/shim_b.img: $(OUT)/Image.gz $(OUT)/ramdisk.cpio
	python3 tools/mkboot.py $(OUT)/Image.gz $(OUT)/ramdisk.cpio $@
$(OUT)/shim_raw_b.img: $(OUT)/Image $(OUT)/ramdisk.cpio
	python3 tools/mkboot.py $(OUT)/Image $(OUT)/ramdisk.cpio $@
$(OUT):
	mkdir -p $(OUT)

hosttest:
	mkdir -p $(OUT)
	gcc -O1 -g -fsanitize=address,undefined -Wall -Wextra -Werror -o $(OUT)/memmaptest tests/host_memmap.c src/memmap.c
	$(OUT)/memmaptest fdt.dtb

loadertest:
	$(MAKE) OUT=out-test XNU_PAYLOAD=0 out-test/ramdisk.cpio
	gcc -O1 -g -fsanitize=address,undefined -Wall -Wextra -Werror -o out-test/host_loader tests/host_loader.c src/loader.c
	out-test/host_loader out-test/ramdisk.cpio out-test/payload.bin
	python3 tests/test_ramdisk.py out-test/ramdisk.cpio out-test/payload.bin

xnutest:
	mkdir -p $(OUT)
	gcc -O1 -g -fsanitize=address,undefined -std=c11 -Wall -Wextra -Werror -o $(OUT)/host_xnu_payload tests/host_xnu_payload.c src/xnu_payload.c
	$(OUT)/host_xnu_payload
	python3 tests/test_xnu_packaging.py $(OUT)/xnu-fixture
	$(OUT)/host_xnu_payload $(OUT)/xnu-fixture/xnu_payload.bin
	$(OUT)/host_xnu_payload $(OUT)/xnu-fixture/xnu_payload_ramdisk.bin
	python3 tests/test_xnu_adt.py

sizes:
	@python3 -c 'import os; from pathlib import Path; d=Path("$(OUT)");\
files=["Image","Image.gz","ramdisk.cpio","shim_b.img","shim_raw_b.img"];\
[(print(f"{f:20} {os.path.getsize(d/f):>12,} bytes") if (d/f).exists() else None) for f in files]'

clean:
	@if [ -d "$(OUT)" ]; then find "$(OUT)" -mindepth 1 -maxdepth 1 -exec rm -rf -- {} +; fi
.PHONY: all clean hosttest loadertest xnutest sizes
