CC      = clang --target=aarch64-none-elf
CFLAGS  = -O2 -ffreestanding -fno-builtin -fno-pic -fno-pie -fno-stack-protector -fno-asynchronous-unwind-tables \
          -mgeneral-regs-only -mstrict-align -Wall -Wextra -Wno-unused-parameter
LD      = ld.lld
OBJCOPY = llvm-objcopy
OUT     ?= out
TEST_EXCEPTION_VECTORS ?= 0
RAMOOPS_LOG ?= 1
MMU_STAGE ?= 1
MMU_FAULT_TEST ?= 0
CPU_BRINGUP ?= 0
LOADER_HANDOFF ?= 1
XNU_PAYLOAD ?= 0
XNU_KERNEL ?=
XNU_RAMDISK ?=
XNU_CMDLINE ?= debug=0xc serial=3 cpus=1 keepsyms=1
D22AP_ADT ?= research/ipsw-x/d22ap.adt

ifeq ($(EMULATOR_COMPAT),1)
LOADER_HANDOFF := 0
ifneq ($(XNU_PAYLOAD),0)
$(error XNU_PAYLOAD=1 is a physical-KL7 path and cannot be combined with EMULATOR_COMPAT=1)
endif
endif
CFLAGS += -DRAMOOPS_LOG=$(RAMOOPS_LOG) -DMMU_STAGE=$(MMU_STAGE) -DMMU_FAULT_TEST=$(MMU_FAULT_TEST) \
          -DCPU_BRINGUP=$(CPU_BRINGUP) -DLOADER_HANDOFF=$(LOADER_HANDOFF) -DXNU_PAYLOAD=$(XNU_PAYLOAD)

ifeq ($(EMULATOR_COMPAT),1)
CFLAGS += -DEMULATOR_COMPAT=1
endif

# The v0.7.1 test-payload path remains the default. XNU mode is opt-in and uses
# the exact same shim image and CPIO transport, but stores the SWEETXNU container
# unwrapped as ios-kl7-xnu.bin.
ifeq ($(XNU_PAYLOAD),1)
$(OUT)/ramdisk.cpio: $(OUT)/xnu_payload.bin tools/mkramdisk.py | $(OUT)
	python3 tools/mkramdisk.py $@ $(OUT)/xnu_payload.bin --xnu

$(OUT)/kl7.adt: tools/mkadt_kl7.py tools/adt.py fdt.dtb | $(OUT)
	python3 tools/mkadt_kl7.py $@ --dtb fdt.dtb --product-adt "$(D22AP_ADT)"

ifneq ($(strip $(XNU_KERNEL)),)
$(OUT)/xnu_payload.bin: $(XNU_KERNEL) $(OUT)/kl7.adt tools/mkpayload_xnu.py tools/adt.py | $(OUT)
	python3 tools/mkpayload_xnu.py "$(XNU_KERNEL)" $(OUT)/kl7.adt $@ --cmdline "$(XNU_CMDLINE)" $(if $(strip $(XNU_RAMDISK)),--ramdisk "$(XNU_RAMDISK)")
else
$(OUT)/xnu_payload.bin: | $(OUT)
	@echo "error: XNU_PAYLOAD=1 requires XNU_KERNEL=/path/to/arm64 Mach-O kernelcache" >&2
	@exit 2
endif
else
$(OUT)/ramdisk.cpio: $(OUT)/payload.bin tools/mkramdisk.py | $(OUT)
	python3 tools/mkramdisk.py $@ $(OUT)/payload.bin

$(OUT)/payload.o: src/handoff_test.S | $(OUT)
	$(CC) $(CFLAGS) -c $< -o $@
$(OUT)/payload.elf: $(OUT)/payload.o payload.ld
	$(LD) -nostdlib -T payload.ld -o $@ $(OUT)/payload.o
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
$(OUT)/main.o: src/main.c src/font.h src/mm.h src/loader.h src/xnu_payload.h | $(OUT)
	$(CC) $(CFLAGS) -c src/main.c -o $@
$(OUT)/mm.o: src/mm.c src/mm.h | $(OUT)
	$(CC) $(CFLAGS) -c src/mm.c -o $@
$(OUT)/loader.o: src/loader.c src/loader.h | $(OUT)
	$(CC) $(CFLAGS) -c src/loader.c -o $@
$(OUT)/xnu_payload.o: src/xnu_payload.c src/xnu_payload.h src/loader.h | $(OUT)
	$(CC) $(CFLAGS) -c src/xnu_payload.c -o $@
$(OUT)/shim.elf: $(OUT)/start.o $(OUT)/vectors.o $(OUT)/handoff.o $(OUT)/main.o $(OUT)/mm.o $(OUT)/loader.o $(OUT)/xnu_payload.o link.ld
	$(LD) -nostdlib -T link.ld -o $@ $(OUT)/start.o $(OUT)/vectors.o $(OUT)/handoff.o $(OUT)/main.o $(OUT)/mm.o $(OUT)/loader.o $(OUT)/xnu_payload.o
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
	gcc -O1 -g -fsanitize=address,undefined -DMM_HOST -Wall -Wextra -o $(OUT)/hosttest tests/host_mm.c src/mm.c
	$(OUT)/hosttest fdt.dtb

# Always test the legacy mode in its own output directory so XNU-mode builds
# do not accidentally replace the known-good test CPIO in ./out.
loadertest:
	$(MAKE) OUT=out-test XNU_PAYLOAD=0 out-test/ramdisk.cpio
	gcc -O1 -g -fsanitize=address,undefined -Wall -Wextra -o out-test/host_loader tests/host_loader.c src/loader.c
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

emutest:
	$(MAKE) OUT=out-emu clean
	$(MAKE) OUT=out-emu EMULATOR_COMPAT=1 RAMOOPS_LOG=1 all
	python3 tools/emutest.py out-emu/Image fdt.dtb out-emu/emu_progress.png

clean:
	@if [ -d "$(OUT)" ]; then find "$(OUT)" -mindepth 1 -maxdepth 1 -exec rm -rf -- {} +; fi
.PHONY: all clean emutest hosttest loadertest xnutest
