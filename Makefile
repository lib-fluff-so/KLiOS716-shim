CC      = clang --target=aarch64-none-elf
CFLAGS  = -O2 -ffreestanding -fno-builtin -fno-pic -fno-pie -fno-stack-protector -fno-asynchronous-unwind-tables \
          -mgeneral-regs-only -mstrict-align -Wall -Wextra -Wno-unused-parameter
LD      = ld.lld
OBJCOPY = llvm-objcopy
OUT     = out
TEST_EXCEPTION_VECTORS ?= 0

# Set TEST_EXCEPTION_VECTORS=1 for a deliberate UDF exception test. Always
# rebuild after changing this option: make clean && make TEST_EXCEPTION_VECTORS=1.
$(OUT)/main.o: CFLAGS += -DTEST_EXCEPTION_VECTORS=$(TEST_EXCEPTION_VECTORS)

all: $(OUT)/shim_b.img $(OUT)/shim_raw_b.img

$(OUT)/start.o: src/start.S | $(OUT)
	$(CC) $(CFLAGS) -c $< -o $@
$(OUT)/vectors.o: src/vectors.S | $(OUT)
	$(CC) $(CFLAGS) -c $< -o $@
$(OUT)/main.o: src/main.c src/font.h | $(OUT)
	$(CC) $(CFLAGS) -c src/main.c -o $@
$(OUT)/shim.elf: $(OUT)/start.o $(OUT)/vectors.o $(OUT)/main.o link.ld
	$(LD) -nostdlib -T link.ld -o $@ $(OUT)/start.o $(OUT)/vectors.o $(OUT)/main.o
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
clean:
	rm -f $(OUT)/*
.PHONY: all clean
