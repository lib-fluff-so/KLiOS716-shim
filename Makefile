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
ifeq ($(EMULATOR_COMPAT),1)
LOADER_HANDOFF := 0
endif
CFLAGS += -DRAMOOPS_LOG=$(RAMOOPS_LOG) -DMMU_STAGE=$(MMU_STAGE) -DMMU_FAULT_TEST=$(MMU_FAULT_TEST) -DCPU_BRINGUP=$(CPU_BRINGUP) -DLOADER_HANDOFF=$(LOADER_HANDOFF)

ifeq ($(EMULATOR_COMPAT),1)
CFLAGS += -DEMULATOR_COMPAT=1
endif

# Build the deliberate UDF exception test into its own output directory with
# OUT=out-exception TEST_EXCEPTION_VECTORS=1 to keep it separate from normal images.
$(OUT)/main.o: CFLAGS += -DTEST_EXCEPTION_VECTORS=$(TEST_EXCEPTION_VECTORS)

all: $(OUT)/shim_b.img $(OUT)/shim_raw_b.img

$(OUT)/start.o: src/start.S | $(OUT)
	$(CC) $(CFLAGS) -c $< -o $@
$(OUT)/vectors.o: src/vectors.S | $(OUT)
	$(CC) $(CFLAGS) -c $< -o $@
$(OUT)/handoff.o: src/handoff.S | $(OUT)
	$(CC) $(CFLAGS) -c $< -o $@
$(OUT)/main.o: src/main.c src/font.h src/mm.h src/loader.h | $(OUT)
	$(CC) $(CFLAGS) -c src/main.c -o $@
$(OUT)/mm.o: src/mm.c src/mm.h | $(OUT)
	$(CC) $(CFLAGS) -c src/mm.c -o $@
$(OUT)/loader.o: src/loader.c src/loader.h | $(OUT)
	$(CC) $(CFLAGS) -c src/loader.c -o $@
$(OUT)/payload.o: src/handoff_test.S | $(OUT)
	$(CC) $(CFLAGS) -c $< -o $@
$(OUT)/payload.elf: $(OUT)/payload.o payload.ld
	$(LD) -nostdlib -T payload.ld -o $@ $(OUT)/payload.o
$(OUT)/payload.bin: $(OUT)/payload.elf
	$(OBJCOPY) -O binary $< $@
$(OUT)/ramdisk.cpio: $(OUT)/payload.bin tools/mkramdisk.py | $(OUT)
	python3 tools/mkramdisk.py $@ $(OUT)/payload.bin
$(OUT)/shim.elf: $(OUT)/start.o $(OUT)/vectors.o $(OUT)/handoff.o $(OUT)/main.o $(OUT)/mm.o $(OUT)/loader.o link.ld
	$(LD) -nostdlib -T link.ld -o $@ $(OUT)/start.o $(OUT)/vectors.o $(OUT)/handoff.o $(OUT)/main.o $(OUT)/mm.o $(OUT)/loader.o
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
emutest:
	$(MAKE) OUT=out-emu clean
	$(MAKE) OUT=out-emu EMULATOR_COMPAT=1 RAMOOPS_LOG=1 all
	python3 tools/emutest.py out-emu/Image fdt.dtb out-emu/emu_progress.png
hosttest:
	mkdir -p $(OUT)
	gcc -O1 -g -fsanitize=address,undefined -DMM_HOST -Wall -Wextra -o $(OUT)/hosttest tests/host_mm.c src/mm.c
	$(OUT)/hosttest fdt.dtb
loadertest: $(OUT)/payload.bin $(OUT)/ramdisk.cpio src/loader.c src/loader.h tests/host_loader.c
	gcc -O1 -g -fsanitize=address,undefined -Wall -Wextra -o $(OUT)/host_loader tests/host_loader.c src/loader.c
	$(OUT)/host_loader $(OUT)/ramdisk.cpio $(OUT)/payload.bin
	python3 tests/test_ramdisk.py $(OUT)/ramdisk.cpio $(OUT)/payload.bin
clean:
	rm -f $(OUT)/*
.PHONY: all clean emutest hosttest loadertest
