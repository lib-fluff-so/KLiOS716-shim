/*
 * Read-only touch probe for the KL7 (Tecno Spark 30 Pro, MT6789).
 *
 * Touch panel lives on SPI1 (spi1@11010000, CS0), compatible "focaltech,fts".
 * Reset on GPIO 116, IRQ on GPIO 9, AVDD on GPIO 61.
 *
 * This is a bring-up probe: reset the chip, issue a read-ID over SPI, and log
 * whatever comes back. No writes to the touch device other than SPI commands.
 *
 * Runs with MMU and caches off, so everything is direct physical access.
 *
 * FocalTech SPI protocol (from kernel google-modules/touch/focaltech_touch):
 *   WRITE: [0x00, reg_addr, data...]
 *   READ:  [0xA0, reg_addr, dummy, dummy] -> data bytes
 *   WRITE_CMD = 0x00, READ_CMD = 0xA0 (0x80 | DATA_CRC_EN 0x20)
 *
 * MTK SPI register map (from drivers/spi/spi-mt65xx.c):
 *   SPI_CFG0_REG   0x0000 - clock/config
 *   SPI_CFG1_REG   0x0004 - packet config
 *   SPI_TX_DATA_REG 0x0010 - FIFO write
 *   SPI_RX_DATA_REG 0x0014 - FIFO read
 *   SPI_CMD_REG     0x0018 - control (BIT(0) = ACT)
 *   SPI_STATUS0_REG 0x001c - status
 */
#include "console.h"
#include "touch_test.h"

typedef unsigned char u8;
typedef unsigned int u32;
typedef unsigned long long u64;

#define GPIO_BASE		0x10005000UL	/* gpio@10005000, mediatek,gpio */
#define GPIO_DIR(bank)		(0x0000u + (u32)(bank) * 4u)
#define GPIO_DOUT(bank)		(0x0100u + (u32)(bank) * 4u)
#define GPIO_DIN(bank)		(0x0200u + (u32)(bank) * 4u)
#define GPIO_BANK(n)		((u32)(n) / 32u)
#define GPIO_BIT(n)		((u32)(n) % 32u)

#define SPI1_BASE		0x11010000UL	/* spi1@11010000, mediatek,mt6789-spi */
#define SPI_CFG0_REG		0x0000u
#define SPI_CFG1_REG		0x0004u
#define SPI_TX_DATA_REG		0x0010u
#define SPI_RX_DATA_REG		0x0014u
#define SPI_CMD_REG		0x0018u
#define SPI_STATUS0_REG		0x001cu

/* Touch GPIO pins (from fdt.dtb). */
#define TOUCH_RESET_GPIO	116u	/* focaltech,reset-gpio  = 0x4d 0x74 0x00 */
#define TOUCH_IRQ_GPIO		9u	/* focaltech,irq-gpio    = 0x4d 0x09 0x00 */
#define TOUCH_AVDD_GPIO		61u	/* focaltech,avdd-gpio   = 0x4d 0x3d 0x00 */

/* FocalTech SPI command bytes. */
#define FT_SPI_WRITE_CMD	0x00u
#define FT_SPI_READ_CMD		0xA0u	/* 0x80 | DATA_CRC_EN(0x20) */

/* FocalTech registers. */
#define FT_REG_ID_G		0xA3u	/* device ID (1 byte) */
#define FT_REG_FW_VER		0xA6u	/* firmware version (1 byte) */
#define FT_REG_CHIP_ID		0xA8u	/* chip ID (2 bytes) */

/* SPI_CMD_REG bits. */
#define SPI_CMD_ACT		(1u << 0)	/* activate transfer */
#define SPI_CMD_CPHA		(1u << 8)
#define SPI_CMD_CPOL		(1u << 9)
#define SPI_CMD_TXMSBF		(1u << 12)
#define SPI_CMD_RXMSBF		(1u << 13)

static inline u32 gpio_rd(u32 off)
{
	return *(volatile u32 *)(GPIO_BASE + off);
}

static inline void gpio_wr(u32 off, u32 v)
{
	*(volatile u32 *)(GPIO_BASE + off) = v;
}

static inline u32 spi_rd(u32 off)
{
	return *(volatile u32 *)(SPI1_BASE + off);
}

static inline void spi_wr(u32 off, u32 v)
{
	*(volatile u32 *)(SPI1_BASE + off) = v;
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

/* Configure one GPIO pin as input (0) or output (1). */
static void gpio_set_dir(u32 pin, u32 is_out)
{
	u32 bank = GPIO_BANK(pin);
	u32 bit = GPIO_BIT(pin);
	u32 v = gpio_rd(GPIO_DIR(bank));
	if (is_out)
		v |= (1u << bit);
	else
		v &= ~(1u << bit);
	gpio_wr(GPIO_DIR(bank), v);
}

static void gpio_set(u32 pin, u32 val)
{
	u32 bank = GPIO_BANK(pin);
	u32 bit = GPIO_BIT(pin);
	u32 v = gpio_rd(GPIO_DOUT(bank));
	if (val)
		v |= (1u << bit);
	else
		v &= ~(1u << bit);
	gpio_wr(GPIO_DOUT(bank), v);
}

static u32 gpio_get(u32 pin)
{
	u32 bank = GPIO_BANK(pin);
	u32 bit = GPIO_BIT(pin);
	return (gpio_rd(GPIO_DIN(bank)) >> bit) & 1u;
}

/*
 * SPI write-one-byte: write tx byte to FIFO, activate transfer with ACT bit,
 * wait for completion, drain RX.
 * Returns 0 on success, nonzero on timeout.
 */
static int spi_write_byte(u8 tx)
{
	spi_wr(SPI_TX_DATA_REG, tx);
	spi_wr(SPI_CMD_REG, SPI_CMD_ACT);

	u64 start = ticks();
	u64 timeout = timer_freq() / 10; /* 100 ms */
	while (!(spi_rd(SPI_STATUS0_REG) & (1u << 0))) {
		if (ticks() - start > timeout)
			return 1;
	}
	(void)spi_rd(SPI_RX_DATA_REG); /* drain RX */
	return 0;
}

/*
 * SPI read-one-byte: write cmd byte, activate, wait, read RX byte.
 * Returns the received byte or 0xff on timeout.
 */
static u8 spi_read_byte(u8 tx)
{
	if (spi_write_byte(tx))
		return 0xff;
	return (u8)spi_rd(SPI_RX_DATA_REG);
}

/* Combined: write cmd bytes, then read rlen response bytes. */
static int spi_wr_then_rd(const u8 *wr_tx, u32 wr_tlen,
                          const u8 *rd_tx, u32 rd_tlen,
                          u8 *rx, u32 rlen)
{
	int rc;

	for (u32 i = 0; i < wr_tlen; i++) {
		rc = spi_write_byte(wr_tx[i]);
		if (rc)
			return rc;
	}

	for (u32 i = 0; i < rlen; i++) {
		rx[i] = spi_read_byte(rd_tx[i < rd_tlen ? i : rd_tlen - 1]);
		if (rx[i] == 0xff)
			return 2;
	}
	return 0;
}

static void touch_reset(void)
{
	gpio_set_dir(TOUCH_RESET_GPIO, 1);
	gpio_set(TOUCH_RESET_GPIO, 0); /* assert reset */
	delay_ms(10);
	gpio_set(TOUCH_RESET_GPIO, 1); /* release reset */
	delay_ms(50);                  /* wait for chip to boot */
}

void touch_test(void)
{
	u8 rx[2];
	int rc;

	logf("TOUCH TEST: spi1=0x%x gpio=0x%x", SPI1_BASE, GPIO_BASE);
	logf("TOUCH: reset=gpio%d irq=gpio%d avdd=gpio%d",
	     TOUCH_RESET_GPIO, TOUCH_IRQ_GPIO, TOUCH_AVDD_GPIO);

	/* Configure IRQ as input. */
	gpio_set_dir(TOUCH_IRQ_GPIO, 0);
	logf("TOUCH: irq gpio%d dir=input din=%d",
	     TOUCH_IRQ_GPIO, gpio_get(TOUCH_IRQ_GPIO));

	/* Reset the chip. */
	touch_reset();
	logf("TOUCH: reset pulsed");

	/* Configure SPI: mode 0, 8-bit, CS active low, MSB first. */
	spi_wr(SPI_CFG0_REG, 0x00000000u); /* default clock div */
	spi_wr(SPI_CFG1_REG, 0x00000000u);
	spi_wr(SPI_CMD_REG, SPI_CMD_CPOL | SPI_CMD_CPHA |
	               SPI_CMD_TXMSBF | SPI_CMD_RXMSBF);
	delay_us(100);

	/* Dummy clocks to wake up the SPI slave. */
	for (int i = 0; i < 10; i++) {
		u8 dummy = 0x00u;
		spi_write_byte(dummy);
	}

	/* Attempt 1: FocalTech read ID (reg 0xA3).
	 * WRITE [0x00, 0xA3], then READ [0xA0, 0xA3, 0x00, 0x00] -> 2 bytes. */
	u8 wr1[] = { FT_SPI_WRITE_CMD, FT_REG_ID_G };
	u8 rd1[] = { FT_SPI_READ_CMD, FT_REG_ID_G, 0x00, 0x00 };
	rc = spi_wr_then_rd(wr1, sizeof(wr1), rd1, sizeof(rd1), rx, 2);
	logf("TOUCH: read ID reg=0xA3 rc=%d id=%x %x irq=%d",
	     (u64)rc, (u64)rx[0], (u64)rx[1], (u64)gpio_get(TOUCH_IRQ_GPIO));

	/* Attempt 2: read FW version (reg 0xA6). */
	u8 wr2[] = { FT_SPI_WRITE_CMD, FT_REG_FW_VER };
	u8 rd2[] = { FT_SPI_READ_CMD, FT_REG_FW_VER, 0x00, 0x00 };
	rc = spi_wr_then_rd(wr2, sizeof(wr2), rd2, sizeof(rd2), rx, 2);
	logf("TOUCH: read FW reg=0xA6 rc=%d fw=%x %x",
	     (u64)rc, (u64)rx[0], (u64)rx[1]);

	/* Attempt 3: read chip ID (reg 0xA8, 2 bytes). */
	u8 wr3[] = { FT_SPI_WRITE_CMD, FT_REG_CHIP_ID };
	u8 rd3[] = { FT_SPI_READ_CMD, FT_REG_CHIP_ID, 0x00, 0x00 };
	rc = spi_wr_then_rd(wr3, sizeof(wr3), rd3, sizeof(rd3), rx, 2);
	logf("TOUCH: read CHIP reg=0xA8 rc=%d chip=%x %x",
	     (u64)rc, (u64)rx[0], (u64)rx[1]);

	logf("TOUCH: irq_level=%d", gpio_get(TOUCH_IRQ_GPIO));
	logf("TOUCH TEST: end");
}
