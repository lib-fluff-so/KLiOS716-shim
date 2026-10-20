/* ios-kl7 diagnostic shim: framebuffer + register/DTB diagnostics + reset.
 * v0.5 adds EL1 exception diagnostics. Ramoops is intentionally not written yet.
 * Runs with MMU off: no libc and no SIMD; use aligned accesses only. */
typedef unsigned char u8;
typedef unsigned int u32;
typedef unsigned long u64;
typedef __builtin_va_list va_list;
#define va_start(a, b) __builtin_va_start(a, b)
#define va_end(a)      __builtin_va_end(a)
#define va_arg(a, t)   __builtin_va_arg(a, t)

#include "font.h"

#ifndef TEST_EXCEPTION_VECTORS
#define TEST_EXCEPTION_VECTORS 0
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

static void fmt_u64(char **o, u64 v, int base, int minw)
{
    char tmp[24]; int n = 0;
    do { int d = v % base; tmp[n++] = d < 10 ? '0' + d : 'a' + d - 10; v /= base; } while (v);
    while (n < minw) tmp[n++] = '0';
    while (n) *(*o)++ = tmp[--n];
}

static void logf(const char *f, ...)
{
    if (nlines >= MAX_LINES) return;
    char *o = lines[nlines++];
    char *start = o;
    va_list ap; va_start(ap, f);
    for (; *f && (o - start) < 40; f++) {
        if (*f != '%') { *o++ = *f; continue; }
        f++;
        if (*f == 'x') fmt_u64(&o, va_arg(ap, u64), 16, 1);
        else if (*f == 'w') fmt_u64(&o, va_arg(ap, u64), 16, 8);
        else if (*f == 'd') fmt_u64(&o, va_arg(ap, u64), 10, 1);
        else if (*f == 's') { const char *s = va_arg(ap, const char *); while (*s && (o - start) < MAX_COLS) *o++ = *s++; }
    }
    va_end(ap);
    *o = 0;
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
    logf("KL7 EXCEPTION v0.5");
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
    logf("HALTED - no reset");

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

void shim_main(u64 dtb, u64 x1, u64 x2, u64 x3, u64 base)
{
    exception_depth = 0;
#if TEST_EXCEPTION_VECTORS
    /* Deliberate synchronous exception used only to validate VBAR_EL1. */
    __asm__ volatile("udf #0");
#endif
    u64 el = (RD(CurrentEL) >> 2) & 3;
    u64 freq = RD(cntfrq_el0);
    if (!freq) freq = 13000000;

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

    const u8 *d = (const u8 *)dtb;
    u32 magic = 0, size = 0;
    if (dtb) { magic = be32(d); size = be32(d + 4); }
    int dtb_ok = (magic == 0xd00dfeed && size < 0x800000);

    /* page 1: CPU / entry state */
    logf("KL7 SHIM v0.5  1/4");
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
    logf("DTB INFO  2/4");
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
    logf("BOOTCONFIG  3/4");
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
    logf("BOOTARGS  4/4");
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

    /* PSCI SYSTEM_RESET (smc conduit per DTB), then MTK TOPRGU software reset as fallback */
    register u64 r0 __asm__("x0") = 0x84000009UL;
    __asm__ volatile("smc #0" : "+r"(r0) : : "x1", "x2", "x3", "memory");
    *(volatile u32 *)(WDT_BASE + 0x14) = 0x1209;
    for (;;) __asm__ volatile("wfe");
}
