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
#include "console.h"
#include "touch_test.h"

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
#define SPI_STATUS0_REG		0x001Cu
#define SPI_PAD_SEL_REG		0x0024u
#define SPI_CFG2_REG		0x0028u

#define SPI_CMD_ACT		(1u << 0)
#define SPI_CMD_RST		(1u << 2)
#define SPI_CMD_PAUSE_EN	(1u << 4)
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
#define FTS_BUF_SIZE		32u

#define FTS_REG_CHIP_ID_H	0xA3u
#define FTS_REG_CHIP_ID_L	0x9Fu
#define FTS_REG_FW_VER		0xA6u
#define FTS_REG_POWER_MODE	0xA5u
#define FTS_CHIP_ID_H		0x54u
#define FTS_CHIP_ID_L		0x52u

static u32 fts_last_status;

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

static u64 ticks(void)
{
	u64 t;
	__asm__ volatile("isb; mrs %0, cntvct_el0" : "=r"(t));
	return t;
}

static u64 timer_freq(void)
{
	u64 f;
	__asm__ volatile("mrs %0, cntfrq_el0" : "=r"(f));
	return f ? f : 19200000u;
}

static void delay_us(u64 us)
{
	u64 start = ticks();
	u64 delta = us * timer_freq() / 1000000u;
	while (ticks() - start < delta)
		;
}

static void delay_ms(u32 ms)
{
	delay_us((u64)ms * 1000u);
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
	delay_us(5);
	spi_wr_at(base, SPI_CMD_REG, 0u);
	delay_us(5);

	spi_wr_at(base, SPI_CFG1_REG, 0u);
	delay_us(2);
	spi_wr_at(base, SPI_CFG1_REG, 0x00050000u);
	delay_us(5);
	cfg1 = spi_rd_at(base, SPI_CFG1_REG);

	spi_wr_at(base, SPI_CMD_REG, SPI_CMD_TXMSBF | SPI_CMD_RXMSBF |
				    SPI_CMD_FINISH_IE | SPI_CMD_PAUSE_IE);
	delay_us(5);
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
	delay_us(5);
	spi_wr(SPI_CMD_REG, 0u);
	delay_us(5);
	spi_wr(SPI_CFG0_REG, 0x00000012u);
	delay_us(5);
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
	delay_us(2);
	spi_wr(SPI_CMD_REG, cmd | SPI_CMD_RST);
	delay_us(2);
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
	delay_us(2);
	spi_wr(SPI_CMD_REG, cmd & ~SPI_CMD_RST);
	delay_us(2);

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
	start = ticks();
	timeout = timer_freq() / 10u;	/* 100 ms */
	while ((spi_rd(SPI_STATUS0_REG) &
		(SPI_STATUS_FINISH | SPI_STATUS_PAUSE)) == 0u) {
		if (ticks() - start > timeout) {
			logf("TOUCH: spi timeout st=%x cmd=%x cfg1=%x cfg2=%x",
			     (u64)spi_rd(SPI_STATUS0_REG),
			     (u64)spi_rd(SPI_CMD_REG),
			     (u64)spi_rd(SPI_CFG1_REG),
			     (u64)spi_rd(SPI_CFG2_REG));
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

/* FTS register read: rx[7] is the first data byte, rx[3] is status. */
static int fts_read(u8 reg, u8 *val, u32 n)
{
	u8 tx[FTS_BUF_SIZE];
	u8 rx[FTS_BUF_SIZE];
	u32 i, total;
	int rc;

	if (n == 0u || n > 16u)
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

	rc = spi_xfer(tx, rx, total);
	if (rc)
		return rc;

	fts_last_status = rx[3];
	for (i = 0; i < n; i++)
		val[i] = rx[FTS_DATA_OFF + i];

	/* FTS requires CS high for at least 150 us between transfers. */
	delay_us(200);
	return 0;
}

/*
 * Read one FTS touch report (register 0x01).  FT3519 uses the 16-bit
 * point format; the first data byte is the point count, then six bytes
 * per point:
 *   [XH(event[7:6]) XL YH(id[7:4]) YL AREA PRE]
 * The raw axes are 16x the panel: X max 17279, Y max 38975.
 */
static int fts_read_touch(u32 *x, u32 *y, int *down)
{
	u8 buf[FTS_BUF_SIZE];
	u32 i, count, rx, ry;
	u8 xh, xl, yh, yl, area, pre;
	int rc;

	for (i = 0; i < FTS_BUF_SIZE; i++)
		buf[i] = 0u;

	rc = fts_read(FTS_CMD_READ_TOUCH_DATA, buf, 16u);
	if (rc)
		return rc;

	count = buf[0] & 0x0Fu;
	if (count == 0u || count > 10u) {
		*down = 0;
		return 0;
	}

	xh = buf[1];
	xl = buf[2];
	yh = buf[3];
	yl = buf[4];
	area = buf[5];
	pre = buf[6];

	rx = ((u32)xl << 3) | ((u32)(xh & 0x0Fu) << 11) |
	     ((u32)(xh >> 5) & 1u) | ((u32)(area >> 5) & 6u);
	ry = ((u32)yl << 4) | ((u32)(yh & 0x0Fu) << 12) |
	     ((u32)(area >> 2) & 0x0Cu) | ((u32)(xh >> 3) & 2u) |
	     ((u32)(pre >> 7) & 1u);

	*x = rx;
	*y = ry;
	*down = ((xh >> 6) != 1u);
	return 0;
}

void touch_test(void)
{
	u8 idh, idl, fw, pm;
	u32 i, mode_old, mode_new, nonce;
	int rc;

	logf("TOUCH: FTS SPI1 probe");
	logf("TOUCH: rst=gpio%d irq=gpio%d avdd=gpio%d",
	     (u64)TOUCH_RESET_GPIO, (u64)TOUCH_IRQ_GPIO, (u64)TOUCH_AVDD_GPIO);

	/* The bootloader already leaves SPI1 clocked.  Print the boot state
	 * so the proof is in the log. */
	logf("TOUCH: boot top cfg6=%x ifrao=%x",
	     (u64)top_rd(TOP_CLK_CFG6), (u64)infra_rd(IFRAO_SPI1_STA));
	logf("TOUCH: boot apmixed con0=%x con1=%x con3=%x",
	     (u64)ap_rd(MSDCPLL_CON0), (u64)ap_rd(MSDCPLL_CON1),
	     (u64)ap_rd(MSDCPLL_CON3));

	/*
	 * ifrao2 is a set/clr gate pair: writing SET asserts the gate
	 * (disables the clock), writing CLR deasserts it (enables).
	 * The LK leaves ifrao_spi1 enabled (bit 6 = 0).  Only ever use
	 * CLR here; the old code wrote SET and killed the controller.
	 */
	logf("TOUCH: alive raw spi1=%d", (u64)spi_regs_alive());
	infra_wr(IFRAO_SPI1_CLR, IFRAO_SPI1_BIT);
	delay_us(100);
	logf("TOUCH: alive gate1=%d sta=%x",
	     (u64)spi_regs_alive(), (u64)infra_rd(IFRAO_SPI1_STA));

	nonce = spi_nonce();
	logf("TOUCH: nonce cfg0=%x", (u64)nonce);

	mode_old = gpio_rd(GPIO_MODE(TOUCH_SPI_PIN_FIRST));
	for (i = TOUCH_SPI_PIN_FIRST; i <= TOUCH_SPI_PIN_LAST; i++)
		gpio_set_mode(i, TOUCH_SPI_PIN_FUNC);
	mode_new = gpio_rd(GPIO_MODE(TOUCH_SPI_PIN_FIRST));
	logf("TOUCH: mode34 old=%x new=%x alive=%d",
	     (u64)mode_old, (u64)mode_new, (u64)spi_regs_alive());

	/* AVDD / reset / IRQ are plain GPIOs. */
	gpio_set_mode(TOUCH_AVDD_GPIO, 0u);
	gpio_set_mode(TOUCH_RESET_GPIO, 0u);
	gpio_set_mode(TOUCH_IRQ_GPIO, 0u);
	gpio_set_dir(TOUCH_IRQ_GPIO, 0u);

	gpio_set(TOUCH_AVDD_GPIO, 1u);
	gpio_set_dir(TOUCH_AVDD_GPIO, 1u);
	delay_ms(2);

	gpio_set(TOUCH_RESET_GPIO, 0u);
	gpio_set_dir(TOUCH_RESET_GPIO, 1u);
	delay_ms(10);

	gpio_set(TOUCH_RESET_GPIO, 1u);
	delay_ms(150);
	logf("TOUCH: alive power=%d sta=%x irq=%d",
	     (u64)spi_regs_alive(), (u64)infra_rd(IFRAO_SPI1_STA),
	     (u64)gpio_get(TOUCH_IRQ_GPIO));

	idh = 0u;
	idl = 0u;
	rc = -1;
	for (i = 0; i < 5u; i++) {
		idh = 0u;
		idl = 0u;
		rc = fts_read(FTS_REG_CHIP_ID_H, &idh, 1u);
		if (rc == 0)
			rc = fts_read(FTS_REG_CHIP_ID_L, &idl, 1u);
		logf("TOUCH: try%d rc=%d st=%x id=%x%x", (u64)i, (u64)rc,
		     (u64)fts_last_status, (u64)idh, (u64)idl);
		if (rc == 0 && idh == FTS_CHIP_ID_H && idl == FTS_CHIP_ID_L)
			break;
		delay_ms(50);
	}

	if (rc == 0 && idh == FTS_CHIP_ID_H && idl == FTS_CHIP_ID_L) {
		fw = 0u;
		pm = 0u;
		fts_read(FTS_REG_FW_VER, &fw, 1u);
		fts_read(FTS_REG_POWER_MODE, &pm, 1u);
		logf("TOUCH: RESULT OK fw=%x pm=%x", (u64)fw, (u64)pm);

		/*
		 * Live coordinate test: draw a full-width/full-height
		 * crosshair at the first finger for 15 seconds, then let the
		 * normal PSCI reset path run.  Watchdog (30 s) stays armed.
		 */
		{
			u64 start = ticks();
			u64 end = start + 15u * timer_freq();
			int shown = 0;
			int logged = 0;

			logf("TOUCH: crosshair 15s, touch the panel");
			while (ticks() < end) {
				u32 rx = 0u, ry = 0u, sx, sy;
				int down = 0;
				int trc = fts_read_touch(&rx, &ry, &down);

				if (trc == 0 && down) {
					sx = rx / 16u;
					sy = ry / 16u;
					if (sx >= 1080u)
						sx = 1079u;
					if (sy >= 2436u)
						sy = 2435u;
					console_crosshair((int)sx, (int)sy);
					if (!logged) {
						logf("TOUCH: first x=%d y=%d raw=%d,%d",
						     (u64)sx, (u64)sy, (u64)rx, (u64)ry);
						logged = 1;
					}
					shown = 1;
				} else if (shown) {
					console_crosshair_hide();
					shown = 0;
				}
				delay_ms(20);
			}
			if (shown)
				console_crosshair_hide();
			logf("TOUCH: crosshair done");
		}
	} else {
		logf("TOUCH: RESULT FAIL rc=%d st=%x id=%x%x",
		     (u64)rc, (u64)fts_last_status, (u64)idh, (u64)idl);
	}
}
