/* ios-kl7 diagnostic shim: framebuffer + register/DTB diagnostics + reset.
 * v0.5.2 adds a persistent_ram-compatible ramoops console log.
 * v0.6.2 adds the DTB memory map, a 16 KiB-granule identity map, MMU enable
 * and PSCI CPU_ON bring-up of all secondary cores (see mm.c, start.S).
 * Runs with MMU off: no libc and no SIMD; use aligned accesses only. */
typedef unsigned char u8;
typedef unsigned int u32;
typedef unsigned long u64;
typedef __builtin_va_list va_list;
#define va_start(a, b) __builtin_va_start(a, b)
#define va_end(a)      __builtin_va_end(a)
#define va_arg(a, t)   __builtin_va_arg(a, t)

#include "font.h"
#include "mm.h"

#ifndef TEST_EXCEPTION_VECTORS
#define TEST_EXCEPTION_VECTORS 0
#endif
#ifndef RAMOOPS_LOG
#define RAMOOPS_LOG 1
#endif
#ifndef MMU_STAGE
#define MMU_STAGE 1      /* 0: skip the MMU + secondary-CPU stage (bisecting) */
#endif
#ifndef MMU_FAULT_TEST
#define MMU_FAULT_TEST 0 /* 1: after the MMU is on, read a no-map address on purpose */
#endif
#ifndef CPU_BRINGUP
#define CPU_BRINGUP 1    /* 0: do not PSCI CPU_ON the secondary cores */
#endif

#define FB_BASE   0xFDFFF000UL
#define FB_W      1080
#define FB_H      2436
#define STRIDE_A  4320          /* 1080 * 4 */
#define STRIDE_B  4352          /* 1088 * 4 (what Android's composer uses) */
#define SCALE     3
#define LINE_H    (16 * SCALE)
#define WDT_BASE  0x10007000UL
#define BG        0xFF101010u

/* Confirmed from KL7's live Linux 5.10 ramoops mapping via /proc/iomem:
 * mem_address=0x48090000, mem_size=0xe0000, console_size=0x40000,
 * record_size=0x1000, ftrace_size=0x1000, pmsg_size=0x40000, ecc=0.
 * The dump ring occupies 0x48090000..0x480eefff, so console starts at
 * 0x480ef000. This build intentionally targets that exact firmware layout. */
#define RAMOOPS_BASE         0x48090000UL
#define RAMOOPS_SIZE         0x000E0000UL
#define RAMOOPS_CONSOLE_BASE 0x480EF000UL
#define RAMOOPS_CONSOLE_SIZE 0x00040000UL
#define PERSISTENT_RAM_SIG   0x43474244u /* "DBGC" in little-endian memory */

void *memset(void *d, int c, unsigned long n) { u8 *p = d; while (n--) *p++ = (u8)c; return d; }
void *memcpy(void *d, const void *s, unsigned long n) { u8 *p = d; const u8 *q = s; while (n--) *p++ = *q++; return d; }

#define RD(reg) ({ u64 v_; __asm__ volatile("mrs %0, " #reg : "=r"(v_)); v_; })

static void put_px(u64 base_off, u32 stride, int x, int y, u32 col)
{
    volatile u32 *p = (volatile u32 *)(FB_BASE + base_off + (u64)y * stride + (u64)x * 4);
    *p = col;
}

static void draw_char(u64 base_off, u32 stride, int x, int y, char c, u32 fg)
{
    if (c < 32 || c > 126) c = '?';
    const u8 *g = font8x16[c - 32];
    for (int row = 0; row < 16; row++)
        for (int col = 0; col < 8; col++) {
            u32 v = (g[row] & (0x80 >> col)) ? fg : BG;
            for (int sy = 0; sy < SCALE; sy++)
                for (int sx = 0; sx < SCALE; sx++)
                    put_px(base_off, stride, x + col * SCALE + sx, y + row * SCALE + sy, v);
        }
}

/* text buffer: lines rendered at the end so both strides get identical output */
#define MAX_LINES 25
#define MAX_COLS  44
static char lines[MAX_LINES][64];
static int nlines;

/* Linux 5.10 struct persistent_ram_buffer: 3 x 32-bit words then data[];
 * ramoops console has sig argument 0, hence PERSISTENT_RAM_SIG ^ 0.
 * start is the next write position; size is the valid byte count. */
#if RAMOOPS_LOG
struct persistent_ram_buffer {
    volatile u32 sig;
    volatile u32 start;
    volatile u32 size;
    volatile u8 data[];
};
_Static_assert(sizeof(struct persistent_ram_buffer) == 12,
               "Linux persistent_ram header must be 12 bytes");
static u32 ramoops_write_pos;
static u32 ramoops_valid_size;
static volatile u32 ramoops_ready;
#define RAMOOPS_DATA_CAPACITY (RAMOOPS_CONSOLE_SIZE - sizeof(struct persistent_ram_buffer))

static void ramoops_barrier(void)
{
    __asm__ volatile("dsb sy" ::: "memory");
}

static void ramoops_append(const char *text, u32 len)
{
    if (!ramoops_ready || !text || !len) return;
    volatile struct persistent_ram_buffer *b =
        (volatile struct persistent_ram_buffer *)RAMOOPS_CONSOLE_BASE;
    u32 pos = ramoops_write_pos;
    u32 valid = ramoops_valid_size;

    for (u32 i = 0; i < len; i++) {
        b->data[pos] = (u8)text[i];
        pos++;
        if (pos == RAMOOPS_DATA_CAPACITY) pos = 0;
        if (valid < RAMOOPS_DATA_CAPACITY) valid++;
    }

    /* Publish the data and size before the new write pointer. If a reset
     * lands between header stores during a growing (not-yet-full) log,
     * the old pointer remains in-range rather than making the buffer invalid. */
    ramoops_barrier();
    b->size = valid;
    ramoops_barrier();
    b->start = pos;
    ramoops_barrier();

    ramoops_write_pos = pos;
    ramoops_valid_size = valid;
}

static void ramoops_append_cstr(const char *s)
{
    u32 n = 0;
    while (s[n]) n++;
    ramoops_append(s, n);
}

static void ramoops_init(void)
{
    volatile struct persistent_ram_buffer *b =
        (volatile struct persistent_ram_buffer *)RAMOOPS_CONSOLE_BASE;

    /* Mark the old record invalid while preparing the new log. */
    b->sig = 0;
    ramoops_barrier();
    b->start = 0;
    b->size = 0;
    ramoops_write_pos = 0;
    ramoops_valid_size = 0;
    ramoops_barrier();

    /* The exact console signature is PERSISTENT_RAM_SIG ^ 0 (sig=0 in
     * ramoops_init_prz for "console"). Publish it only after the header is ready. */
    b->sig = PERSISTENT_RAM_SIG;
    ramoops_barrier();
    ramoops_ready = 1;
    ramoops_append_cstr("ios-kl7 v0.6.2: shim entered\n");
}

static void ramoops_record_lines(void)
{
    for (int i = 0; i < nlines; i++) {
        u32 n = 0;
        while (lines[i][n]) n++;
        ramoops_append(lines[i], n);
        ramoops_append_cstr("\n");
    }
}
#else
static volatile u32 ramoops_ready;
static void ramoops_init(void) { ramoops_ready = 0; }
static void ramoops_record_lines(void) {}
#endif

static void fmt_u64(char **o, u64 v, int base, int minw)
{
    char tmp[24]; int n = 0;
    do { int d = v % base; tmp[n++] = d < 10 ? '0' + d : 'a' + d - 10; v /= base; } while (v);
    while (n < minw) tmp[n++] = '0';
    while (n) *(*o)++ = tmp[--n];
}

/* format into o (at most cap chars + NUL); %x hex, %w 8-digit hex, %d decimal, %s string */
static int vfmt(char *o, int cap, const char *f, va_list ap)
{
    char *start = o;
    for (; *f && (o - start) < cap; f++) {
        if (*f != '%') { *o++ = *f; continue; }
        f++;
        if (*f == 'x') fmt_u64(&o, va_arg(ap, u64), 16, 1);
        else if (*f == 'w') fmt_u64(&o, va_arg(ap, u64), 16, 8);
        else if (*f == 'd') fmt_u64(&o, va_arg(ap, u64), 10, 1);
        else if (*f == 's') { const char *s = va_arg(ap, const char *); while (*s && (o - start) < MAX_COLS) *o++ = *s++; }
    }
    *o = 0;
    return (int)(o - start);
}

static void logf(const char *f, ...)
{
    if (nlines >= MAX_LINES) return;
    va_list ap; va_start(ap, f);
    vfmt(lines[nlines++], 40, f, ap);
    va_end(ap);
}

/* log straight to the ramoops console (not the screen); long lines are fine */
__attribute__((unused)) static void klog(const char *f, ...)
{
    char b[112];
    va_list ap; va_start(ap, f);
    int n = vfmt(b, 100, f, ap);
    va_end(ap);
    b[n++] = '\n';
#if RAMOOPS_LOG
    ramoops_append(b, (u32)n);
#else
    (void)n;
#endif
}

static int str_eq_n(const u8 *a, const char *b, int n)
{
    for (int i = 0; i < n; i++) if (a[i] != (u8)b[i]) return 0;
    return 1;
}

static u32 be32(const u8 *p) { return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3]; }

static int name_is(const u8 *name, const char *want)
{
    int i = 0;
    for (; want[i]; i++) if (name[i] != (u8)want[i]) return 0;
    return name[i] == 0 || name[i] == '@';
}

static int streq(const u8 *a, const char *b)
{
    int i = 0;
    for (; b[i]; i++) if (a[i] != (u8)b[i]) return 0;
    return a[i] == 0;
}

/* property `prop` of a depth-1 node named `node` ("chosen", "memory", ...); NULL if absent */
static const u8 *fdt_prop(const u8 *dtb, const char *node, const char *prop, u32 *plen)
{
    u32 total = be32(dtb + 4), off_st = be32(dtb + 8), off_str = be32(dtb + 12);
    const u8 *p = dtb + off_st, *end = dtb + total;
    int depth = 0, match = 0;
    while (p + 4 <= end) {
        u32 tok = be32(p); p += 4;
        if (tok == 1) {
            const u8 *nm = p; int n = 0;
            while (nm[n]) n++;
            p += (n + 4) & ~3;
            depth++;
            if (depth == 2) match = name_is(nm, node);
        } else if (tok == 2) {
            if (depth == 2) match = 0;
            depth--;
        } else if (tok == 3) {
            u32 len = be32(p), noff = be32(p + 4);
            const u8 *val = p + 8;
            p += 8 + ((len + 3) & ~3);
            if (match && depth == 2 && streq(dtb + off_str + noff, prop)) { *plen = len; return val; }
        } else if (tok != 4) break;
    }
    return 0;
}

static u64 be_val(const u8 *p, u32 len)
{
    u64 v = 0;
    for (u32 i = 0; i < len && i < 8; i++) v = (v << 8) | p[i];
    return v;
}

/* find "key=" in the DTB blob, print up to 24 chars of the value */
static void find_kv(const u8 *dtb, u32 size, const char *key, int klen, const char *label)
{
    for (u32 i = 0; i + klen < size; i++) {
        if (dtb[i] == (u8)key[0] && str_eq_n(dtb + i, key, klen)) {
            char val[28]; int n = 0;
            const u8 *p = dtb + i + klen;
            while (n < 24 && p[n] > 32 && p[n] < 127) { val[n] = p[n]; n++; }
            val[n] = 0;
            logf("%s=%s", label, val);
            return;
        }
    }
    logf("%s=none", label);
}

static void fill_range(u64 off, u64 bytes, u32 col)
{
    volatile u64 *p = (volatile u64 *)(FB_BASE + off);
    u64 v = ((u64)col << 32) | col;
    for (u64 i = 0; i < bytes / 8; i++) p[i] = v;
}

static void render(u64 base_off, u32 stride)
{
    for (int l = 0; l < nlines; l++)
        for (int c = 0; lines[l][c] && c < MAX_COLS; c++)
            draw_char(base_off, stride, 8 + c * 8 * SCALE, 8 + l * LINE_H, lines[l][c], 0xFFFFFFFFu);
}

static void clear_screen(void);

/* Layout must match src/vectors.S (304-byte frame). */
struct exception_frame {
    u64 x[31];
    u64 pad0;
    u64 esr;
    u64 far;
    u64 elr;
    u64 spsr;
    u64 sp;
    u64 vbar;
};
_Static_assert(__builtin_offsetof(struct exception_frame, esr) == 256,
               "exception frame ESR offset must match vectors.S");
_Static_assert(__builtin_offsetof(struct exception_frame, vbar) == 296,
               "exception frame VBAR offset must match vectors.S");
_Static_assert(sizeof(struct exception_frame) == 304,
               "exception frame size must match vectors.S");

static volatile u32 exception_depth;

/* Best-effort fatal exception screen. Never return to the faulting context
 * and never issue a watchdog/PSCI reset, so the screen stays available. */
__attribute__((noreturn)) void exception_c_entry(struct exception_frame *f)
{
    __asm__ volatile("msr daifset, #0xf" ::: "memory");
    if (exception_depth++) {
        for (;;) __asm__ volatile("wfe");
    }

    nlines = 0;
    logf("KL7 EXCEPTION v0.6.2");
    logf("ESR=%w", f->esr);
    logf("EC=%x", (f->esr >> 26) & 0x3f);
    logf("ISS=%x", f->esr & 0x1ffffffUL);
    logf("FAR=%w", f->far);
    logf("ELR=%w", f->elr);
    logf("SPSR=%w", f->spsr);
    logf("SP=%w", f->sp);
    logf("VBAR=%w", f->vbar);
    logf("X0=%w", f->x[0]);
    logf("X1=%w", f->x[1]);
    logf("X16=%w", f->x[16]);
    logf("X30=%w", f->x[30]);
    logf("CPU=%x", RD(mpidr_el1) & 0xFFFFFFUL);
    logf("HALTED - no reset");
#if RAMOOPS_LOG
    if (ramoops_ready) ramoops_record_lines();
#endif

    clear_screen();
    render(1230UL * STRIDE_B, STRIDE_B);
    for (;;) __asm__ volatile("wfe");
}

/* append text (\n, ';' and NUL separate entries) to the line buffer, wrapping at 42 columns */
static void add_wrapped(const u8 *p, u32 n)
{
    int col = 0, open_line = 0;
    for (u32 i = 0; i < n && nlines < MAX_LINES; i++) {
        u8 c = p[i];
        if (c == '\n' || c == ';' || c == 0) {
            if (open_line) { lines[nlines][col] = 0; nlines++; open_line = 0; col = 0; }
            continue;
        }
        if (c < 32 || c > 126) c = '?';
        if (col >= 42) { lines[nlines][col] = 0; nlines++; col = 0; if (nlines >= MAX_LINES) return; }
        lines[nlines][col++] = (char)c; open_line = 1;
    }
    if (open_line && nlines < MAX_LINES) { lines[nlines][col] = 0; nlines++; }
}

static u64 cnt(void) { return RD(cntvct_el0); }

static void clear_screen(void)
{
    /* The tested visible region uses the 4352-byte stride. */
    fill_range(1230UL * STRIDE_B, 1206UL * STRIDE_B, BG);
}

/* Show the current page for `secs` seconds using the tested 4352-byte stride. */
static void show(u64 freq, int secs)
{
    /* Preserve each diagnostic page before its line buffer is cleared. */
    ramoops_record_lines();
    clear_screen();
    render(1230UL * STRIDE_B, STRIDE_B);
    u64 t0 = cnt(); int on = 0; u64 last = (u64)-1;
    while (cnt() - t0 < (u64)secs * freq) {
        u64 sec = (cnt() - t0) / freq;
        if (sec != last) {
            last = sec; on ^= 1;
            u32 col = on ? 0xFFFFFF00u : 0xFFFF00FFu;
            for (int y = 0; y < 48; y++)
                for (int x = 0; x < 48; x++)
                    put_px(1230UL * STRIDE_B, STRIDE_B, FB_W - 60 + x, 8 + y, col);
        }
    }
    nlines = 0;
}

extern char _start[], _end[];

#ifndef EMULATOR_COMPAT
extern char __pgtbl_start[], __pgtbl_end[], __seccpu_start[], __seccpu_end[];
extern void secondary_entry(void);

/* One per secondary CPU, in the NOLOAD __seccpu area. Layout shared with start.S. */
struct sec_ctx { u64 stack_top, mair, tcr, ttbr0, state, mpidr, sctlr, el; };
_Static_assert(sizeof(struct sec_ctx) == 64, "sec_ctx layout must match start.S");

static struct mm_dtb_info dinfo;
static struct mm_tables mtab;

__attribute__((unused)) static u64 psci_cpu_on(u64 mpidr, u64 entry, u64 ctx)
{
    register u64 x0 __asm__("x0") = 0xC4000003UL;      /* CPU_ON, SMC64 */
    register u64 x1 __asm__("x1") = mpidr;
    register u64 x2 __asm__("x2") = entry;
    register u64 x3 __asm__("x3") = ctx;
    __asm__ volatile("smc #0"
        : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3)
        :
        : "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "memory", "cc");
    return x0;
}

/* PSCI AFFINITY_INFO(mpidr, level 0): 0 ON, 1 OFF, 2 ON_PENDING */
static u64 psci_affinity(u64 mpidr)
{
    register u64 x0 __asm__("x0") = 0xC4000004UL;
    register u64 x1 __asm__("x1") = mpidr;
    register u64 x2 __asm__("x2") = 0;
    __asm__ volatile("smc #0"
        : "+r"(x0), "+r"(x1), "+r"(x2)
        :
        : "x3", "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "memory", "cc");
    return x0;
}

/* clean+invalidate the cache line holding p so a read sees what another core wrote to DRAM */
static void line_inval(const volatile void *p)
{
    __asm__ volatile("dc civac, %0\n dsb sy" :: "r"(p) : "memory");
}

static int overlaps(u64 a, u64 as, u64 b, u64 bs) { return a < b + bs && b < a + as; }

/* 1 if [a, a+sz) touches neither the DTB, the initrd nor any no-map reservation */
static int region_free(u64 a, u64 sz, u64 dtb, u32 dsz, u64 is, u64 ie)
{
    if (overlaps(a, sz, dtb, dsz)) return 0;
    if (ie > is && overlaps(a, sz, is, ie - is)) return 0;
    for (u32 k = 0; k < dinfo.nnomap; k++)
        if (overlaps(a, sz, dinfo.nomap[k].base, dinfo.nomap[k].size)) return 0;
    return 1;
}

/* DTB memory map -> 16 KiB identity map -> MMU on -> PSCI CPU_ON of every other core.
 * Every step is written to the ramoops console before it runs, so the last
 * line in console-ramoops-0 says exactly how far a failed boot got. */
__attribute__((unused)) static void mmu_stage(const u8 *d, u32 dsz, u64 base, u64 freq)
{
    nlines = 0;
    logf("MMU + CPU  5/5");

    int rc = mm_parse_dtb(d, &dinfo);
    if (rc) { klog("mm: DTB parse FAILED code -%d", (u64)(-rc)); logf("PARSE FAIL -%d", (u64)(-rc)); show(freq, 6); return; }
    klog("mm: parse ok ram=%d nomap=%d rsv_mapped=%d memrsv=%d cpus=%d",
         (u64)dinfo.nram, (u64)dinfo.nnomap, (u64)dinfo.nrsv_mapped, (u64)dinfo.nrsvmap, (u64)dinfo.ncpu);
    for (u32 k = 0; k < dinfo.nram; k++) klog("mm: ram %x +%x", dinfo.ram[k].base, dinfo.ram[k].size);
    for (u32 k = 0; k < dinfo.nnomap; k++) klog("mm: nomap %x +%x", dinfo.nomap[k].base, dinfo.nomap[k].size);
    for (u32 k = 0; k < dinfo.ncpu; k++) klog("mm: cpu%d mpidr=%x", (u64)k, dinfo.cpu_mpidr[k]);

    u32 l1 = 0, l2 = 0;
    const u8 *vs = fdt_prop(d, "chosen", "linux,initrd-start", &l1);
    const u8 *ve = fdt_prop(d, "chosen", "linux,initrd-end", &l2);
    u64 is = vs ? be_val(vs, l1) : 0, ie = ve ? be_val(ve, l2) : 0;

    u64 pool = (u64)__pgtbl_start, pool_sz = (u64)(__pgtbl_end - __pgtbl_start);
    u64 sec = (u64)__seccpu_start, sec_sz = (u64)(__seccpu_end - __seccpu_start);
    u64 img_sz = (u64)(_end - _start);
    klog("mm: image %x +%x  pool %x +%x  seccpu %x +%x", base, img_sz, pool, pool_sz, sec, sec_sz);
    if (!region_free(base, img_sz, (u64)d, dsz, is, ie) || !region_free(pool, pool_sz + sec_sz, (u64)d, dsz, is, ie)) {
        klog("mm: ABORT image/pool overlaps DTB, initrd or a no-map region");
        logf("ABORT: POOL OVERLAP"); show(freq, 6); return;
    }

    struct mm_range forced = { RAMOOPS_BASE, RAMOOPS_SIZE };
    struct mm_range image = { base, img_sz };
    rc = mm_build(&dinfo, pool, pool_sz, &forced, &image, RD(id_aa64mmfr0_el1) & 0xf, &mtab);
    klog("mm: build rc=%d tables=%d oom=%d", (u64)(rc < 0 ? -rc : rc), (u64)mtab.tables_used, (u64)mtab.oom);
    if (rc) { logf("BUILD FAIL"); show(freq, 6); return; }

    /* Software-walk the new tables for everything the shim touches next. */
    u64 sp_now; __asm__ volatile("mov %0, sp" : "=r"(sp_now));
    struct { u64 va; const char *what; } need[] = {
        { base, "image" }, { (u64)secondary_entry, "secondary_entry" }, { sp_now, "stack" },
        { RAMOOPS_CONSOLE_BASE, "ramoops" }, { RAMOOPS_CONSOLE_BASE + RAMOOPS_CONSOLE_SIZE - 1, "ramoops end" },
        { FB_BASE + 1230UL * STRIDE_B, "fb" }, { FB_BASE + 2436UL * STRIDE_B - 1, "fb end" },
        { 0x0C000000UL, "gicd" }, { 0x0C040000UL, "gicr" }, { WDT_BASE, "wdt" },
        { (u64)d, "dtb" }, { pool, "pool" }, { sec, "seccpu" },
    };
    for (u32 k = 0; k < sizeof need / sizeof need[0]; k++) {
        u64 pa = 0, desc = 0;
        int ok = mm_translate(&mtab, need[k].va, &pa, &desc) && pa == need[k].va;
        if (!ok) {
            klog("mm: VERIFY FAIL %s va=%x", need[k].what, need[k].va);
            logf("VERIFY FAIL %s", need[k].what); show(freq, 6); return;
        }
    }
    klog("mm: verify ok (%d addresses)", (u64)(sizeof need / sizeof need[0]));

    klog("mm: ttbr0=%x tcr=%x mair=%x", mtab.ttbr0, mtab.tcr, mtab.mair);
    klog("mm: enabling MMU, SCTLR=%x", RD(sctlr_el1));
    mm_enable(&mtab);
    u64 sctlr_on = RD(sctlr_el1);
    klog("mm: MMU ON SCTLR=%x", sctlr_on);
    logf("MMU ON SCTLR=%x", sctlr_on);

    /* MMIO through the Device mapping: GICv3 distributor ID registers */
    u32 typer = *(volatile u32 *)(0x0C000000UL + 0x0004), pidr2 = *(volatile u32 *)(0x0C000000UL + 0xFFE8);
    klog("mm: GICD_TYPER=%x GICD_PIDR2=%x arch=%d", (u64)typer, (u64)pidr2, (u64)((pidr2 >> 4) & 0xf));
    logf("GICD PIDR2=%x ARCH=%d", (u64)pidr2, (u64)((pidr2 >> 4) & 0xf));
    /* RAM through the Normal mapping */
    volatile u64 *tw = (volatile u64 *)(sec + 0x800);
    *tw = 0x0123456789abcdefUL;
    __asm__ volatile("dsb sy" ::: "memory");
    u64 rb = *tw;
    klog("mm: ram rw test %s", rb == 0x0123456789abcdefUL ? "ok" : "FAIL");
    logf("RAM RW %s", rb == 0x0123456789abcdefUL ? "OK" : "FAIL");

#if MMU_FAULT_TEST
    klog("mm: FAULT TEST reading no-map 0x70000000 (expect translation fault)");
    (void)*(volatile u32 *)0x70000000UL;
    klog("mm: FAULT TEST returned (!)");
#endif

#if CPU_BRINGUP
    struct sec_ctx *ctx = (struct sec_ctx *)sec;
    u8 *stacks = (u8 *)sec + 0x1000;
    u64 self = RD(mpidr_el1) & 0xFFFFFFUL;
    u32 up = 0, tried = 0;
    klog("cpu: boot cpu mpidr=%x", self);
    for (u32 k = 0; k < dinfo.ncpu; k++) {
        u64 m = dinfo.cpu_mpidr[k];
        if (m == self) { logf("CPU%d %x BOOT CPU", (u64)k, m); continue; }
        ctx[k].stack_top = (u64)(stacks + (u64)(k + 1) * 0x2000);
        ctx[k].mair = mtab.mair; ctx[k].tcr = mtab.tcr; ctx[k].ttbr0 = mtab.ttbr0;
        ctx[k].state = 0; ctx[k].mpidr = 0; ctx[k].sctlr = 0; ctx[k].el = 0;
        /* The context was written through a Non-cacheable mapping but may still sit in
         * (or be shadowed by) a cache line; the secondary reads DRAM with its MMU off.
         * Clean+invalidate both ends of the 64-byte struct before CPU_ON. */
        line_inval(&ctx[k]);
        line_inval((volatile u8 *)&ctx[k] + 63);
        u64 r = psci_cpu_on(m, (u64)secondary_entry, (u64)&ctx[k]);
        tried++;
        u64 t0 = cnt(), st = 0;
        while (cnt() - t0 < freq * 3 / 10) {
            line_inval(&ctx[k]);
            st = *(volatile u64 *)&ctx[k].state;
            if (st == 2) break;
        }
        u64 aff = psci_affinity(m);
        line_inval(&ctx[k]);
        klog("cpu: CPU_ON %x entry=%x ret=%x aff=%x state=%d mpidr=%x el=%x sctlr=%x", m, (u64)secondary_entry, r, aff, st,
             *(volatile u64 *)&ctx[k].mpidr, *(volatile u64 *)&ctx[k].el, *(volatile u64 *)&ctx[k].sctlr);
        logf("CPU%d %x R=%x ST=%d A=%x", (u64)k, m, r, st, aff);
        if (st == 2) up++;
    }
    klog("cpu: %d of %d secondary cores reached MMU-on", (u64)up, (u64)tried);
    klog("cpu: framebuffer squares: orange = entered, red = zero context (cache), blue = enabling MMU, green = MMU on");
    logf("SECONDARY UP %d/%d", (u64)up, (u64)tried);
#endif
    show(freq, 8);
}
#endif

void shim_main(u64 dtb, u64 x1, u64 x2, u64 x3, u64 base)
{
    exception_depth = 0;
    ramoops_init();
#if TEST_EXCEPTION_VECTORS
    /* Deliberate synchronous exception used only to validate VBAR_EL1. */
    __asm__ volatile("udf #0");
#endif
    u64 el = (RD(CurrentEL) >> 2) & 3;
    u64 freq = RD(cntfrq_el0);
    if (!freq) freq = 13000000;

#ifdef EMULATOR_COMPAT
    /* The Unicorn harness is not a secure monitor: emulate the two PSCI
     * return values so the framebuffer path can be tested independently. */
    u64 psci_ver = 0x00010001UL;
    u64 psci_cpu_on_features = 0;
#else
    /* Query PSCI_VERSION through the SMC conduit advertised by the DTB. */
    register u64 psci_ver __asm__("x0") = 0x84000000UL;
    __asm__ volatile(
        "smc #0"
        : "+r"(psci_ver)
        :
        : "x1", "x2", "x3", "memory", "cc"
    );

    /* Query whether PSCI_CPU_ON_AARCH64 is implemented.
     * PSCI_FEATURES: x0 = 0x8400000A, x1 = queried function ID.
     * This only queries support; it does not start another CPU. */
    register u64 psci_features_fid __asm__("x0") = 0x8400000AUL;
    register u64 psci_cpu_on_fid __asm__("x1") = 0xC4000003UL;
    __asm__ volatile(
        "smc #0"
        : "+r"(psci_features_fid), "+r"(psci_cpu_on_fid)
        :
        : "x2", "x3", "memory", "cc"
    );
    u64 psci_cpu_on_features = psci_features_fid;
#endif

    const u8 *d = (const u8 *)dtb;
    u32 magic = 0, size = 0;
    if (dtb) { magic = be32(d); size = be32(d + 4); }
    int dtb_ok = (magic == 0xd00dfeed && size < 0x800000);

    /* page 1: CPU / entry state */
    logf("KL7 SHIM v0.6.2  1/5");
    logf("EL=%d MPIDR=%x", el, RD(mpidr_el1));
    logf("MIDR=%w", RD(midr_el1));
    logf("PFR0=%x", RD(id_aa64pfr0_el1));
    logf("MMFR0=%x", RD(id_aa64mmfr0_el1));
    logf("ISAR0=%x", RD(id_aa64isar0_el1));
    logf("ISAR1=%x", RD(id_aa64isar1_el1));
    logf("CNTFRQ=%d", freq);
    logf("PSCI_VER=%w", psci_ver);
    logf("PSCI_CPU_ON=%w", psci_cpu_on_features);
    logf("BASE=%x", base);
    logf("SIZE=%x", (u64)(_end - _start));
    if (el == 2) logf("SCTLR_EL2=%x HCR=%x", RD(sctlr_el2), RD(hcr_el2));
    else         logf("SCTLR_EL1=%x", RD(sctlr_el1));
    logf("VBAR_EL1=%w", RD(vbar_el1));
    logf("X0=%x", dtb);
    logf("X1=%x X2=%x X3=%x", x1, x2, x3);
    logf("DTB MAGIC=%w SIZE=%x", magic, size);
    show(freq, 4);

    /* page 2: boot mode keys + memory layout from DTB */
    logf("DTB INFO  2/5");
    if (dtb_ok) {
        find_kv(d, size, "bootopt=", 8, "BOOTOPT");
        u32 l;
        const u8 *v;
        if ((v = fdt_prop(d, "chosen", "linux,initrd-start", &l))) logf("INITRD S=%x", be_val(v, l));
        if ((v = fdt_prop(d, "chosen", "linux,initrd-end", &l)))   logf("INITRD E=%x", be_val(v, l));
        if ((v = fdt_prop(d, "memory", "reg", &l))) {
            logf("MEM LEN=%d", (u64)l);
            if (l >= 16) logf("MEM %x +%x", be_val(v, 8), be_val(v + 8, 8));
        }
        logf("DTB END=%x", dtb + size);
    } else {
        logf("DTB BAD");
    }
    show(freq, 4);

    /* page 3: bootconfig appended to the initrd by LK (androidboot.* live here, not in bootargs) */
    logf("BOOTCONFIG  3/5");
    if (dtb_ok) {
        u32 l1 = 0, l2 = 0;
        const u8 *vs = fdt_prop(d, "chosen", "linux,initrd-start", &l1);
        const u8 *ve = fdt_prop(d, "chosen", "linux,initrd-end", &l2);
        u64 is = vs ? be_val(vs, l1) : 0, ie = ve ? be_val(ve, l2) : 0;
        if (is && ie > is + 32 && ie - is < 0x10000000UL) {
            const u8 *t = (const u8 *)ie;
            if (str_eq_n(t - 12, "#BOOTCONFIG\n", 12)) {
                u32 sz = (u32)t[-20] | ((u32)t[-19] << 8) | ((u32)t[-18] << 16) | ((u32)t[-17] << 24);
                if (sz < 0x4000) add_wrapped(t - 20 - sz, sz);
                else logf("BC SIZE BAD %x", (u64)sz);
            } else logf("NO #BOOTCONFIG TAIL");
        } else logf("NO INITRD");
    }
    show(freq, 4);

    /* page 4: /chosen/bootargs, 42 chars per line */
    logf("BOOTARGS  4/5");
    if (dtb_ok) {
        u32 l = 0;
        const u8 *v = fdt_prop(d, "chosen", "bootargs", &l);
        if (v) {
            u32 pos = 0;
            while (pos < l && v[pos] && nlines < MAX_LINES) {
                char *o = lines[nlines++];
                int n = 0;
                while (n < 42 && pos < l && v[pos]) o[n++] = (char)v[pos++];
                o[n] = 0;
            }
        } else logf("NO BOOTARGS");
    }
    show(freq, 4);

#if MMU_STAGE && !defined(EMULATOR_COMPAT)
    if (dtb_ok) mmu_stage(d, size, base, freq);
#endif

#ifdef EMULATOR_COMPAT
    /* Let the Unicorn harness stop after page 4 and write its PNG snapshots.
     * This BRK is never present in a normal hardware build. */
    __asm__ volatile("brk #0");
    for (;;) __asm__ volatile("wfe");
#else
    /* PSCI SYSTEM_RESET (smc conduit per DTB), then MTK TOPRGU fallback. */
    register u64 r0 __asm__("x0") = 0x84000009UL;
    __asm__ volatile("smc #0" : "+r"(r0) : : "x1", "x2", "x3", "memory");
    *(volatile u32 *)(WDT_BASE + 0x14) = 0x1209;
    for (;;) __asm__ volatile("wfe");
#endif
}
