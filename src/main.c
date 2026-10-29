/* ios-kl7 v0.10.0: minimal payload loader with a framebuffer console and ramoops.
 * Entry: EL1, MMU/caches off, x0 = live Linux DTB. No shim page tables,
 * no CPU_ON, no GIC/timer probes. Prepare payload, boot_args and ADT, then jump. */
typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long u64;
typedef __builtin_va_list va_list;
#define va_start(a, b) __builtin_va_start(a, b)
#define va_end(a)      __builtin_va_end(a)
#define va_arg(a, t)   __builtin_va_arg(a, t)

#include "console.h"
#include "memmap.h"
#include "loader.h"
#include "xnu_payload.h"
#include "ufs_test.h"
#include "touch_test.h"
#include "battery_test.h"
#include "gpu_test.h"

#ifndef RAMOOPS_LOG
#define RAMOOPS_LOG 1
#endif
#ifndef XNU_PAYLOAD
#define XNU_PAYLOAD 0
#endif
#ifndef UFS_TEST
#define UFS_TEST 0
#endif
#ifndef TOUCH_TEST
#define TOUCH_TEST 0
#endif
#ifndef BATTERY_TEST
#define BATTERY_TEST 0
#endif
#ifndef GPU_TEST
#define GPU_TEST 0
#endif
#ifndef CHARGER_WRITE_TEST
#define CHARGER_WRITE_TEST 0
#endif
#ifndef AUTO_REBOOT_TEST
#if XNU_PAYLOAD
#define AUTO_REBOOT_TEST 0
#else
#define AUTO_REBOOT_TEST 1
#endif
#endif
#ifndef PSCI_RESET_TIMER
#define PSCI_RESET_TIMER 1
#endif

#define FB_BASE               0xFDFFF000UL
#define FB_STRIDE             4352u
#define FB_HEIGHT             2436u
#define FB_VISIBLE_OFFSET     0UL
#define XNU_FB_BASE           FB_BASE
#define XNU_FB_STRIDE         FB_STRIDE
#define XNU_FB_WIDTH          1080u
#define XNU_FB_HEIGHT         FB_HEIGHT
#define XNU_EXPECTED_PHYS_BASE 0x140000000UL
#define XNU_EXPECTED_MEM_SIZE  0xFFEFF000UL
#define RAMOOPS_BASE          0x48090000UL
#define RAMOOPS_SIZE          0x000E0000UL
#define RAMOOPS_CONSOLE_BASE  0x480EF000UL
#define RAMOOPS_CONSOLE_SIZE  0x00040000UL
#define PERSISTENT_RAM_SIG    0x43474244u
#define TEST_PAYLOAD_BASE     0x140000000UL
#define TEST_PAYLOAD_NAME     "ios-kl7-test.bin"
#define XNU_PAYLOAD_NAME      "ios-kl7-xnu.bin"
#define TEST_PAYLOAD_RESULT   0x3054534554374c4bUL
#define BOOT_ARGS_MAGIC       0x30544f4f42374c4bUL
#define FDT_MAGIC             0xd00dfeedu
#define MAX_DTB_SIZE          0x800000u
#define MAX_INITRD_SIZE       0x10000000UL

/* MT6789 TOPRGU watchdog (DTB: watchdog@10007000, mediatek,mt6589-wdt layout).
 * Register layout per Linux drivers/watchdog/mtk_wdt.c; verify the readback in
 * ramoops on the device. WDT_MODE writes need the 0x22 key in bits 31:24. */
#define WDT_BASE              0x10007000UL
#define WDT_MODE_REG          0x00u
#define WDT_LENGTH_REG        0x04u
#define WDT_MODE_KEY          0x22000000u
#define WDT_MODE_EN           0x1u
#define WDT_RESTART_REG       0x08u
#define WDT_RESTART_KEY       0x1971u
/* WDT_TIMEOUT_AT_HANDOFF: arm (rather than disable) the watchdog right before
 * jumping to XNU, as a last-resort backstop. LENGTH ticks are 1/2048 s:
 * 0x5000 = 10 s, 0xf000 = 30 s (the MT6789 reads back 0xf000, i.e. the write
 * is ignored, so the real reset always comes from the optional PSCI timer). */
#ifndef WDT_TIMEOUT_AT_HANDOFF
/* WDT is disabled at handoff. With PSCI_RESET_TIMER=1 the timer on cpu1 is the
 * only reset source; with it disabled there is no automatic reset at all.
 * A watchdog reset loses DRAM/ramoops, so it must never fire first. */
#define WDT_TIMEOUT_AT_HANDOFF 0u
#endif
/* WDT_DISABLE_AT_HANDOFF=1: stop the watchdog before a long-running path.
 * Keep it enabled for GPU_TEST when PSCI_RESET_TIMER=0: the GPU can hang in an
 * MMIO/job wait, and the hardware WDT is then the only reset backstop. If the
 * PSCI timer is enabled, it owns the clean warm reset and preserves ramoops. */
#ifndef WDT_DISABLE_AT_HANDOFF
#define WDT_DISABLE_AT_HANDOFF (XNU_PAYLOAD || BATTERY_TEST || (GPU_TEST && PSCI_RESET_TIMER))
#endif

#define RD(reg) ({ u64 v_; __asm__ volatile("mrs %0, " #reg : "=r"(v_)); v_; })

void *memset(void *d, int c, unsigned long n)
{
    u8 *p = (u8 *)d;
    while (n--) *p++ = (u8)c;
    return d;
}

void *memcpy(void *d, const void *s, unsigned long n)
{
    u8 *p = (u8 *)d;
    const u8 *q = (const u8 *)s;
    while (n--) *p++ = *q++;
    return d;
}

/* The Linux 5.10 persistent_ram_buffer used by KL7's configured ramoops
 * console: signature, next-write position, valid byte count, then ring data. */
#if RAMOOPS_LOG
struct persistent_ram_buffer {
    volatile u32 sig;
    volatile u32 start;
    volatile u32 size;
    volatile u8 data[];
};
_Static_assert(sizeof(struct persistent_ram_buffer) == 12,
               "persistent_ram_buffer ABI changed");
#define RAMOOPS_CAPACITY (RAMOOPS_CONSOLE_SIZE - sizeof(struct persistent_ram_buffer))
static u32 ramoops_pos;
static u32 ramoops_size;
static volatile u32 ramoops_ready;

static void ramoops_barrier(void)
{
    __asm__ volatile("dsb sy" ::: "memory");
}

static void ramoops_write(const char *s, u32 n)
{
    if (!ramoops_ready || !s || !n) return;
    volatile struct persistent_ram_buffer *b =
        (volatile struct persistent_ram_buffer *)RAMOOPS_CONSOLE_BASE;
    u32 pos = ramoops_pos;
    u32 used = ramoops_size;
    for (u32 i = 0; i < n; i++) {
        b->data[pos++] = (u8)s[i];
        if (pos == RAMOOPS_CAPACITY) pos = 0;
        if (used < RAMOOPS_CAPACITY) used++;
    }
    ramoops_barrier();
    b->size = used;
    ramoops_barrier();
    b->start = pos;
    ramoops_barrier();
    ramoops_pos = pos;
    ramoops_size = used;
}

static void ramoops_init(void)
{
    volatile struct persistent_ram_buffer *b =
        (volatile struct persistent_ram_buffer *)RAMOOPS_CONSOLE_BASE;

    /* Warm B-slot retries must not erase the previous attempt's log. The
     * persistent_ram header stores the next write position in start and the
     * number of valid bytes in size. Keep a plausible existing ring and append
     * a separator; initialize only when the header is not ours/valid. */
    u32 old_sig = b->sig;
    u32 old_pos = b->start;
    u32 old_size = b->size;
    if (old_sig == PERSISTENT_RAM_SIG &&
        old_pos < RAMOOPS_CAPACITY && old_size <= RAMOOPS_CAPACITY) {
        ramoops_pos = old_pos;
        ramoops_size = old_size;
        ramoops_ready = 1;
        static const char separator[] = "\n--- ios-kl7 shim boot ---\n";
        ramoops_write(separator, (u32)(sizeof(separator) - 1u));
        return;
    }

    b->sig = 0;
    ramoops_barrier();
    b->start = 0;
    b->size = 0;
    ramoops_pos = 0;
    ramoops_size = 0;
    ramoops_barrier();
    b->sig = PERSISTENT_RAM_SIG;
    ramoops_barrier();
    ramoops_ready = 1;
}
#else
static void ramoops_init(void) { }
static void ramoops_write(const char *s, u32 n) { (void)s; (void)n; }
#endif

static void fmt_u64(char **out, char *end, u64 value, u32 base, u32 width)
{
    char tmp[24];
    u32 n = 0;
    do {
        u32 d = (u32)(value % base);
        tmp[n++] = (char)(d < 10 ? '0' + d : 'a' + d - 10);
        value /= base;
    } while (value && n < sizeof(tmp));
    while (n < width && n < sizeof(tmp)) tmp[n++] = '0';
    while (n && *out < end) *(*out)++ = tmp[--n];
}

static int format_line(char *out, int cap, const char *fmt, va_list ap)
{
    char *begin = out;
    if (cap <= 0) return 0;
    char *end = out + cap - 1;
    for (; *fmt && out < end; fmt++) {
        if (*fmt != '%') {
            *out++ = *fmt;
            continue;
        }
        fmt++;
        if (!*fmt) break;
        if (*fmt == 'x') fmt_u64(&out, end, va_arg(ap, u64), 16, 1);
        else if (*fmt == 'w') fmt_u64(&out, end, va_arg(ap, u64), 16, 16);
        else if (*fmt == 'd') fmt_u64(&out, end, va_arg(ap, u64), 10, 1);
        else if (*fmt == 's') {
            const char *s = va_arg(ap, const char *);
            if (!s) s = "(null)";
            while (*s && out < end) *out++ = *s++;
        } else {
            *out++ = *fmt;
        }
    }
    *out = 0;
    return (int)(out - begin);
}

void logf(const char *fmt, ...)
{
    char line[160];
    va_list ap;
    va_start(ap, fmt);
    int n = format_line(line, 150, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    line[n++] = '\n';
    ramoops_write(line, (u32)n);
    console_write(line, (unsigned long)n);
}

static void wdt_report(const char *when)
{
    volatile u32 *w = (volatile u32 *)WDT_BASE;
    u32 mode = w[WDT_MODE_REG / 4];
    u32 length = w[WDT_LENGTH_REG / 4];
    logf("wdt(%s): MODE=%x LENGTH=%x enabled=%d", when, (u64)mode, (u64)length,
         (u64)(mode & WDT_MODE_EN));
}

static __attribute__((unused)) void wdt_arm(u32 ticks)
{
    volatile u32 *w = (volatile u32 *)WDT_BASE;
    u32 mode = w[WDT_MODE_REG / 4] & 0x00ffffffu;
    w[WDT_MODE_REG / 4] = WDT_MODE_KEY | (mode | WDT_MODE_EN);
    w[WDT_LENGTH_REG / 4] = ticks;
    w[WDT_RESTART_REG / 4] = WDT_RESTART_KEY;
    wdt_report("armed");
}

#if PSCI_RESET_TIMER
/* PSCI 1.0 (DT /psci method = "smc"). Start a secondary core in a tiny timer
 * loop that issues SYSTEM_RESET after a delay. A PSCI reset is clean: it keeps
 * DRAM (and therefore the ramoops/pstore ring) across the reboot, unlike a
 * watchdog reset, which resets the SoC without a cache/DRAM handshake. */
#define PSCI_CPU_ON          0xC4000003UL
#define PSCI_RESET_CPU_MPIDR 0x100UL   /* cpu1: Aff1 = 1 */
#define PSCI_RESET_DELAY_S   50u

static u64 psci_smc(u64 fn, u64 a1, u64 a2, u64 a3)
{
    register u64 x0 __asm__("x0") = fn;
    register u64 x1 __asm__("x1") = a1;
    register u64 x2 __asm__("x2") = a2;
    register u64 x3 __asm__("x3") = a3;
    __asm__ volatile("smc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3)
                     : "memory");
    return x0;
}

/* Runs on the secondary core: MMU/caches off, no stack, no memory accesses. */
static __attribute__((naked, noreturn)) void psci_reset_timer(void)
{
    __asm__ volatile(
        "mrs x0, cntfrq_el0\n"
        "mov x2, #50\n"                     /* PSCI_RESET_DELAY_S */
        "mul x2, x0, x2\n"
        "mrs x1, cntpct_el0\n"
        "add x2, x2, x1\n"
        "1: mrs x3, cntpct_el0\n"
        "cmp x3, x2\n"
        "b.lo 1b\n"
        "movz x0, #0x0009\n"
        "movk x0, #0x8400, lsl #16\n"
        "mov x1, xzr\n"
        "mov x2, xzr\n"
        "mov x3, xzr\n"
        "smc #0\n"
        "b 1b\n");
}

static __attribute__((unused)) void psci_start_reset_timer(void)
{
    u64 entry = (u64)(unsigned long)&psci_reset_timer;
    u64 rc = psci_smc(PSCI_CPU_ON, PSCI_RESET_CPU_MPIDR, entry, 0);
    logf("psci cpu_on(mpidr=%x entry=%x) -> %x", PSCI_RESET_CPU_MPIDR,
         entry, rc);
}
#endif /* PSCI_RESET_TIMER */

#if WDT_DISABLE_AT_HANDOFF
static __attribute__((unused)) void wdt_disable(void)
{
    volatile u32 *w = (volatile u32 *)WDT_BASE;
    u32 mode = w[WDT_MODE_REG / 4] & 0x00ffffffu;
    w[WDT_MODE_REG / 4] = WDT_MODE_KEY | (mode & ~WDT_MODE_EN);
    __asm__ volatile("dsb sy" ::: "memory");
    wdt_report("disabled");
}
#endif

#if AUTO_REBOOT_TEST
/* The supplied KL7 DTB declares /psci method = "smc". PSCI SYSTEM_RESET is
 * function ID 0x84000009. On a conforming implementation this call does not
 * return; if firmware rejects it, report the returned status and park. */
static __attribute__((noreturn)) void psci_system_reset(void)
{
    u64 status;
    __asm__ volatile(
        "dsb sy\n"
        "isb\n"
        "movz x0, #0x0009\n"
        "movk x0, #0x8400, lsl #16\n"
        "mov x1, xzr\n"
        "mov x2, xzr\n"
        "mov x3, xzr\n"
        "smc #0\n"
        "mov %0, x0\n"
        : "=r"(status)
        :
        : "x0", "x1", "x2", "x3", "memory");
    logf("ERROR: PSCI SYSTEM_RESET returned x0=%x", status);
    for (;;) __asm__ volatile("wfe");
}
#endif /* AUTO_REBOOT_TEST */

#if AUTO_REBOOT_TEST || UFS_TEST || !XNU_PAYLOAD
static __attribute__((noreturn)) void finish_test_attempt(const char *reason)
{
#if AUTO_REBOOT_TEST
    logf("REBOOT: %s; PSCI SYSTEM_RESET", reason);
    psci_system_reset();
#else
    logf("HALT: %s", reason);
    for (;;) __asm__ volatile("wfe");
#endif
}
#endif

static int be32_checked(const u8 *p, u32 *out)
{
    if (!p || !out) return 0;
    *out = ((u32)p[0] << 24) | ((u32)p[1] << 16) |
           ((u32)p[2] << 8) | (u32)p[3];
    return 1;
}

static u64 be_value(const u8 *p, u32 len)
{
    u64 v = 0;
    if (!p || (len != 4u && len != 8u)) return 0;
    for (u32 i = 0; i < len; i++) v = (v << 8) | p[i];
    return v;
}

static int dtb_header(const u8 *dtb, u32 *total_out)
{
    if (!dtb || !total_out) return 0;
    u32 magic, total, off_struct, off_strings, off_rsv, size_strings, size_struct;
    be32_checked(dtb, &magic);
    be32_checked(dtb + 4, &total);
    be32_checked(dtb + 8, &off_struct);
    be32_checked(dtb + 12, &off_strings);
    be32_checked(dtb + 16, &off_rsv);
    be32_checked(dtb + 32, &size_strings);
    be32_checked(dtb + 36, &size_struct);
    if (magic != FDT_MAGIC || total < 40u || total > MAX_DTB_SIZE) return 0;
    if (off_struct > total || size_struct > total - off_struct) return 0;
    if (off_strings > total || size_strings > total - off_strings) return 0;
    if (off_rsv < 40u || off_rsv > total || (off_rsv & 7u)) return 0;
    *total_out = total;
    return 1;
}

/* Return a property's value from a direct child of the FDT root. The header
 * and all block bounds are validated before this parser is called. */
static const u8 *fdt_child_prop(const u8 *dtb, u32 total,
                                const char *child, const char *property,
                                u32 *length)
{
    u32 off_struct = ((u32)dtb[8] << 24) | ((u32)dtb[9] << 16) |
                     ((u32)dtb[10] << 8) | dtb[11];
    u32 off_strings = ((u32)dtb[12] << 24) | ((u32)dtb[13] << 16) |
                      ((u32)dtb[14] << 8) | dtb[15];
    u32 size_strings = ((u32)dtb[32] << 24) | ((u32)dtb[33] << 16) |
                       ((u32)dtb[34] << 8) | dtb[35];
    u32 size_struct = ((u32)dtb[36] << 24) | ((u32)dtb[37] << 16) |
                      ((u32)dtb[38] << 8) | dtb[39];
    const u8 *p = dtb + off_struct;
    const u8 *end = p + size_struct;
    const u8 *strings = dtb + off_strings;
    const u8 *strings_end = strings + size_strings;
    int depth = 0;
    int selected = 0;

    while (p <= end && (u64)(end - p) >= 4u) {
        u32 token;
        be32_checked(p, &token);
        p += 4;
        if (token == 1u) {
            const u8 *nul = p;
            while (nul < end && *nul) nul++;
            if (nul == end) return 0;
            u32 n = (u32)(nul - p);
            u64 step = ((u64)n + 4u) & ~(u64)3u;
            if (step > (u64)(end - p)) return 0;
            const u8 *name = p;
            p += step;
            depth++;
            if (depth == 2) {
                u32 i = 0;
                while (child[i] && i < n && name[i] == (u8)child[i]) i++;
                selected = !child[i] && (i == n || name[i] == '@');
            }
        } else if (token == 2u) {
            if (depth <= 0) return 0;
            if (depth == 2) selected = 0;
            depth--;
        } else if (token == 3u) {
            if ((u64)(end - p) < 8u) return 0;
            u32 len, nameoff;
            be32_checked(p, &len);
            be32_checked(p + 4, &nameoff);
            p += 8;
            if ((u64)len > (u64)(end - p)) return 0;
            const u8 *value = p;
            u64 padded = ((u64)len + 3u) & ~(u64)3u;
            if (padded > (u64)(end - p) || nameoff >= size_strings) return 0;
            p += padded;
            const u8 *name = strings + nameoff;
            const u8 *name_end = name;
            while (name_end < strings_end && *name_end) name_end++;
            if (name_end == strings_end) return 0;
            u32 prop_len = (u32)(name_end - name);
            u32 i = 0;
            while (property[i] && i < prop_len && name[i] == (u8)property[i]) i++;
            if (selected && depth == 2 && !property[i] && i == prop_len) {
                if (length) *length = len;
                return value;
            }
        } else if (token == 4u) {
            continue;
        } else if (token == 9u) {
            break;
        } else {
            return 0;
        }
    }
    (void)total;
    return 0;
}

static struct memmap_info memory_map;
extern char _start[], _end[];
extern void transfer_to_payload(u64 target, u64 boot_args) __attribute__((noreturn));

static int range_in_ram(u64 base, u64 size)
{
    if (!size || size > (u64)-1 - base) return 0;
    for (u32 i = 0; i < memory_map.nram; i++) {
        u64 rb = memory_map.ram[i].base;
        u64 rs = memory_map.ram[i].size;
        if (rs <= (u64)-1 - rb && base >= rb && base < rb + rs && size <= rb + rs - base)
            return 1;
    }
    return 0;
}

static int overlaps(u64 a, u64 as, u64 b, u64 bs)
{
    if (!as || !bs) return 0;
    if (as > (u64)-1 - a || bs > (u64)-1 - b) return 1;
    return a < b + bs && b < a + as;
}

static int region_is_free(u64 base, u64 size, u64 dtb, u32 dtb_size,
                          u64 initrd_start, u64 initrd_end, u64 shim_base)
{
    if (!range_in_ram(base, size)) return 0;
    if (overlaps(base, size, dtb, dtb_size)) return 0;
    if (initrd_end > initrd_start && overlaps(base, size, initrd_start, initrd_end - initrd_start)) return 0;
    if (overlaps(base, size, shim_base, (u64)(_end - _start))) return 0;
    if (overlaps(base, size, RAMOOPS_BASE, RAMOOPS_SIZE)) return 0;
    for (u32 i = 0; i < memory_map.nreserved; i++)
        if (overlaps(base, size, memory_map.reserved[i].base, memory_map.reserved[i].size)) return 0;
    for (u32 i = 0; i < memory_map.nfdt_reserved; i++)
        if (overlaps(base, size, memory_map.fdt_reserved[i].base, memory_map.fdt_reserved[i].size)) return 0;
    for (u32 i = 0; i < memory_map.nnomap; i++)
        if (overlaps(base, size, memory_map.nomap[i].base, memory_map.nomap[i].size)) return 0;
    return 1;
}

#if XNU_PAYLOAD
static u64 align_up(u64 value, u64 alignment)
{
    if (!alignment || value > (u64)-1 - (alignment - 1u)) return 0;
    return (value + alignment - 1u) & ~(alignment - 1u);
}

static void copy_words(u64 dst, const u8 *src, u64 size)
{
    volatile u64 *d = (volatile u64 *)dst;
    const volatile u64 *s = (const volatile u64 *)src;
    for (u64 i = 0; i < (size + 7u) / 8u; i++) d[i] = s[i];
}

static void zero_words(u64 dst, u64 size)
{
    volatile u64 *d = (volatile u64 *)dst;
    for (u64 i = 0; i < size / 8u; i++) d[i] = 0;
}
#endif

__attribute__((noreturn)) void exception_c_entry(u64 esr, u64 far, u64 elr, u64 spsr)
{
    __asm__ volatile("msr daifset, #0xf" ::: "memory");
    logf("PANIC: exception in shim");
    logf("ESR=%w FAR=%w", esr, far);
    logf("ELR=%w SPSR=%w", elr, spsr);
#if AUTO_REBOOT_TEST
    finish_test_attempt("shim exception");
#else
    logf("HALT: display + ramoops contain panic");
    for (;;) __asm__ volatile("wfe");
#endif
}

static __attribute__((noreturn)) void fail(const char *reason, int rc)
{
    logf("ERROR: %s (rc=%d)", reason, (u64)(rc < 0 ? -rc : rc));
#if AUTO_REBOOT_TEST
    finish_test_attempt("shim error; ramoops preserved");
#else
    logf("STOP: no payload handoff");
    for (;;) __asm__ volatile("wfe");
#endif
}

static int get_initrd(const u8 *dtb, u32 dtb_size, u64 *start, u64 *end)
{
    u32 ls = 0, le = 0;
    const u8 *vs = fdt_child_prop(dtb, dtb_size, "chosen", "linux,initrd-start", &ls);
    const u8 *ve = fdt_child_prop(dtb, dtb_size, "chosen", "linux,initrd-end", &le);
    if (!vs || !ve || (ls != 4u && ls != 8u) || (le != 4u && le != 8u)) return -1;
    *start = be_value(vs, ls);
    *end = be_value(ve, le);
    if (*end <= *start || *end - *start > MAX_INITRD_SIZE || !range_in_ram(*start, *end - *start)) return -2;
    return 0;
}

#if XNU_PAYLOAD
static int load_xnu(const u8 *dtb, u32 dtb_size, u64 initrd_start,
                    u64 initrd_end, u64 shim_base,
                    const loader_u8 *file, u32 file_size)
{
    struct xnu_payload_view p;
    int rc = xnu_payload_parse(file, file_size, &p);
    if (rc) return -20 + rc;
    /* MMU is off, so the copy below uses aligned 8-byte Device accesses: the
     * container start (inside the CPIO), the ADT and every segment source must
     * be 8-byte aligned or the copy would fault. */
    if (((u64)file & 7u) || (p.adt_off & 7u)) return -42;
    if (p.phys_base != XNU_EXPECTED_PHYS_BASE || p.mem_size != XNU_EXPECTED_MEM_SIZE)
        return -30;
    if (!range_in_ram(p.phys_base, p.mem_size)) return -31;

    logf("XNU payload: %d bytes, %d segments", (u64)file_size, (u64)p.nsegs);
    logf("XNU memory: PA=%x VA=%x size=%x", p.phys_base, p.virt_base, p.mem_size);

    u64 max_end_va = p.virt_base;
    int entry_found = 0;
    for (u32 i = 0; i < p.nsegs; i++) {
        struct xnu_payload_segment seg;
        if (xnu_payload_get_segment(&p, i, &seg)) return -32;
        if (seg.vmaddr < p.virt_base || (seg.vmsize & 7u)) return -33;
        if (seg.file_off & 7u) return -43;
        u64 delta = seg.vmaddr - p.virt_base;
        if (delta > p.mem_size || seg.vmsize > p.mem_size - delta ||
            p.phys_base > (u64)-1 - delta) return -34;
        u64 dst = p.phys_base + delta;
        if ((dst & 7u) || !region_is_free(dst, seg.vmsize, (u64)dtb, dtb_size,
                                          initrd_start, initrd_end, shim_base)) return -35;
        u64 seg_end = seg.vmaddr + seg.vmsize;
        if (seg_end > max_end_va) max_end_va = seg_end;
        if (p.entry_va >= seg.vmaddr && p.entry_va < seg_end) entry_found = 1;
    }
    if (!entry_found || p.kernel_end < max_end_va || p.kernel_end > p.virt_base + p.mem_size)
        return -36;

    u64 adt_delta = p.kernel_end - p.virt_base;
    if (p.phys_base > (u64)-1 - adt_delta) return -37;
    u64 adt_pa = p.phys_base + adt_delta;
    u64 adt_span = align_up(p.adt_size, 8);
    if (!adt_span || !region_is_free(adt_pa, adt_span, (u64)dtb, dtb_size,
                                     initrd_start, initrd_end, shim_base)) return -38;
    u64 adt_va = p.virt_base + adt_delta;
    u64 boot_args_pa = align_up(adt_pa + adt_span, XNU_PAGE_SIZE);
    if (!boot_args_pa || boot_args_pa > (u64)-1 - XNU_PAGE_SIZE) return -39;
    if (boot_args_pa < p.phys_base || boot_args_pa + XNU_PAGE_SIZE > p.phys_base + p.mem_size ||
        !region_is_free(boot_args_pa, XNU_PAGE_SIZE, (u64)dtb, dtb_size,
                        initrd_start, initrd_end, shim_base)) return -40;

    /* Complete preflight is above; only now touch kernel RAM. */
    for (u32 i = 0; i < p.nsegs; i++) {
        struct xnu_payload_segment seg;
        (void)xnu_payload_get_segment(&p, i, &seg);
        u64 dst = p.phys_base + (seg.vmaddr - p.virt_base);
        copy_words(dst, file + seg.file_off, seg.file_size);
        u64 copied = align_up(seg.file_size, 8);
        if (seg.vmsize > copied) zero_words(dst + copied, seg.vmsize - copied);
        __asm__ volatile("dsb sy" ::: "memory");
        logf("segment %d: PA=%x file=%x vm=%x", (u64)i, dst, seg.file_size, seg.vmsize);
    }

    copy_words(adt_pa, file + p.adt_off, p.adt_size);
    zero_words(boot_args_pa, XNU_PAGE_SIZE);

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
    _Static_assert(sizeof(struct xnu_boot_args) == 736, "boot_args rev2 size");

    volatile struct xnu_boot_args *ba = (volatile struct xnu_boot_args *)boot_args_pa;
    ba->Revision = 2;
    ba->Version = 2;
    ba->virtBase = p.virt_base;
    ba->physBase = p.phys_base;
    ba->memSize = p.mem_size;
    ba->topOfKernelData = boot_args_pa + XNU_PAGE_SIZE;
    ba->Video.v_baseAddr = XNU_FB_BASE;
    ba->Video.v_display = 1;
    ba->Video.v_rowBytes = XNU_FB_STRIDE;
    ba->Video.v_width = XNU_FB_WIDTH;
    ba->Video.v_height = XNU_FB_HEIGHT;
    ba->Video.v_depth = 32;
    ba->machineType = 0;
    ba->deviceTreeP = adt_va;
    ba->deviceTreeLength = (u32)p.adt_size;
    for (u32 i = 0; i < XNU_BOOT_LINE_LENGTH; i++) ba->CommandLine[i] = (char)p.cmdline[i];
    ba->bootFlags = 0;
    ba->memSizeActual = p.mem_size;
    __asm__ volatile("dsb sy" ::: "memory");

    u64 entry_delta = p.entry_va - p.virt_base;
    if (entry_delta >= p.mem_size || p.phys_base > (u64)-1 - entry_delta) return -41;
    u64 entry_pa = p.phys_base + entry_delta;
    logf("boot_args rev2: PA=%x ADT=%x+%x", boot_args_pa, adt_pa, p.adt_size);
    logf("XNU entry: PA=%x x0=%x", entry_pa, boot_args_pa);
#if XNU_PAYLOAD && PSCI_RESET_TIMER
    psci_start_reset_timer();
#endif
#if WDT_TIMEOUT_AT_HANDOFF
    wdt_arm(WDT_TIMEOUT_AT_HANDOFF);
#elif WDT_DISABLE_AT_HANDOFF
    wdt_disable();
#endif
#if PSCI_RESET_TIMER
    logf("HANDOFF: MMU off, caches off, secondaries untouched (cpu1 has the "
         "reset timer)");
#else
    logf("HANDOFF: MMU off, caches off, all CPUs handed to XNU; "
         "watchdog disabled, no reset timer");
#endif
    transfer_to_payload(entry_pa, boot_args_pa);
}
#endif

#if !XNU_PAYLOAD
struct test_boot_args {
    u64 signature;
    u64 version;
    u64 dtb;
    u64 dtb_size;
    u64 initrd_start;
    u64 initrd_end;
    u64 payload_start;
    u64 payload_size;
    u64 return_address;
    u64 payload_magic;
    u64 payload_pc;
    u64 payload_sctlr;
    u64 payload_currentel;
    u64 payload_x0;
    u64 reserved;
};
_Static_assert(__builtin_offsetof(struct test_boot_args, return_address) == 64, "test ABI return offset");
_Static_assert(__builtin_offsetof(struct test_boot_args, payload_magic) == 72, "test ABI result offset");
static struct test_boot_args test_args __attribute__((aligned(16)));

static __attribute__((noreturn)) void test_payload_return(struct test_boot_args *a)
{
    int ok = a == &test_args && a->signature == BOOT_ARGS_MAGIC &&
             a->payload_magic == TEST_PAYLOAD_RESULT && a->payload_x0 == (u64)a &&
             a->payload_pc >= a->payload_start &&
             a->payload_pc < a->payload_start + a->payload_size &&
             (a->payload_sctlr & (1u | 4u | 0x1000u)) == 0 &&
             a->payload_currentel == 4 && a->payload_start == TEST_PAYLOAD_BASE;
    if (ok) {
        logf("HANDOFF PASS: payload ran, EL1, MMU/caches off, x0=boot_args");
        finish_test_attempt("test passed");
    }
    logf("HANDOFF FAIL: test payload ABI mismatch");
    finish_test_attempt("test failed; ramoops preserved");
}

static int load_test_payload(const u8 *dtb, u32 dtb_size, u64 is, u64 ie, u64 shim_base,
                             const loader_u8 *file, u32 file_size)
{
    const loader_u8 *payload = 0;
    u32 payload_size = 0, expected_crc = 0;
    int rc = loader_parse_payload(file, file_size, &payload, &payload_size, &expected_crc);
    if (rc) return -50 + rc;
    if (!region_is_free(TEST_PAYLOAD_BASE, payload_size, (u64)dtb, dtb_size,
                        is, ie, shim_base)) return -55;
    memcpy((void *)TEST_PAYLOAD_BASE, payload, payload_size);
    __asm__ volatile("dsb sy" ::: "memory");
    if (loader_crc32((const loader_u8 *)TEST_PAYLOAD_BASE, payload_size) != expected_crc)
        return -56;

    memset(&test_args, 0, sizeof(test_args));
    test_args.signature = BOOT_ARGS_MAGIC;
    test_args.version = 1;
    test_args.dtb = (u64)dtb;
    test_args.dtb_size = dtb_size;
    test_args.initrd_start = is;
    test_args.initrd_end = ie;
    test_args.payload_start = TEST_PAYLOAD_BASE;
    test_args.payload_size = payload_size;
    test_args.return_address = (u64)test_payload_return;
    logf("test payload: PA=%x size=%d CRC verified", TEST_PAYLOAD_BASE, (u64)payload_size);
    logf("HANDOFF: test entry=%x x0=%x", TEST_PAYLOAD_BASE, (u64)&test_args);
    transfer_to_payload(TEST_PAYLOAD_BASE, (u64)&test_args);
}
#endif

void shim_main(u64 dtb_address, u64 x1, u64 x2, u64 x3, u64 shim_base)
{
    (void)x1;
    (void)x2;
    (void)x3;
    ramoops_init();
    console_init();
    logf("ios-kl7 shim v0.10.0");
    logf("console: framebuffer + ramoops");
    wdt_report("entry");
    logf("entry: DTB=%x shim=%x size=%x", dtb_address, shim_base, (u64)(_end - _start));

#if UFS_TEST
    logf("UFS TEST: read-only probe (XNU will not be loaded)");
    ufs_test();
    logf("UFS TEST: probe returned");
#if AUTO_REBOOT_TEST
    finish_test_attempt("UFS probe done");
#else
    logf("UFS TEST: halting (no reboot path)");
    for (;;) __asm__ volatile("wfe");
#endif
#endif

#if TOUCH_TEST
    logf("TOUCH TEST: SPI1 probe (XNU will not be loaded)");
    touch_test();
    logf("TOUCH TEST: probe returned");
#if AUTO_REBOOT_TEST
    finish_test_attempt("TOUCH probe done");
#else
    logf("TOUCH TEST: halting (no reboot path)");
    for (;;) __asm__ volatile("wfe");
#endif
#endif

#if BATTERY_TEST
#if WDT_DISABLE_AT_HANDOFF
    wdt_disable();
#endif
#if CHARGER_WRITE_TEST
    logf("BATTERY TEST: charger write/readback + MT6366 AUXADC probe (XNU will not be loaded)");
#else
    logf("BATTERY TEST: charger i2c + MT6366 AUXADC read-only probe (XNU will not be loaded)");
#endif
    battery_test();
    logf("BATTERY TEST: probe returned");
#if AUTO_REBOOT_TEST
    finish_test_attempt("BATTERY probe done");
#else
    logf("BATTERY TEST: halting (power-cycle to fastboot)");
    for (;;) __asm__ volatile("wfe");
#endif
#endif

#if GPU_TEST
#if WDT_DISABLE_AT_HANDOFF
    wdt_disable();
#else
    wdt_report("gpu-test-watchdog-backstop");
#endif
    logf("GPU TEST: Mali probe (XNU will not be loaded)");
#if AUTO_REBOOT_TEST && PSCI_RESET_TIMER
    /* Warm-reset backstop: if a register access hangs, cpu1 will still issue
     * PSCI SYSTEM_RESET and preserve ramoops for the next Android boot. */
    psci_start_reset_timer();
#endif
    gpu_test();
#if AUTO_REBOOT_TEST
    /* Keep the WB output-mapping test short; reset after its final log. */
    psci_system_reset();
#else
    logf("GPU TEST: probe returned");
    logf("GPU TEST: halting (power-cycle to boot iOS)");
    for (;;) __asm__ volatile("wfe");
#endif
#endif

    const u8 *dtb = (const u8 *)dtb_address;
    u32 dtb_size = 0;
    if (!dtb_header(dtb, &dtb_size)) fail("invalid FDT header", -1);
    int rc = memmap_parse_dtb(dtb, &memory_map);
    if (rc) fail("cannot parse FDT memory map/reservations", rc);
    logf("DTB: size=%d RAM=%d reserved=%d no-map=%d FDT-rsv=%d",
         (u64)dtb_size, (u64)memory_map.nram, (u64)memory_map.nreserved,
         (u64)memory_map.nnomap, (u64)memory_map.nfdt_reserved);

    u64 initrd_start = 0, initrd_end = 0;
    rc = get_initrd(dtb, dtb_size, &initrd_start, &initrd_end);
    if (rc) fail("initrd range missing, malformed, or outside RAM", rc);
    logf("initrd: %x..%x size=%x", initrd_start, initrd_end, initrd_end - initrd_start);

    const loader_u8 *payload_file = 0;
    u32 payload_file_size = 0;
#if XNU_PAYLOAD
    rc = loader_cpio_find_file((const loader_u8 *)initrd_start, initrd_end - initrd_start,
                               XNU_PAYLOAD_NAME, &payload_file, &payload_file_size);
    if (rc || !payload_file) fail("SWEETXNU container not found in CPIO", rc);
    logf("CPIO: %s size=%d", XNU_PAYLOAD_NAME, (u64)payload_file_size);
    rc = load_xnu(dtb, dtb_size, initrd_start, initrd_end, shim_base,
                  payload_file, payload_file_size);
    fail("XNU handoff preparation failed", rc);
#else
    rc = loader_cpio_find_file((const loader_u8 *)initrd_start, initrd_end - initrd_start,
                               TEST_PAYLOAD_NAME, &payload_file, &payload_file_size);
    if (rc || !payload_file) fail("test payload not found in CPIO", rc);
    logf("CPIO: %s size=%d", TEST_PAYLOAD_NAME, (u64)payload_file_size);
    rc = load_test_payload(dtb, dtb_size, initrd_start, initrd_end, shim_base,
                           payload_file, payload_file_size);
    fail("test payload handoff failed", rc);
#endif
}
