/*
 * Touch probe for KL7 (Tecno Spark 30 Pro, MT6789).
 *
 * FocalTech FTS on SPI1 (spi1@11010000, CS0):
 *   SCK=GPIO34, CS=GPIO35, MOSI=GPIO36, MISO=GPIO37 (pinmux function 1)
 *   RESET=GPIO116, IRQ=GPIO9, AVDD=GPIO61
 *
 * Register facts are taken from the MT6789 downstream kernel
 * (Mandi-Sa/micode-yunluo-s-oss, branch yunluo-s-oss):
 *   drivers/pinctrl/mediatek/pinctrl-mt6789.c
 *   drivers/clk/mediatek/clk-mt6789.c, clk-mt6789-bus.c
 *   drivers/spi/spi-mt65xx.c
 * and the FTS SPI framing from focaltech_spi.c (ft3658):
 *   header [reg, cmd, len_hi, len_lo], 3 dummy bytes, data at rx[7],
 *   read cmd 0xA0 (0x80 | DATA_CRC_EN), 2 CRC bytes, status at rx[3].
 */
#include "../../console.h"
#include "../../core/timer.h"
#include "fts3519.h"

typedef unsigned char u8;
typedef unsigned int u32;
typedef unsigned long long u64;

/* MT6789 pin controller, base 0x10005000 (pinctrl-mt6789.c). */
#define GPIO_BASE		0x10005000UL
#define GPIO_BANK(n)		((u32)(n) / 32u)
#define GPIO_BIT(n)		((u32)(n) % 32u)
#define GPIO_DIR(bank)		(0x0000u + (u32)(bank) * 0x10u)
#define GPIO_DOUT(bank)		(0x0100u + (u32)(bank) * 0x10u)
#define GPIO_DIN(bank)		(0x0200u + (u32)(bank) * 0x10u)
#define GPIO_MODE(pin)		(0x0300u + ((u32)(pin) / 8u) * 0x10u)
#define GPIO_MODE_SHIFT(pin)	(((u32)(pin) % 8u) * 4u)

/* MT6789 topckgen, base 0x10000000 (clk-mt6789.c). */
#define TOPCKGEN_BASE		0x10000000UL
#define TOP_CLK_CFG6		0x0070u
#define TOP_CLK_CFG6_SET	0x0074u
#define TOP_CLK_CFG6_CLR	0x0078u
#define TOP_CLK_UPDATE		0x0004u
#define TOP_SPI_MUX_SHIFT	16u
#define TOP_SPI_MUX_MASK	(7u << TOP_SPI_MUX_SHIFT)
#define TOP_SPI_PDN		(1u << 23)	/* 1 = gated */

/* APMIXEDSYS: MSDCPLL is the default spi_sel parent (msdcpll_d2). */
#define APMIXED_BASE		0x1000C000ul
#define MSDCPLL_CON0		0x0350u
#define MSDCPLL_CON1		0x0354u
#define MSDCPLL_CON3		0x035Cu
#define PLL_PWR_ON		(1u << 0)
#define PLL_ISO_EN		(1u << 1)
#define PLL_EN			(1u << 0)
#define TOP_SPI_UPDATE		(1u << 26)

/* MT6789 infracfg_ao, base 0x10001000 (clk-mt6789-bus.c). */
#define INFRACFG_AO_BASE	0x10001000UL
#define IFRAO_SPI1_SET		0x00A4u
#define IFRAO_SPI1_CLR		0x00A8u
#define IFRAO_SPI1_STA		0x00ACu
#define IFRAO_SPI1_BIT		(1u << 6)
#define IFRAO_SPI2_BIT		(1u << 9)
#define IFRAO_SPI3_BIT		(1u << 10)

/* MTK SPI1 controller, base 0x11010000 (spi-mt65xx.c). */
#define SPI1_BASE		0x11010000UL
#define SPI2_BASE		0x11012000UL
#define SPI3_BASE		0x11013000UL
#define SPI_CFG0_REG		0x0000u
#define SPI_CFG1_REG		0x0004u
#define SPI_TX_DATA_REG		0x0010u
#define SPI_RX_DATA_REG		0x0014u
#define SPI_CMD_REG		0x0018u
#define SPI_TX_SRC_REG		0x0008u
#define SPI_RX_DST_REG		0x000Cu
#define SPI_STATUS0_REG		0x001Cu
#define SPI_STATUS1_REG		0x0020u
#define SPI_TX_SRC_REG_64	0x002Cu
#define SPI_RX_DST_REG_64	0x0030u
#define SPI_PAD_SEL_REG		0x0024u
#define SPI_CFG2_REG		0x0028u

#define SPI_CMD_ACT		(1u << 0)
#define SPI_CMD_RESUME		(1u << 1)
#define SPI_CMD_RST		(1u << 2)
#define SPI_CMD_PAUSE_EN	(1u << 4)
#define SPI_CMD_RX_DMA		(1u << 10)
#define SPI_CMD_TX_DMA		(1u << 11)
#define SPI_CMD_TXMSBF		(1u << 12)
#define SPI_CMD_RXMSBF		(1u << 13)
#define SPI_CMD_FINISH_IE	(1u << 16)
#define SPI_CMD_PAUSE_IE	(1u << 17)

#define SPI_STATUS_FINISH	(1u << 0)
#define SPI_STATUS_PAUSE	(1u << 1)

/*
 * sck_time/cs_time are selected after the topckgen parent is chosen.
 * sck_time is the half period in parent clock cycles, so
 * SCK = parent / (2 * sck_time), same math as mtk_spi_prepare_transfer().
 * Defaults match msdcpll_d2 (192 MHz) at ~5.3 MHz.
 */
static u32 spi_sck_time = 18u;
static u32 spi_cs_time = 36u;

/* Board pins from the live KL7 device tree. */
#define TOUCH_SPI_PIN_FIRST	34u
#define TOUCH_SPI_PIN_LAST	37u
#define TOUCH_SPI_PIN_FUNC	1u	/* PINMUX_GPIOxx__FUNC_SPI1_* */
#define TOUCH_RESET_GPIO	116u
#define TOUCH_IRQ_GPIO		9u
#define TOUCH_AVDD_GPIO		61u

/* FocalTech SPI framing (focaltech_spi.c). */
#define FTS_CMD_READ		0xA0u	/* 0x80 | DATA_CRC_EN(0x20) */
#define FTS_CMD_READ_TOUCH_DATA	0x01u	/* FTS touch report register */
#define FTS_DUMMY_BYTE		3u
#define FTS_HDR_LEN		4u
#define FTS_CRC_LEN		2u
#define FTS_DATA_OFF		(FTS_HDR_LEN + FTS_DUMMY_BYTE)
#define FTS_BUF_SIZE		96u

#define FTS_REG_CHIP_ID_H	0xA3u
#define FTS_REG_CHIP_ID_L	0x9Fu
#define FTS_REG_FW_VER		0xA6u
#define FTS_REG_POWER_MODE	0xA5u
#define FTS_CHIP_ID_H		0x54u
#define FTS_CHIP_ID_L		0x52u

static u32 fts_last_status;
static int fts_ready;

static inline u32 gpio_rd(u32 off)
{
	return *(volatile u32 *)(GPIO_BASE + off);
}

static inline void gpio_wr(u32 off, u32 v)
{
	*(volatile u32 *)(GPIO_BASE + off) = v;
}

static inline u32 top_rd(u32 off)
{
	return *(volatile u32 *)(TOPCKGEN_BASE + off);
}

static inline u32 ap_rd(u32 off)
{
	return *(volatile u32 *)(APMIXED_BASE + off);
}

static inline u32 infra_rd(u32 off)
{
	return *(volatile u32 *)(INFRACFG_AO_BASE + off);
}

static inline void infra_wr(u32 off, u32 v)
{
	*(volatile u32 *)(INFRACFG_AO_BASE + off) = v;
}

static inline u32 spi_rd(u32 off)
{
	return *(volatile u32 *)(SPI1_BASE + off);
}

static inline void spi_wr(u32 off, u32 v)
{
	*(volatile u32 *)(SPI1_BASE + off) = v;
}

static inline u32 spi_rd_at(u64 base, u32 off)
{
	return *(volatile u32 *)(base + off);
}

static inline void spi_wr_at(u64 base, u32 off, u32 v)
{
	*(volatile u32 *)(base + off) = v;
}









/* Set a pin's 4-bit pinmux function (0 = GPIO). */
static void gpio_set_mode(u32 pin, u32 fn)
{
	u32 off = GPIO_MODE(pin);
	u32 shift = GPIO_MODE_SHIFT(pin);
	u32 v = gpio_rd(off);

	v &= ~(0xFu << shift);
	v |= (fn & 0xFu) << shift;
	gpio_wr(off, v);
}

static void gpio_set_dir(u32 pin, u32 is_out)
{
	u32 off = GPIO_DIR(GPIO_BANK(pin));
	u32 bit = GPIO_BIT(pin);
	u32 v = gpio_rd(off);

	if (is_out)
		v |= (1u << bit);
	else
		v &= ~(1u << bit);
	gpio_wr(off, v);
}

static void gpio_set(u32 pin, u32 val)
{
	u32 off = GPIO_DOUT(GPIO_BANK(pin));
	u32 bit = GPIO_BIT(pin);
	u32 v = gpio_rd(off);

	if (val)
		v |= (1u << bit);
	else
		v &= ~(1u << bit);
	gpio_wr(off, v);
}

static u32 gpio_get(u32 pin)
{
	return (gpio_rd(GPIO_DIN(GPIO_BANK(pin))) >> GPIO_BIT(pin)) & 1u;
}

/*
 * Prove that the SPI controller register block is really clocked:
 * write a known packet length to CFG1 and the MSB-first bits to CMD,
 * then read both back.  A dead bus returns zero for every register.
 */
static int spi_regs_alive_at(u64 base)
{
	u32 cmd, cfg1;

	spi_wr_at(base, SPI_CMD_REG, SPI_CMD_RST);
	timer_delay_us(5);
	spi_wr_at(base, SPI_CMD_REG, 0u);
	timer_delay_us(5);

	spi_wr_at(base, SPI_CFG1_REG, 0u);
	timer_delay_us(2);
	spi_wr_at(base, SPI_CFG1_REG, 0x00050000u);
	timer_delay_us(5);
	cfg1 = spi_rd_at(base, SPI_CFG1_REG);

	spi_wr_at(base, SPI_CMD_REG, SPI_CMD_TXMSBF | SPI_CMD_RXMSBF |
				    SPI_CMD_FINISH_IE | SPI_CMD_PAUSE_IE);
	timer_delay_us(5);
	cmd = spi_rd_at(base, SPI_CMD_REG);

	return ((cfg1 & 0x001F0000u) == 0x00050000u) &&
	       ((cmd & (SPI_CMD_TXMSBF | SPI_CMD_RXMSBF)) ==
		(SPI_CMD_TXMSBF | SPI_CMD_RXMSBF));
}

static int spi_regs_alive(void)
{
	return spi_regs_alive_at(SPI1_BASE);
}

/*
 * Nonce readback: write a value that no previous FTS packet used, then
 * read it back.  This rules out stale registers on a dead controller.
 */
static u32 spi_nonce(void)
{
	spi_wr(SPI_CMD_REG, SPI_CMD_RST);
	timer_delay_us(5);
	spi_wr(SPI_CMD_REG, 0u);
	timer_delay_us(5);
	spi_wr(SPI_CFG0_REG, 0x00000012u);
	timer_delay_us(5);
	return spi_rd(SPI_CFG0_REG);
}

/*
 * mtk_spi_set_cs(true): drop PAUSE_EN to release CS, then soft reset.
 * The driver does this after every message; without it CS stays low and
 * the FTS state machine rejects the next header.
 */
static void spi_cs_deassert(void)
{
	u32 cmd = spi_rd(SPI_CMD_REG);

	cmd &= ~(SPI_CMD_PAUSE_EN | SPI_CMD_ACT);
	spi_wr(SPI_CMD_REG, cmd);
	timer_delay_us(2);
	spi_wr(SPI_CMD_REG, cmd | SPI_CMD_RST);
	timer_delay_us(2);
	spi_wr(SPI_CMD_REG, cmd & ~SPI_CMD_RST);
}

/* One full-duplex SPI packet, mode 0, MSB first. */
static int spi_xfer(const u8 *tx, u8 *rx, u32 len)
{
	u32 i, w, cmd;
	u64 start, timeout;

	if (len == 0u || len > 32u)
		return -1;

	/* Reset FIFO/state, then program mode 0 + MSB first. */
	cmd = spi_rd(SPI_CMD_REG);
	spi_wr(SPI_CMD_REG, cmd | SPI_CMD_RST);
	timer_delay_us(2);
	spi_wr(SPI_CMD_REG, cmd & ~SPI_CMD_RST);
	timer_delay_us(2);

	/* PAUSE_EN must be set before the FIFO and ACT (mtk_spi_set_cs). */
	spi_wr(SPI_CMD_REG, SPI_CMD_TXMSBF | SPI_CMD_RXMSBF |
			    SPI_CMD_FINISH_IE | SPI_CMD_PAUSE_IE |
			    SPI_CMD_PAUSE_EN);
	spi_wr(SPI_PAD_SEL_REG, 0u);

	/* enhance_timing: SCK in CFG2, CS setup/hold in CFG0. */
	spi_wr(SPI_CFG2_REG, (spi_sck_time - 1u) |
			     ((spi_sck_time - 1u) << 16));
	spi_wr(SPI_CFG0_REG, (spi_cs_time - 1u) |
			     ((spi_cs_time - 1u) << 16));

	/* CS idle time + packet length in bytes; packet loop = 0. */
	spi_wr(SPI_CFG1_REG, (spi_cs_time - 1u) | ((len - 1u) << 16));

	/* 32-bit FIFO: four wire bytes per word, little endian. */
	for (i = 0; i < len; i += 4u) {
		w = (u32)tx[i];
		if (i + 1u < len)
			w |= (u32)tx[i + 1u] << 8;
		if (i + 2u < len)
			w |= (u32)tx[i + 2u] << 16;
		if (i + 3u < len)
			w |= (u32)tx[i + 3u] << 24;
		spi_wr(SPI_TX_DATA_REG, w);
	}

	cmd = spi_rd(SPI_CMD_REG);
	spi_wr(SPI_CMD_REG, cmd | SPI_CMD_ACT);

	/* Completion is STATUS0 bit0 (finish) or bit1 (pause). */
	start = timer_ticks();
	timeout = timer_frequency() / 10u;	/* 100 ms */
	while ((spi_rd(SPI_STATUS0_REG) &
		(SPI_STATUS_FINISH | SPI_STATUS_PAUSE)) == 0u) {
		if (timer_ticks() - start > timeout) {
			u32 st = spi_rd(SPI_STATUS0_REG);
			u32 s1 = spi_rd(SPI_STATUS1_REG);
			u32 r0 = spi_rd(SPI_RX_DATA_REG);
			u32 r1 = spi_rd(SPI_RX_DATA_REG);
			u32 r2 = spi_rd(SPI_RX_DATA_REG);
			u32 r3 = spi_rd(SPI_RX_DATA_REG);
			logf("TOUCH: spi timeout st=%x s1=%x cmd=%x cfg1=%x rx=%x,%x,%x,%x",
			     (u64)st, (u64)s1,
			     (u64)spi_rd(SPI_CMD_REG),
			     (u64)spi_rd(SPI_CFG1_REG),
			     (u64)r0, (u64)r1, (u64)r2, (u64)r3);
			spi_cs_deassert();
			return -2;
		}
	}

	for (i = 0; i < len; i += 4u) {
		w = spi_rd(SPI_RX_DATA_REG);
		rx[i] = (u8)w;
		if (i + 1u < len)
			rx[i + 1u] = (u8)(w >> 8);
		if (i + 2u < len)
			rx[i + 2u] = (u8)(w >> 16);
		if (i + 3u < len)
			rx[i + 3u] = (u8)(w >> 24);
	}

	spi_cs_deassert();
	return 0;
}

/*
 * Long PIO read: the MTK FIFO is 32 bytes, so send the packet in chunks
 * with the CS kept asserted between chunks.  This is the same chunking
 * the mtk_spi_interrupt() FIFO path uses for transfers > 32 bytes.  It
 * avoids the internal DMA engine entirely.
 */
static int spi_xfer_long(const u8 *tx, u8 *rx, u32 len)
{
	u32 off, chunk, i, w, cmd;

	if (len == 0u || len > FTS_BUF_SIZE)
		return -1;

	cmd = spi_rd(SPI_CMD_REG);
	spi_wr(SPI_CMD_REG, cmd | SPI_CMD_RST);
	timer_delay_us(2);
	spi_wr(SPI_CMD_REG, cmd & ~SPI_CMD_RST);
	timer_delay_us(2);

	spi_wr(SPI_CMD_REG, SPI_CMD_TXMSBF | SPI_CMD_RXMSBF |
			    SPI_CMD_FINISH_IE | SPI_CMD_PAUSE_IE |
			    SPI_CMD_PAUSE_EN);
	spi_wr(SPI_PAD_SEL_REG, 0u);
	spi_wr(SPI_CFG2_REG, (spi_sck_time - 1u) |
			     ((spi_sck_time - 1u) << 16));
	spi_wr(SPI_CFG0_REG, (spi_cs_time - 1u) |
			     ((spi_cs_time - 1u) << 16));

	for (off = 0u; off < len; off += chunk) {
		chunk = len - off;
		if (chunk > 32u)
			chunk = 32u;

		spi_wr(SPI_CFG1_REG,
		       (spi_cs_time - 1u) | ((chunk - 1u) << 16));

		for (i = 0u; i < chunk; i += 4u) {
			w = (u32)tx[off + i];
			if (i + 1u < chunk)
				w |= (u32)tx[off + i + 1u] << 8;
			if (i + 2u < chunk)
				w |= (u32)tx[off + i + 2u] << 16;
			if (i + 3u < chunk)
				w |= (u32)tx[off + i + 3u] << 24;
			spi_wr(SPI_TX_DATA_REG, w);
		}

		cmd = spi_rd(SPI_CMD_REG);
		spi_wr(SPI_CMD_REG, cmd | SPI_CMD_ACT);

		/*
		 * ACT clears immediately on this controller, before the
		 * FIFO has actually been clocked out, and the finish/pause
		 * status is a transient that MMIO polling misses.  Wait
		 * long enough for a 32-byte chunk (well under 100 us at
		 * the configured SCK) before draining the RX FIFO.
		 */
		timer_delay_us(2000);

		for (i = 0u; i < chunk; i += 4u) {
			w = spi_rd(SPI_RX_DATA_REG);
			rx[off + i] = (u8)w;
			if (i + 1u < chunk)
				rx[off + i + 1u] = (u8)(w >> 8);
			if (i + 2u < chunk)
				rx[off + i + 2u] = (u8)(w >> 16);
			if (i + 3u < chunk)
				rx[off + i + 3u] = (u8)(w >> 24);
		}
	}

	spi_cs_deassert();
	return 0;
}

/* Clean+invalidate by VA if the D-cache is enabled.  The shim normally
 * runs with SCTLR.C clear, but the bootloader may leave it on, and then
 * the CPU would read stale zeros from the stack after an SPI DMA write. */


/*
 * Same packet as spi_xfer(), but using the MTK SPI controller's internal
 * DMA.  The FTS touch report is 62 bytes (Android reads exactly 0x3e),
 * so the full packet is 4 header + 3 dummy + 62 data + 2 CRC = 71 bytes.
 * The FIFO path cannot complete this; DMA is what the Android driver uses.
 */


/* FTS register read: rx[7] is the first data byte, rx[3] is status. */
static int fts_read(u8 reg, u8 *val, u32 n)
{
	u8 tx[FTS_BUF_SIZE] __attribute__((aligned(64)));
	u8 rx[FTS_BUF_SIZE] __attribute__((aligned(64)));
	u32 i, total;
	int rc;

	if (n == 0u || n > 62u)
		return -1;
	total = FTS_HDR_LEN + FTS_DUMMY_BYTE + n + FTS_CRC_LEN;
	if (total > FTS_BUF_SIZE)
		return -1;

	for (i = 0; i < FTS_BUF_SIZE; i++) {
		tx[i] = 0u;
		rx[i] = 0u;
	}
	tx[0] = reg;
	tx[1] = FTS_CMD_READ;
	tx[2] = 0u;
	tx[3] = (u8)n;

	if (total > 32u)
		rc = spi_xfer_long(tx, rx, total);
	else
		rc = spi_xfer(tx, rx, total);
	if (rc)
		return rc;

	fts_last_status = rx[3];
	for (i = 0; i < n; i++)
		val[i] = rx[FTS_DATA_OFF + i];

	/* FTS requires CS high for at least 150 us between transfers. */
	timer_delay_us(200);
	return 0;
}

/*
 * Read one FTS touch report (register 0x01).  FT3519 uses the 16-bit
 * point format; the first data byte is the point count, then six bytes
 * per point:
 *   [XH(event[7:6]) XL YH(id[7:4]) YL AREA PRE]
 * The raw axes are 16x the panel: X max 17279, Y max 38975.
 */
#define FTS_MAX_POINTS	10u



int fts3519_read_points(struct fts3519_point *pts, u32 max)
{
	if (!fts_ready || !pts || max == 0u)
		return -1;
	u8 buf[FTS_BUF_SIZE];
	u32 i, j, count, base, rx, ry;
	u8 xh, xl, yh, yl, area, pre;
	int rc;

	for (i = 0; i < FTS_BUF_SIZE; i++)
		buf[i] = 0u;

	rc = fts_read(FTS_CMD_READ_TOUCH_DATA, buf, 62u);
	if (rc)
		return rc;

	/*
	 * The 62-byte report starts with a dummy byte (Android's
	 * fts_touch_point shows 00 01 ...); the point count is at [1]
	 * and each point is six bytes starting at [2 + 6 * i].
	 * fts_input_report_touch reads the count with ldrb [x1, #1]
	 * and the point at [x1 + 2], [x1 + 8], ...
	 */
	count = buf[1] & 0x0Fu;
	if (count > FTS_MAX_POINTS)
		count = 0u;
	if (count > max)
		count = max;
	if (count == 0u)
		return 0;

	for (i = 0; i < count; i++) {
		base = 2u + 6u * i;
		xh = buf[base + 0u];
		xl = buf[base + 1u];
		yh = buf[base + 2u];
		yl = buf[base + 3u];
		area = buf[base + 4u];
		pre = buf[base + 5u];

		rx = ((u32)xl << 3) | ((u32)(xh & 0x0Fu) << 11) |
		     ((u32)(xh >> 5) & 1u) | ((u32)(area >> 5) & 6u);
		ry = ((u32)yl << 4) | ((u32)(yh & 0x0Fu) << 12) |
		     ((u32)(area >> 2) & 0x0Cu) | ((u32)(xh >> 3) & 2u) |
		     ((u32)(pre >> 7) & 1u);

		pts[i].raw_x = rx;
		pts[i].raw_y = ry;
		pts[i].x = rx / 16u;
		pts[i].y = ry / 16u;
		if (pts[i].x >= 1080u)
			pts[i].x = 1079u;
		if (pts[i].y >= 2436u)
			pts[i].y = 2435u;
		pts[i].id = (u8)(yh >> 4);
		pts[i].down = ((xh >> 6) != 1u) ? 1u : 0u;
		for (j = 0; j < 6u; j++)
			pts[i].b[j] = buf[base + j];
	}

	return (int)count;
}



int fts3519_init(void)
{
	u8 idh = 0u, idl = 0u, fw = 0u, pm = 0u;
	u32 i, mode_old, mode_new, nonce;
	int rc = -1;

	if (fts_ready)
		return 0;

	{
		u64 sctlr;
		asm volatile("mrs %0, sctlr_el1" : "=r"(sctlr));
		logf("TOUCH: sctlr=%x", sctlr);
	}
	logf("FTS3519: SPI1 init rst=gpio%d irq=gpio%d avdd=gpio%d",
	     (u64)TOUCH_RESET_GPIO, (u64)TOUCH_IRQ_GPIO,
	     (u64)TOUCH_AVDD_GPIO);
	logf("FTS3519: boot top cfg6=%x ifrao=%x",
	     (u64)top_rd(TOP_CLK_CFG6), (u64)infra_rd(IFRAO_SPI1_STA));
	logf("FTS3519: boot apmixed con0=%x con1=%x con3=%x",
	     (u64)ap_rd(MSDCPLL_CON0), (u64)ap_rd(MSDCPLL_CON1),
	     (u64)ap_rd(MSDCPLL_CON3));

	/* The LK leaves SPI1 enabled. CLR deasserts the gate; SET disables it. */
	logf("FTS3519: SPI1 raw alive=%d", (u64)spi_regs_alive());
	infra_wr(IFRAO_SPI1_CLR, IFRAO_SPI1_BIT);
	timer_delay_us(100);
	logf("FTS3519: SPI1 gate alive=%d sta=%x",
	     (u64)spi_regs_alive(), (u64)infra_rd(IFRAO_SPI1_STA));

	nonce = spi_nonce();
	logf("FTS3519: nonce cfg0=%x", (u64)nonce);
	mode_old = gpio_rd(GPIO_MODE(TOUCH_SPI_PIN_FIRST));
	for (i = TOUCH_SPI_PIN_FIRST; i <= TOUCH_SPI_PIN_LAST; i++)
		gpio_set_mode(i, TOUCH_SPI_PIN_FUNC);
	mode_new = gpio_rd(GPIO_MODE(TOUCH_SPI_PIN_FIRST));
	logf("FTS3519: mode34 old=%x new=%x alive=%d",
	     (u64)mode_old, (u64)mode_new, (u64)spi_regs_alive());

	gpio_set_mode(TOUCH_AVDD_GPIO, 0u);
	gpio_set_mode(TOUCH_RESET_GPIO, 0u);
	gpio_set_mode(TOUCH_IRQ_GPIO, 0u);
	gpio_set_dir(TOUCH_IRQ_GPIO, 0u);

	gpio_set(TOUCH_AVDD_GPIO, 1u);
	gpio_set_dir(TOUCH_AVDD_GPIO, 1u);
	timer_delay_ms(2);
	gpio_set(TOUCH_RESET_GPIO, 0u);
	gpio_set_dir(TOUCH_RESET_GPIO, 1u);
	timer_delay_ms(10);
	gpio_set(TOUCH_RESET_GPIO, 1u);
	timer_delay_ms(150);
	logf("FTS3519: powered alive=%d sta=%x irq=%d",
	     (u64)spi_regs_alive(), (u64)infra_rd(IFRAO_SPI1_STA),
	     (u64)gpio_get(TOUCH_IRQ_GPIO));

	for (i = 0; i < 5u; i++) {
		idh = 0u;
		idl = 0u;
		rc = fts_read(FTS_REG_CHIP_ID_H, &idh, 1u);
		if (rc == 0)
			rc = fts_read(FTS_REG_CHIP_ID_L, &idl, 1u);
		logf("FTS3519: try=%d rc=%d status=%x id=%x%x",
		     (u64)i, (u64)(rc < 0 ? -rc : rc),
		     (u64)fts_last_status, (u64)idh, (u64)idl);
		if (rc == 0 && idh == FTS_CHIP_ID_H && idl == FTS_CHIP_ID_L)
			break;
		timer_delay_ms(50);
	}

	if (rc != 0 || idh != FTS_CHIP_ID_H || idl != FTS_CHIP_ID_L) {
		logf("FTS3519: init failed rc=%d status=%x id=%x%x",
		     (u64)(rc < 0 ? -rc : rc), (u64)fts_last_status,
		     (u64)idh, (u64)idl);
		return -2;
	}

	(void)fts_read(FTS_REG_FW_VER, &fw, 1u);
	(void)fts_read(FTS_REG_POWER_MODE, &pm, 1u);
	logf("FTS3519: ready id=%x%x fw=%x pm=%x",
	     (u64)idh, (u64)idl, (u64)fw, (u64)pm);
	fts_ready = 1;
	return 0;
}
