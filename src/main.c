/* ios-kl7 v0.8.0: ramoops-first diagnostics, bottom-only progress UI, and a
 * controlled high-RAM test-payload handoff. v0.6.3 proved secondary CPU_ON;
 * this stage leaves secondaries off so a future XNU entry can own their startup.
 * No libc or SIMD. The MMU stage intentionally keeps caches disabled. */
typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long u64;
typedef __builtin_va_list va_list;
#define va_start(a, b) __builtin_va_start(a, b)
#define va_end(a)      __builtin_va_end(a)
#define va_arg(a, t)   __builtin_va_arg(a, t)

#include "font.h"
#include "mm.h"
#include "loader.h"
#include "xnu_payload.h"

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
#define CPU_BRINGUP 0    /* Leave secondaries OFF for XNU to start. */
#endif
#ifndef LOADER_HANDOFF
#define LOADER_HANDOFF 1 /* 0: stop after diagnostics, without payload handoff */
#endif
#ifndef XNU_PAYLOAD
#define XNU_PAYLOAD 0 /* 0: verified test ABI; 1: SWEETXNU Mach-O container */
#endif
#if LOADER_HANDOFF && CPU_BRINGUP
#error "LOADER_HANDOFF requires CPU_BRINGUP=0; XNU must start its own secondary CPUs"
#endif
#if LOADER_HANDOFF && !MMU_STAGE
#error "LOADER_HANDOFF requires MMU_STAGE=1"
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
#define VISIBLE_FB_OFF (1230UL * STRIDE_B)
#define PROGRESS_Y 1118
#define PROGRESS_H 88
#define PROGRESS_BAR_X 80
#define PROGRESS_BAR_Y (PROGRESS_Y + 58)
#define PROGRESS_BAR_W (FB_W - 2 * PROGRESS_BAR_X)
#define PROGRESS_BAR_H 14
#define TEST_PAYLOAD_BASE 0x140000000UL
#define TEST_PAYLOAD_NAME "ios-kl7-test.bin"
#define XNU_PAYLOAD_NAME "ios-kl7-xnu.bin"
#define TEST_PAYLOAD_RESULT 0x3054534554374c4bUL /* bytes: KL7TEST0 */
#define BOOT_ARGS_MAGIC 0x30544f4f42374c4bUL /* bytes: KL7BOOT0 */

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

static void fill_range(u64 off, u64 bytes, u32 col);


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
    ramoops_append_cstr("ios-kl7 v0.8.0: shim entered\n");
}

#else
static volatile u32 ramoops_ready;
static void ramoops_init(void) { ramoops_ready = 0; }
#endif

static void fmt_u64(char **o, char *end, u64 v, int base, int minw)
{
    char tmp[24]; int n = 0;
    do { int d = v % base; tmp[n++] = d < 10 ? '0' + d : 'a' + d - 10; v /= base; } while (v);
    while (n < minw) tmp[n++] = '0';
    while (n && *o < end) *(*o)++ = tmp[--n];
}

/* Bounded formatter: always reserves one byte for NUL, even for several
 * 64-bit conversions in one record. */
static int vfmt(char *o, int cap, const char *f, va_list ap)
{
    char *start = o;
    if (cap <= 0) return 0;
    char *end = o + cap - 1;
    for (; *f && o < end; f++) {
        if (*f != '%') { *o++ = *f; continue; }
        f++;
        if (!*f) break;
        if (*f == 'x') fmt_u64(&o, end, va_arg(ap, u64), 16, 1);
        else if (*f == 'w') fmt_u64(&o, end, va_arg(ap, u64), 16, 8);
        else if (*f == 'd') fmt_u64(&o, end, va_arg(ap, u64), 10, 1);
        else if (*f == 's') {
            const char *s = va_arg(ap, const char *);
            while (*s && o < end) *o++ = *s++;
        } else if (o < end) *o++ = *f;
    }
    *o = 0;
    return (int)(o - start);
}

static void logf(const char *f, ...)
{
    char b[112];
    va_list ap; va_start(ap, f);
    int n = vfmt(b, 100, f, ap);
    va_end(ap);
    if (n < 0) return;
    b[n++] = '\n';
#if RAMOOPS_LOG
    ramoops_append(b, (u32)n);
#else
    (void)b; (void)n;
#endif
}

/* Legacy MMU/CPU diagnostics use klog; both paths now stream to ramoops. */
#define klog logf

static int progress_percent;
static int progress_failed;

static void fill_rect(int x, int y, int w, int h, u32 col)
{
    for (int yy = 0; yy < h; yy++)
        for (int xx = 0; xx < w; xx++)
            put_px(VISIBLE_FB_OFF, STRIDE_B, x + xx, y + yy, col);
}

/* Only this 88-pixel strip is touched. The rest of the boot screen is left
 * exactly as the Android bootloader handed it to us. */
static void progress_redraw(void)
{
    fill_range(VISIBLE_FB_OFF + (u64)PROGRESS_Y * STRIDE_B,
               (u64)PROGRESS_H * STRIDE_B, BG);

    char pct[4];
    int n = 0;
    if (progress_percent >= 100) {
        pct[n++] = '1'; pct[n++] = '0'; pct[n++] = '0';
    } else if (progress_percent >= 10) {
        pct[n++] = (char)('0' + progress_percent / 10);
        pct[n++] = (char)('0' + progress_percent % 10);
    } else {
        pct[n++] = (char)('0' + progress_percent);
    }
    pct[n++] = '%';
    int tx = (FB_W - n * 8 * SCALE) / 2;
    u32 text_col = progress_failed ? 0xFFFF7070u : 0xFFFFFFFFu;
    for (int i = 0; i < n; i++)
        draw_char(VISIBLE_FB_OFF, STRIDE_B, tx + i * 8 * SCALE,
                  PROGRESS_Y + 2, pct[i], text_col);

    u32 edge = progress_failed ? 0xFFFF5555u : 0xFF909090u;
    u32 track = 0xFF303030u;
    u32 fill = progress_failed ? 0xFFB02020u : 0xFF20E060u;
    fill_rect(PROGRESS_BAR_X, PROGRESS_BAR_Y,
              PROGRESS_BAR_W, PROGRESS_BAR_H, edge);
    fill_rect(PROGRESS_BAR_X + 2, PROGRESS_BAR_Y + 2,
              PROGRESS_BAR_W - 4, PROGRESS_BAR_H - 4, track);
    int usable = PROGRESS_BAR_W - 4;
    int filled = usable * progress_percent / 100;
    if (filled > 0)
        fill_rect(PROGRESS_BAR_X + 2, PROGRESS_BAR_Y + 2,
                  filled, PROGRESS_BAR_H - 4, fill);
}

static void progress_set(int p, const char *what)
{
    if (p < 0) p = 0;
    if (p > 100) p = 100;
    if (p < progress_percent) p = progress_percent;
    progress_percent = p;
    logf("progress %d%%: %s", (u64)p, what);
    progress_redraw();
}

static void progress_fail(const char *why)
{
    progress_failed = 1;
    logf("FAIL: %s", why);
    progress_redraw();
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

/* Fatal exception report: write registers to ramoops, paint only the bottom
 * progress strip red, and never return to the faulting context. */
__attribute__((noreturn)) void exception_c_entry(struct exception_frame *f)
{
    __asm__ volatile("msr daifset, #0xf" ::: "memory");
    if (exception_depth++) {
        for (;;) __asm__ volatile("wfe");
    }

    logf("KL7 EXCEPTION v0.8.0");
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
    progress_fail("fatal exception; registers are in ramoops");
    for (;;) __asm__ volatile("wfe");
}

/* Stream full DTB text properties to ramoops in bounded lines; none of this
 * data is painted on the framebuffer. Long bootargs/bootconfig are not capped
 * by the old 25-line diagnostic-page buffer. */
static void log_text_blob(const char *label, const u8 *p, u32 len)
{
    logf("%s BEGIN bytes=%d", label, (u64)len);
#if RAMOOPS_LOG
    char line[96];
    u32 used = 0;
    for (u32 i = 0; i < len; i++) {
        u8 c = p[i];
        if (c == 0 || c == '\n' || c == ';' || used >= 88) {
            if (used) {
                line[used++] = '\n';
                ramoops_append(line, used);
                used = 0;
            }
            if (c == 0) break;
            if (c == '\n' || c == ';') continue;
        }
        if (c < 32 || c > 126) c = '?';
        line[used++] = (char)c;
    }
    if (used) {
        line[used++] = '\n';
        ramoops_append(line, used);
    }
#endif
    logf("%s END", label);
}

#ifndef EMULATOR_COMPAT
static u64 cnt(void) { return RD(cntvct_el0); }
#endif

extern char _start[], _end[];

#ifndef EMULATOR_COMPAT
extern char __pgtbl_start[], __pgtbl_end[], __seccpu_start[], __seccpu_end[];
extern void secondary_entry(void);

/* One per secondary CPU, in the NOLOAD __seccpu area. Layout shared with start.S. */
struct sec_ctx { u64 stack_top, mair, tcr, ttbr0, state, mpidr, sctlr, el, x0_seen, pad[7]; };
_Static_assert(sizeof(struct sec_ctx) == 128, "sec_ctx layout must match start.S");

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

#if CPU_BRINGUP
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
#endif

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

/* Parse the live DTB, build/verify the 16 KiB identity map, then enable the
 * MMU. Optional CPU_ON remains available for diagnostic-only builds, but the
 * payload-handoff build keeps all secondary CPUs OFF for XNU. Every detail is
 * streamed to ramoops; the display gets only the bottom progress bar. */
static int mmu_stage(const u8 *d, u32 dsz, u64 base, u64 freq)
{
    logf("stage: memory-map/MMU begin");
    int rc = mm_parse_dtb(d, &dinfo);
    if (rc) {
        klog("mm: DTB parse FAILED code -%d", (u64)(-rc));
        progress_fail("DTB memory map parse failed");
        return rc;
    }
    progress_set(30, "DTB RAM/reservations parsed");
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
        progress_fail("shim/page-table memory overlaps live boot data");
        return -10;
    }
    progress_set(36, "live RAM ranges checked for overlap");

    struct mm_range forced = { RAMOOPS_BASE, RAMOOPS_SIZE };
    struct mm_range image = { base, img_sz };
    rc = mm_build(&dinfo, pool, pool_sz, &forced, &image, RD(id_aa64mmfr0_el1) & 0xf, &mtab);
    klog("mm: build rc=%d tables=%d oom=%d", (u64)(rc < 0 ? -rc : rc), (u64)mtab.tables_used, (u64)mtab.oom);
    if (rc) { logf("BUILD FAIL"); progress_fail("translation-table build failed"); return rc; }
    progress_set(48, "16 KiB identity page tables built");

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
            logf("VERIFY FAIL %s", need[k].what);
            progress_fail("identity-map verification failed");
            return -11;
        }
    }
    klog("mm: verify ok (%d addresses)", (u64)(sizeof need / sizeof need[0]));
    progress_set(58, "image/DTB/stack/ramoops/MMIO translations verified");

    klog("mm: ttbr0=%x tcr=%x mair=%x", mtab.ttbr0, mtab.tcr, mtab.mair);
    klog("mm: enabling MMU, SCTLR=%x", RD(sctlr_el1));
    mm_enable(&mtab);
    u64 sctlr_on = RD(sctlr_el1);
    klog("mm: MMU ON SCTLR=%x", sctlr_on);
    logf("MMU ON SCTLR=%x", sctlr_on);
    progress_set(66, "MMU enabled with caches still off");

    /* MMIO through the Device mapping: GICv3 distributor ID registers */
    u32 typer = *(volatile u32 *)(0x0C000000UL + 0x0004), pidr2 = *(volatile u32 *)(0x0C000000UL + 0xFFE8);
    klog("mm: GICD_TYPER=%x GICD_PIDR2=%x arch=%d", (u64)typer, (u64)pidr2, (u64)((pidr2 >> 4) & 0xf));
    logf("GICD PIDR2=%x ARCH=%d", (u64)pidr2, (u64)((pidr2 >> 4) & 0xf));
    if (((pidr2 >> 4) & 0xf) != 3) {
        progress_fail("GIC distributor is not identified as GICv3");
        return -12;
    }
    progress_set(72, "GICv3 distributor identified");
    /* RAM through the Normal mapping */
    volatile u64 *tw = (volatile u64 *)(sec + 0x800);
    *tw = 0x0123456789abcdefUL;
    __asm__ volatile("dsb sy" ::: "memory");
    u64 rb = *tw;
    klog("mm: ram rw test %s", rb == 0x0123456789abcdefUL ? "ok" : "FAIL");
    logf("RAM RW %s", rb == 0x0123456789abcdefUL ? "OK" : "FAIL");
    if (rb != 0x0123456789abcdefUL) {
        progress_fail("high-level RAM read/write test failed");
        return -13;
    }
    progress_set(78, "normal-memory read/write verified");

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
        u32 ix = (u32)((m >> 8) & 0xff);                 /* the secondary derives the same index from its MPIDR */
        if (ix >= 8 || (m & ~0xFF00UL)) { klog("cpu: skip mpidr=%x (not Aff1-only)", m); continue; }
        struct sec_ctx *c = &ctx[ix];
        c->stack_top = (u64)(stacks + (u64)(ix + 1) * 0x2000);
        c->mair = mtab.mair; c->tcr = mtab.tcr; c->ttbr0 = mtab.ttbr0;
        c->state = 0; c->mpidr = 0; c->sctlr = 0; c->el = 0; c->x0_seen = 0;
        line_inval(c);
        line_inval((volatile u8 *)c + 127);
        u64 r = psci_cpu_on(m, (u64)secondary_entry, (u64)c);
        tried++;
        u64 t0 = cnt(), st = 0;
        while (cnt() - t0 < freq * 3 / 10) {
            line_inval(c); line_inval((volatile u8 *)c + 127);
            st = *(volatile u64 *)&c->state;
            if (st == 2) break;
        }
        u64 aff = psci_affinity(m);
        line_inval(c); line_inval((volatile u8 *)c + 127);
        klog("cpu: CPU_ON %x entry=%x ret=%x aff=%x state=%d mpidr=%x el=%x sctlr=%x x0=%x ctx=%x", m, (u64)secondary_entry, r, aff, st,
             *(volatile u64 *)&c->mpidr, *(volatile u64 *)&c->el, *(volatile u64 *)&c->sctlr, *(volatile u64 *)&c->x0_seen, (u64)c);
        logf("CPU%d %x R=%x ST=%d A=%x", (u64)k, m, r, st, aff);
        if (st == 2) up++;
        progress_set(78 + (tried * 2 / (dinfo.ncpu ? dinfo.ncpu : 1)), "secondary CPU_ON checks running");
    }
    klog("cpu: %d of %d secondary cores reached MMU-on", (u64)up, (u64)tried);
    logf("SECONDARY UP %d/%d", (u64)up, (u64)tried);
    progress_set(78 + (tried * 2 / (dinfo.ncpu ? dinfo.ncpu : 1)), "secondary CPU_ON checks complete");
#else
    klog("cpu: CPU_ON intentionally skipped; XNU must start secondaries");
#endif
    return 0;
}
#endif

#ifndef EMULATOR_COMPAT
extern void transfer_to_payload(u64 target, u64 boot_args) __attribute__((noreturn));
static int range_in_ram(u64 a, u64 sz);

#if XNU_PAYLOAD
#define XNU_FB_BASE   0xFDFFF000UL
#define XNU_FB_STRIDE 4352u
#define XNU_FB_WIDTH  1080u
#define XNU_FB_HEIGHT 2436u
#define XNU_EXPECTED_PHYS_BASE 0x140000000UL
#define XNU_EXPECTED_MEM_SIZE  0xFFEFF000UL

static u64 align_up_u64(u64 n, u64 align)
{
    if (n > (u64)-1 - (align - 1)) return 0;
    return (n + align - 1) & ~(align - 1);
}

/* MMU remains enabled, but the loader intentionally has C/I caches disabled.
 * Word copies avoid the byte-at-a-time loop on a multi-megabyte kernel image. */
static void copy_words(u64 dst, const u8 *src, u64 n)
{
    volatile u64 *d = (volatile u64 *)dst;
    const volatile u64 *s = (const volatile u64 *)src;
    for (u64 i = 0; i < (n + 7u) / 8u; i++) d[i] = s[i];
}

static void zero_words(u64 dst, u64 n)
{
    volatile u64 *d = (volatile u64 *)dst;
    for (u64 i = 0; i < n / 8u; i++) d[i] = 0;
}

static int xnu_target_region_ok(u64 dst, u64 sz, u64 dtb, u32 dsz,
                                u64 initrd_start, u64 initrd_end, u64 base)
{
    u64 image_sz = (u64)(_end - _start);
    u64 pool = (u64)__pgtbl_start;
    u64 pool_sz = (u64)(__pgtbl_end - __pgtbl_start);
    u64 sec = (u64)__seccpu_start;
    u64 sec_sz = (u64)(__seccpu_end - __seccpu_start);
    if (!sz || dst + sz < dst || !range_in_ram(dst, sz)) return 0;
    if (!region_free(dst, sz, dtb, dsz, initrd_start, initrd_end)) return 0;
    if (overlaps(dst, sz, base, image_sz) || overlaps(dst, sz, pool, pool_sz) ||
        overlaps(dst, sz, sec, sec_sz) || overlaps(dst, sz, RAMOOPS_BASE, RAMOOPS_SIZE))
        return 0;
    return 1;
}

static int loader_handoff_xnu(const u8 *d, u32 dsz, u64 base,
                              u64 initrd_start, u64 initrd_end,
                              const loader_u8 *file, u32 file_size)
{
    struct xnu_payload_view p;
    int rc = xnu_payload_parse(file, file_size, &p);
    if (rc) {
        logf("xnu: SWEETXNU container rejected rc=%d", (u64)(-rc));
        progress_fail("XNU payload container is malformed");
        return -30;
    }
    logf("xnu: payload size=%d segments=%d virt=%x entry=%x", (u64)file_size,
         (u64)p.nsegs, p.virt_base, p.entry_va);
    logf("xnu: phys=%x mem=%x end=%x adt=%x+%x", p.phys_base, p.mem_size,
         p.kernel_end, p.adt_off, p.adt_size);
    if (p.phys_base != XNU_EXPECTED_PHYS_BASE ||
        p.mem_size != XNU_EXPECTED_MEM_SIZE) {
        logf("xnu: refusing non-KL7 ADT RAM phys=%x size=%x", p.phys_base, p.mem_size);
        progress_fail("payload ADT does not describe the expected KL7 upper DRAM bank");
        return -31;
    }
    if (!range_in_ram(p.phys_base, p.mem_size)) {
        progress_fail("ADT kernel RAM range is outside the live FDT RAM banks");
        return -32;
    }
    progress_set(87, "XNU container and target RAM validated");

    /* Preflight every physical write before the first segment is copied. */
    u64 max_end_va = p.virt_base;
    int entry_found = 0;
    for (u32 i = 0; i < p.nsegs; i++) {
        struct xnu_payload_segment seg;
        if (xnu_payload_get_segment(&p, i, &seg)) return -33;
        u64 delta = seg.vmaddr - p.virt_base;
        if (delta > p.mem_size || seg.vmsize > p.mem_size - delta ||
            p.phys_base > (u64)-1 - delta) {
            logf("xnu: segment %d VA=%x size=%x exceeds ADT RAM", (u64)i, seg.vmaddr, seg.vmsize);
            progress_fail("an XNU segment exceeds the ADT RAM window");
            return -34;
        }
        u64 dst = p.phys_base + delta;
        if (!xnu_target_region_ok(dst, seg.vmsize, (u64)d, dsz,
                                  initrd_start, initrd_end, base)) {
            logf("xnu: segment %d destination rejected PA=%x size=%x", (u64)i, dst, seg.vmsize);
            progress_fail("an XNU segment overlaps live or reserved memory");
            return -35;
        }
        u64 pa0 = 0, pa1 = 0;
        if (!mm_translate(&mtab, dst, &pa0, 0) || pa0 != dst ||
            !mm_translate(&mtab, dst + seg.vmsize - 1, &pa1, 0) ||
            pa1 != dst + seg.vmsize - 1) {
            logf("xnu: segment %d lacks an identity mapping", (u64)i);
            progress_fail("an XNU segment is not identity-mapped");
            return -36;
        }
        u64 seg_end = seg.vmaddr + seg.vmsize;
        if (seg_end > max_end_va) max_end_va = seg_end;
        if (p.entry_va >= seg.vmaddr && p.entry_va < seg_end) entry_found = 1;
    }
    if (!entry_found || p.kernel_end < max_end_va) {
        progress_fail("XNU entry or kernel-end metadata is inconsistent");
        return -37;
    }

    u64 adt_pa = p.phys_base + (p.kernel_end - p.virt_base);
    u64 adt_copy_span = align_up_u64(p.adt_size, 8);
    if (!adt_copy_span || adt_pa < p.phys_base ||
        adt_pa > (u64)-1 - adt_copy_span) {
        progress_fail("ADT destination address overflow");
        return -38;
    }
    if (!xnu_target_region_ok(adt_pa, adt_copy_span, (u64)d, dsz,
                              initrd_start, initrd_end, base)) {
        logf("xnu: ADT destination rejected PA=%x size=%x", adt_pa, adt_copy_span);
        progress_fail("ADT destination overlaps live or reserved memory");
        return -39;
    }
    u64 adt_va = p.virt_base + (adt_pa - p.phys_base);
    u64 ba_pa = align_up_u64(adt_pa + adt_copy_span, XNU_PAGE_SIZE);
    if (!ba_pa || ba_pa > (u64)-1 - XNU_PAGE_SIZE) {
        progress_fail("boot_args destination address overflow");
        return -40;
    }
    u64 ba_end = ba_pa + XNU_PAGE_SIZE;
    if (ba_pa < p.phys_base || ba_end > p.phys_base + p.mem_size ||
        !xnu_target_region_ok(ba_pa, XNU_PAGE_SIZE, (u64)d, dsz,
                              initrd_start, initrd_end, base)) {
        logf("xnu: boot_args destination rejected PA=%x size=%x", ba_pa, (u64)XNU_PAGE_SIZE);
        progress_fail("boot_args exceed available kernel RAM or overlap live memory");
        return -41;
    }

    progress_set(90, "all XNU segment destinations preflighted");
    for (u32 i = 0; i < p.nsegs; i++) {
        struct xnu_payload_segment seg;
        (void)xnu_payload_get_segment(&p, i, &seg);
        u64 dst = p.phys_base + (seg.vmaddr - p.virt_base);
        copy_words(dst, file + seg.file_off, seg.file_size);
        u64 copied = align_up_u64(seg.file_size, 8);
        if (seg.vmsize > copied) zero_words(dst + copied, seg.vmsize - copied);
        logf("xnu: copied seg %d VA=%x PA=%x file=%x vm=%x", (u64)i,
             seg.vmaddr, dst, seg.file_size, seg.vmsize);
    }
    __asm__ volatile("dsb sy" ::: "memory");
    progress_set(94, "Mach-O segments copied to high DRAM");

    copy_words(adt_pa, file + p.adt_off, p.adt_size);
    zero_words(ba_pa, XNU_PAGE_SIZE);

    /* XNU arm64 boot_args rev 2, matching the sweet ABI exactly (736 bytes). */
    struct boot_video {
        u64 v_baseAddr, v_display, v_rowBytes, v_width, v_height, v_depth;
    };
    struct xnu_boot_args {
        u16 Revision, Version;
        u64 virtBase, physBase, memSize, topOfKernelData;
        struct boot_video Video;
        u32 machineType;
        u64 deviceTreeP;
        u32 deviceTreeLength;
        char CommandLine[XNU_BOOT_LINE_LENGTH];
        u64 bootFlags, memSizeActual;
    };
    _Static_assert(__builtin_offsetof(struct xnu_boot_args, Video) == 40, "boot_args Video offset");
    _Static_assert(__builtin_offsetof(struct xnu_boot_args, CommandLine) == 108, "boot_args command line offset");
    _Static_assert(sizeof(struct xnu_boot_args) == 736, "boot_args rev 2 size");
    volatile struct xnu_boot_args *ba = (volatile struct xnu_boot_args *)ba_pa;
    ba->Revision = 2;
    ba->Version = 2;
    ba->virtBase = p.virt_base;
    ba->physBase = p.phys_base;
    ba->memSize = p.mem_size;
    ba->topOfKernelData = ba_end;
    ba->Video.v_baseAddr = XNU_FB_BASE;
    ba->Video.v_display = 1;
    ba->Video.v_rowBytes = XNU_FB_STRIDE;
    ba->Video.v_width = XNU_FB_WIDTH;
    ba->Video.v_height = XNU_FB_HEIGHT;
    ba->Video.v_depth = 32;
    ba->machineType = 0;
    ba->deviceTreeP = adt_va; /* XNU's virtual address after it builds its own map. */
    ba->deviceTreeLength = (u32)p.adt_size;
    for (u32 i = 0; i < XNU_BOOT_LINE_LENGTH; i++) ba->CommandLine[i] = (char)p.cmdline[i];
    ba->bootFlags = 0;
    ba->memSizeActual = p.mem_size;
    __asm__ volatile("dsb sy" ::: "memory");
    logf("xnu: boot_args PA=%x top=%x ADT PA=%x VA=%x len=%x", ba_pa,
         ba_end, adt_pa, adt_va, p.adt_size);
    logf("xnu: rev=%d ver=%d phys=%x virt=%x mem=%x", (u64)ba->Revision,
         (u64)ba->Version, ba->physBase, ba->virtBase, ba->memSize);

    u64 entry_pa = p.phys_base + (p.entry_va - p.virt_base);
    if (entry_pa < p.phys_base || entry_pa >= p.phys_base + p.mem_size) {
        progress_fail("translated XNU entry is outside kernel RAM");
        return -42;
    }
    logf("handoff: XNU entry PA=%x x0=boot_args PA=%x; secondary CPUs OFF", entry_pa, ba_pa);
    progress_set(98, "XNU boot_args rev 2 and ADT ready");
    progress_set(99, "branching to XNU with MMU/caches off");
    transfer_to_payload(entry_pa, ba_pa);
}
#endif

#if !XNU_PAYLOAD
struct test_boot_args {
    u64 signature;             /* 0 */
    u64 version;               /* 8 */
    u64 dtb;                   /* 16 */
    u64 dtb_size;              /* 24 */
    u64 initrd_start;          /* 32 */
    u64 initrd_end;            /* 40 */
    u64 payload_start;         /* 48 */
    u64 payload_size;          /* 56 */
    u64 return_address;        /* 64 */
    u64 payload_magic;         /* 72: written by test payload */
    u64 payload_pc;            /* 80 */
    u64 payload_sctlr;         /* 88 */
    u64 payload_currentel;     /* 96 */
    u64 payload_x0;            /* 104 */
    u64 reserved;              /* 112 */
};
_Static_assert(__builtin_offsetof(struct test_boot_args, return_address) == 64,
               "test payload return-address offset mismatch");
_Static_assert(__builtin_offsetof(struct test_boot_args, payload_magic) == 72,
               "test payload result offset mismatch");
_Static_assert(__builtin_offsetof(struct test_boot_args, payload_x0) == 104,
               "test payload x0 offset mismatch");

static struct test_boot_args handoff_args __attribute__((aligned(16)));
static u64 boot_timer_freq;
#endif

static int range_in_ram(u64 a, u64 sz)
{
    if (!sz || a + sz < a) return 0;
    for (u32 k = 0; k < dinfo.nram; k++) {
        u64 rb = dinfo.ram[k].base, rs = dinfo.ram[k].size;
        if (rs > (u64)-1 - rb) continue;
        u64 re = rb + rs;
        if (a >= rb && a < re && sz <= re - a) return 1;
    }
    return 0;
}

#if !XNU_PAYLOAD
static int payload_destination_ok(u64 dst, u64 sz, u64 dtb, u32 dsz,
                                  u64 initrd_start, u64 initrd_end, u64 base)
{
    u64 image_sz = (u64)(_end - _start);
    u64 pool = (u64)__pgtbl_start;
    u64 pool_sz = (u64)(__pgtbl_end - __pgtbl_start);
    u64 sec = (u64)__seccpu_start;
    u64 sec_sz = (u64)(__seccpu_end - __seccpu_start);
    if (!range_in_ram(dst, sz)) return 0;
    if (!region_free(dst, sz, dtb, dsz, initrd_start, initrd_end)) return 0;
    if (overlaps(dst, sz, base, image_sz)) return 0;
    if (overlaps(dst, sz, pool, pool_sz)) return 0;
    if (overlaps(dst, sz, sec, sec_sz)) return 0;
    if (overlaps(dst, sz, RAMOOPS_BASE, RAMOOPS_SIZE)) return 0;
    return 1;
}
#endif

static __attribute__((noreturn)) void reset_after(u64 freq, u32 seconds)
{
    if (!freq) freq = 13000000;
    u64 t0 = cnt();
    while (cnt() - t0 < freq * seconds) __asm__ volatile("yield");
    register u64 r0 __asm__("x0") = 0x84000009UL; /* PSCI SYSTEM_RESET */
    __asm__ volatile("smc #0" : "+r"(r0) : : "x1", "x2", "x3", "memory", "cc");
    *(volatile u32 *)(WDT_BASE + 0x14) = 0x1209;
    for (;;) __asm__ volatile("wfe");
}

#if !XNU_PAYLOAD
static __attribute__((noreturn)) void payload_return(struct test_boot_args *a)
{
    logf("handoff: test payload returned to shim at EL=%x", RD(CurrentEL));
    if (!a || a != &handoff_args) {
        progress_fail("payload returned with an invalid boot_args pointer");
        reset_after(boot_timer_freq, 3);
    }
    logf("handoff: args=%x sig=%w x0=%x payload_pc=%x", (u64)a,
         a->signature, a->payload_x0, a->payload_pc);
    logf("handoff: payload_magic=%w sctlr=%x currentel=%x",
         a->payload_magic, a->payload_sctlr, a->payload_currentel);
    int ok = a->signature == BOOT_ARGS_MAGIC &&
             a->payload_magic == TEST_PAYLOAD_RESULT &&
             a->payload_x0 == (u64)a &&
             a->payload_pc >= a->payload_start &&
             a->payload_pc < a->payload_start + a->payload_size &&
             (a->payload_sctlr & (1UL | 4UL | 0x1000UL)) == 0 &&
             a->payload_currentel == 4 &&
             a->payload_start == TEST_PAYLOAD_BASE;
    if (!ok) {
        progress_fail("test payload did not confirm MMU-off x0=boot_args handoff");
        reset_after(boot_timer_freq, 3);
    }
    logf("handoff: PASS; payload executed above 4 GiB, MMU off, x0=boot_args");
    progress_set(100, "payload execution and handoff ABI verified");
    reset_after(boot_timer_freq, 3);
}
#endif

static int loader_handoff(const u8 *d, u32 dsz, u64 base)
{
    u32 ls = 0, le = 0;
    const u8 *vs = fdt_prop(d, "chosen", "linux,initrd-start", &ls);
    const u8 *ve = fdt_prop(d, "chosen", "linux,initrd-end", &le);
    if (!vs || !ve) {
        progress_fail("DTB does not provide initrd bounds");
        return -20;
    }
    u64 is = be_val(vs, ls), ie = be_val(ve, le);
    if (ie <= is || ie - is > 0x10000000UL || !range_in_ram(is, ie - is)) {
        logf("loader: invalid initrd range %x..%x", is, ie);
        progress_fail("initrd range is invalid or outside RAM");
        return -21;
    }
    logf("loader: initrd %x..%x bytes=%x", is, ie, ie - is);
    progress_set(80, "initrd bounds located and validated");

    const loader_u8 *file = 0;
    u32 file_size = 0;
#if XNU_PAYLOAD
    int rc = loader_cpio_find_file((const loader_u8 *)is, ie - is,
                                   XNU_PAYLOAD_NAME, &file, &file_size);
    if (rc || !file) {
        logf("xnu: CPIO payload search failed rc=%d", (u64)(-rc));
        progress_fail("XNU payload is absent from the live initrd");
        return -22;
    }
    logf("xnu: CPIO file %s at %x size=%d", XNU_PAYLOAD_NAME, (u64)file, (u64)file_size);
    progress_set(84, "SWEETXNU payload found in raw CPIO");
    return loader_handoff_xnu(d, dsz, base, is, ie, file, file_size);
#else
    int rc = loader_cpio_find_file((const loader_u8 *)is, ie - is,
                                   TEST_PAYLOAD_NAME, &file, &file_size);
    if (rc || !file) {
        logf("loader: CPIO payload search failed rc=%d", (u64)(-rc));
        progress_fail("test payload is absent from the live initrd");
        return -22;
    }
    logf("loader: CPIO file %s at %x size=%d", TEST_PAYLOAD_NAME, (u64)file, (u64)file_size);
    progress_set(84, "test payload found in ramdisk CPIO");

    const loader_u8 *payload = 0;
    u32 payload_size = 0, expected_crc = 0;
    rc = loader_parse_payload((const loader_u8 *)file, file_size,
                              &payload, &payload_size, &expected_crc);
    if (rc) {
        logf("loader: payload header/CRC rejected rc=%d", (u64)(-rc));
        progress_fail("payload header, size, or CRC32 is invalid");
        return -23;
    }
    u32 found_crc = loader_crc32(payload, payload_size);
    logf("loader: payload bytes=%d crc expected=%x actual=%x", (u64)payload_size,
         (u64)expected_crc, (u64)found_crc);
    progress_set(88, "payload header and CRC32 validated");

    u64 dst = TEST_PAYLOAD_BASE;
    if (!payload_destination_ok(dst, payload_size, (u64)d, dsz, is, ie, base)) {
        logf("loader: target range rejected base=%x size=%x", dst, (u64)payload_size);
        progress_fail("high-RAM target overlaps a reserved or live region");
        return -26;
    }
    u64 pa0 = 0, pa1 = 0;
    if (!mm_translate(&mtab, dst, &pa0, 0) || pa0 != dst ||
        !mm_translate(&mtab, dst + payload_size - 1, &pa1, 0) || pa1 != dst + payload_size - 1) {
        progress_fail("target payload pages are not identity-mapped");
        return -27;
    }
    logf("loader: target validated in DRAM rank 1 at %x", dst);
    progress_set(91, "high-RAM destination validated");

    memcpy((void *)dst, payload, payload_size);
    __asm__ volatile("dsb sy" ::: "memory");
    u32 copied_crc = loader_crc32((const loader_u8 *)dst, payload_size);
    logf("loader: copy dst=%x size=%d crc=%x", dst, (u64)payload_size, (u64)copied_crc);
    if (copied_crc != expected_crc) {
        progress_fail("payload copy verification failed");
        return -28;
    }
    progress_set(94, "payload copied above 4 GiB and read-back verified");

    memset(&handoff_args, 0, sizeof handoff_args);
    handoff_args.signature = BOOT_ARGS_MAGIC;
    handoff_args.version = 1;
    handoff_args.dtb = (u64)d;
    handoff_args.dtb_size = dsz;
    handoff_args.initrd_start = is;
    handoff_args.initrd_end = ie;
    handoff_args.payload_start = dst;
    handoff_args.payload_size = payload_size;
    handoff_args.return_address = (u64)payload_return;
    logf("loader: boot_args=%x sig=%w dtb=%x initrd=%x..%x payload=%x+%x return=%x",
         (u64)&handoff_args, handoff_args.signature, handoff_args.dtb,
         is, ie, dst, (u64)payload_size, handoff_args.return_address);
    progress_set(97, "boot_args assembled with DTB/initrd/return entry");
    logf("handoff: secondary CPUs left OFF; disabling MMU/caches before branch");
    progress_set(99, "entering copied test payload with x0=boot_args");
    transfer_to_payload(dst, (u64)&handoff_args);
#endif
}
#endif

void shim_main(u64 dtb, u64 x1, u64 x2, u64 x3, u64 base)
{
    exception_depth = 0;
    progress_percent = 0;
    progress_failed = 0;
    ramoops_init();
    progress_set(1, "persistent ramoops console initialized");
#if TEST_EXCEPTION_VECTORS
    /* Deliberate synchronous exception used only to validate VBAR_EL1. */
    __asm__ volatile("udf #0");
#endif
    u64 el = (RD(CurrentEL) >> 2) & 3;
    u64 freq = RD(cntfrq_el0);
    if (!freq) freq = 13000000;
#ifndef EMULATOR_COMPAT
#if !XNU_PAYLOAD
    boot_timer_freq = freq;
#endif
#endif

#ifdef EMULATOR_COMPAT
    /* The Unicorn harness is not a secure monitor: emulate PSCI query values. */
    u64 psci_ver = 0x00010001UL;
    u64 psci_cpu_on_features = 0;
#else
    register u64 psci_ver __asm__("x0") = 0x84000000UL;
    __asm__ volatile("smc #0" : "+r"(psci_ver) : : "x1", "x2", "x3", "memory", "cc");
    register u64 psci_features_fid __asm__("x0") = 0x8400000AUL;
    register u64 psci_cpu_on_fid __asm__("x1") = 0xC4000003UL;
    __asm__ volatile("smc #0" : "+r"(psci_features_fid), "+r"(psci_cpu_on_fid)
                     : : "x2", "x3", "memory", "cc");
    u64 psci_cpu_on_features = psci_features_fid;
#endif

    const u8 *d = (const u8 *)dtb;
    u32 magic = 0, size = 0;
    if (dtb) { magic = be32(d); size = be32(d + 4); }
    int dtb_ok = (magic == 0xd00dfeed && size >= 40 && size < 0x800000);

    logf("ios-kl7 SHIM v0.8.0");
    logf("entry: EL=%d MPIDR=%x MIDR=%w", el, RD(mpidr_el1), RD(midr_el1));
    logf("cpu: PFR0=%x MMFR0=%x ISAR0=%x ISAR1=%x", RD(id_aa64pfr0_el1),
         RD(id_aa64mmfr0_el1), RD(id_aa64isar0_el1), RD(id_aa64isar1_el1));
    logf("timer: CNTFRQ=%d", freq);
    logf("psci: version=%w CPU_ON_FEATURES=%w", psci_ver, psci_cpu_on_features);
    logf("entry: base=%x image_size=%x", base, (u64)(_end - _start));
    if (el == 2) logf("entry: SCTLR_EL2=%x HCR=%x", RD(sctlr_el2), RD(hcr_el2));
    else         logf("entry: SCTLR_EL1=%x", RD(sctlr_el1));
    logf("entry: VBAR_EL1=%w x0(DTB)=%x x1=%x x2=%x x3=%x", RD(vbar_el1), dtb, x1, x2, x3);
    logf("dtb: magic=%w size=%x valid=%d", (u64)magic, (u64)size, (u64)dtb_ok);
    progress_set(7, "entry registers and PSCI queries recorded");

    logf("stage: DTB/boot environment begin");
    if (dtb_ok) {
        find_kv(d, size, "bootopt=", 8, "BOOTOPT");
        u32 l = 0;
        const u8 *v;
        if ((v = fdt_prop(d, "chosen", "linux,initrd-start", &l)))
            logf("initrd: start=%x", be_val(v, l));
        if ((v = fdt_prop(d, "chosen", "linux,initrd-end", &l)))
            logf("initrd: end=%x", be_val(v, l));
        if ((v = fdt_prop(d, "memory", "reg", &l))) {
            logf("memory: reg_len=%d", (u64)l);
            if (l >= 16) logf("memory: %x +%x", be_val(v, 8), be_val(v + 8, 8));
        }
        logf("dtb: address=%x end=%x", dtb, dtb + size);
    } else {
        logf("dtb: BAD header or size");
    }
    progress_set(11, "DTB header and boot memory properties checked");

    logf("stage: bootconfig begin");
    if (dtb_ok) {
        u32 l1 = 0, l2 = 0;
        const u8 *vs = fdt_prop(d, "chosen", "linux,initrd-start", &l1);
        const u8 *ve = fdt_prop(d, "chosen", "linux,initrd-end", &l2);
        u64 is = vs ? be_val(vs, l1) : 0, ie = ve ? be_val(ve, l2) : 0;
        if (is && ie > is + 32 && ie - is < 0x10000000UL) {
            const u8 *t = (const u8 *)ie;
            if (str_eq_n(t - 12, "#BOOTCONFIG\n", 12)) {
                u32 sz = (u32)t[-20] | ((u32)t[-19] << 8) |
                         ((u32)t[-18] << 16) | ((u32)t[-17] << 24);
                if (sz < 0x4000 && (u64)sz + 20 <= ie - is)
                    log_text_blob("bootconfig", t - 20 - sz, sz);
                else logf("bootconfig: size rejected (%x)", (u64)sz);
            } else logf("bootconfig: no #BOOTCONFIG tail");
        } else logf("bootconfig: initrd absent or invalid");
    }
    progress_set(15, "bootconfig inspected and streamed to ramoops");

    logf("stage: bootargs begin");
    if (dtb_ok) {
        u32 l = 0;
        const u8 *v = fdt_prop(d, "chosen", "bootargs", &l);
        if (v) log_text_blob("bootargs", v, l);
        else logf("bootargs: property absent");
    }
    progress_set(20, "bootargs streamed to ramoops");

#ifdef EMULATOR_COMPAT
    /* This build validates the bottom-strip UI and log flow only. */
    __asm__ volatile("brk #0");
    for (;;) __asm__ volatile("wfe");
#else
#if MMU_STAGE
    if (!dtb_ok) {
        progress_fail("invalid DTB; refusing MMU setup and payload load");
        reset_after(freq, 3);
    }
    int mmrc = mmu_stage(d, size, base, freq);
    if (mmrc) reset_after(freq, 3);
#if LOADER_HANDOFF
    int hrc = loader_handoff(d, size, base);
    if (hrc) reset_after(freq, 3);
    progress_fail("unexpected return from loader_handoff");
    reset_after(freq, 3);
#else
    progress_set(90, "MMU test complete; loader handoff disabled");
    reset_after(freq, 3);
#endif
#else
    progress_fail("MMU_STAGE=0; loader handoff cannot run");
    reset_after(freq, 3);
#endif
#endif
}
