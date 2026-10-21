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
CPU_BRINGUP ?= 1
CFLAGS += -DRAMOOPS_LOG=$(RAMOOPS_LOG) -DMMU_STAGE=$(MMU_STAGE) -DMMU_FAULT_TEST=$(MMU_FAULT_TEST) -DCPU_BRINGUP=$(CPU_BRINGUP)

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
$(OUT)/main.o: src/main.c src/font.h src/mm.h | $(OUT)
	$(CC) $(CFLAGS) -c src/main.c -o $@
$(OUT)/mm.o: src/mm.c src/mm.h | $(OUT)
	$(CC) $(CFLAGS) -c src/mm.c -o $@
$(OUT)/shim.elf: $(OUT)/start.o $(OUT)/vectors.o $(OUT)/main.o $(OUT)/mm.o link.ld
	$(LD) -nostdlib -T link.ld -o $@ $(OUT)/start.o $(OUT)/vectors.o $(OUT)/main.o $(OUT)/mm.o
$(OUT)/Image: $(OUT)/shim.elf
	$(OBJCOPY) -O binary $< $@
$(OUT)/Image.gz: $(OUT)/Image
	gzip -9 -n -c $< > $@
$(OUT)/ramdisk.cpio.gz: | $(OUT)
	python3 tools/mkramdisk.py $@
$(OUT)/shim_b.img: $(OUT)/Image.gz $(OUT)/ramdisk.cpio.gz
	python3 tools/mkboot.py $(OUT)/Image.gz $(OUT)/ramdisk.cpio.gz $@
$(OUT)/shim_raw_b.img: $(OUT)/Image $(OUT)/ramdisk.cpio.gz
	python3 tools/mkboot.py $(OUT)/Image $(OUT)/ramdisk.cpio.gz $@
$(OUT):
	mkdir -p $(OUT)
emutest:
	$(MAKE) OUT=out-emu clean
	$(MAKE) OUT=out-emu EMULATOR_COMPAT=1 RAMOOPS_LOG=1 all
	python3 tools/emutest.py out-emu/Image fdt.dtb
hosttest:
	gcc -O1 -g -fsanitize=address,undefined -DMM_HOST -Wall -Wextra -o out/hosttest tests/host_mm.c src/mm.c
	out/hosttest fdt.dtb
clean:
	rm -f $(OUT)/*
.PHONY: all clean emutest hosttest
