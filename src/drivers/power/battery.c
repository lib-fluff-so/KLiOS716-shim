/*
 * Read-only battery telemetry probe for KL7 (MT6789 + MT6366 PMIC).
 *
 * The PMIC is reached through the MT6789 PMIC wrapper at 0x10026000, using
 * the same WACS2 sequence as the working key/vibrator code in XNU.  The
 * AUXADC part only reads PMIC state and issues a normal AUXADC conversion
 * request for the battery, battery-temperature and chip-temperature channels.
 * It does not touch the SGM41516D charger, the charger watchdog or any
 * charging setting.
 *
 * The second part is a separate SGM41516D probe over i2c5.  It is read-only:
 * registers 0x00..0x0f are read with a register-address write followed by a
 * repeated-start read.  No charger configuration register is written.
 *
 * AUXADC facts come from the MT6358 vendor/mainline driver:
 *   BATADC   : request RQST0 bit 0, output ADC0, 15-bit, ratio 3:1
 *   VCDT     : request RQST0 bit 0, output ADC2, 12-bit, ratio 1:1
 *   BAT_TEMP : request RQST0 bit 3, output ADC3, 12-bit, ratio 2:1
 *   CHIP_TEMP: request RQST0 bit 4, output ADC4, 12-bit, ratio 1:1
 *   request  : write BIT(req_bit) to AUXADC_RQST0
 *   stop     : write 0 to AUXADC_RQST0 after reading the result
 *   ready    : bit 15 of the channel output register
 *   wait     : avg_num * 10 us, then up to 160 * (100..200 us)
 *   voltage  : raw * r0 * 1800 mV / (r1 << resolution)
 *
 * VCDT shares the BATADC request bit on MT6358; BATADC and VCDT are two
 * different output registers produced by the same battery-interface
 * conversion.  BAT_TEMP is the raw NTC voltage, not a temperature.
 *
 * The result is nominal, without the PMIC efuse calibration that Android
 * applies.  The raw values are logged as well, so calibration can be added
 * after this first access check.
 */
#include "console.h"
#include "battery.h"

#ifndef CHARGER_WRITE_TEST
#define CHARGER_WRITE_TEST 0
#endif

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long long u64;

/* MT6789 PMIC wrapper (same layout as xnu/patches/0113). */
#define KL7_PWRAP_PA		0x10026000UL
#define PWRAP_INIT_DONE2	0x0000u
#define PWRAP_WACS2_CMD		0x0880u
#define PWRAP_SWINF_2_WDATA	0x0884u
#define PWRAP_SWINF_2_RDATA	0x0894u
#define PWRAP_WACS2_VLDCLR	0x08a4u
#define PWRAP_WACS2_RDATA	0x08a8u

#define PWRAP_TIMEOUT_US	10000u
#define PWRAP_FSM_READY		0u
#define PWRAP_FSM_READ_DONE	6u

/* MT6358/MT6366 PMIC registers (16-bit addresses behind PWRAP). */
#define MT6358_SWCID		0x000au
#define MT6358_TOPSTATUS	0x0028u
#define MT6358_HK_TOP_CLK_CON0	0x0f8cu
#define MT6358_HK_TOP_CLK_CON1	0x0f8eu
#define MT6358_HK_TOP_RST_CON0	0x0f90u
#define MT6358_AUXADC_ANA_CON0	0x1008u
#define MT6358_AUXADC_ADC0	0x1088u
#define MT6358_AUXADC_ADC2	0x108cu
#define MT6358_AUXADC_ADC3	0x108eu
#define MT6358_AUXADC_ADC4	0x1090u
#define MT6358_AUXADC_RQST0	0x1108u
#define MT6358_AUXADC_CON0	0x1188u

#define AUXADC_RDY_SHIFT	15u
#define AUXADC_AVG_TIME_US	10u
#define AUXADC_MAX_POLLS	160u
#define AUXADC_POLL_US		150u
#define AUXADC_VOLT_FULL	1800u

/*
 * SGM41516D on i2c5 (0x11017000), address 0x6b.
 *
 * Clock tree from the live DTB and the downstream MT6789 clock driver:
 *   topckgen i2c_sel (CLK_CFG_10) -> i2c_ck -> ifrao_i2c_pseudo
 *   -> i2c_pseudo_ck -> impc_ap_clock_i2c5
 * The top mux is gated by CLK_CFG_10 bit 15 in the active clock table.
 * Gate enable is a write to the CLR register; status 0 means enabled.
 *
 * Pinmux: SCL5 = GPIO140, SDA5 = GPIO141, function 1.
 *
 * i2c controller offsets are the mt6983/mt_i2c_regs_v2 layout from
 * drivers/i2c/busses/i2c-mt65xx.c.  A 400 kHz transfer is used because the
 * driver's AC-timing search cannot produce a valid 100 kHz setting from the
 * 124.8 MHz parent with this controller.
 */
#define I2C5_BASE		0x11017000UL
#define TOPCKGEN_BASE		0x10000000UL
#define INFRACFG_AO_BASE	0x10001000UL
#define IMP_IIC_WRAP_C_BASE	0x1101b000UL
#define GPIO_BASE		0x10005000UL

#define TOP_CLK_CFG_10		0x00b0u
#define TOP_CLK_CFG_10_SET	0x00b4u
#define TOP_CLK_CFG_10_CLR	0x00b8u
#define TOP_CLK_CFG_UPDATE1	0x0008u
#define TOP_I2C_MUX_SHIFT	8u
#define TOP_I2C_MUX_MASK	0x0300u
#define TOP_I2C_MUX_UPDATE	10u
#define TOP_I2C_MUX_GATE	15u
#define TOP_I2C_PARENT_MAINPLL	1u
#define I2C_MUX_UPDATE_BIT	(1u << TOP_I2C_MUX_UPDATE)
#define I2C_MUX_GATE_BIT	(1u << TOP_I2C_MUX_GATE)

#define IFRAO_GATE_CLR		0x0084u
#define IFRAO_GATE_STA		0x0090u
#define IFRAO_I2C_PSEUDO_BIT	(1u << 11)
#define IMPC_GATE_CLR		0x0e04u
#define IMPC_GATE_STA		0x0e00u
#define IMPC_I2C5_BIT		(1u << 1)

#define I2C5_SCL_GPIO		140u
#define I2C5_SDA_GPIO		141u
#define I2C5_PIN_FUNC		1u
#define GPIO_MODE_REG(pin)	(0x0300u + ((u32)(pin) / 8u) * 0x10u)
#define GPIO_MODE_SHIFT(pin)	(((u32)(pin) % 8u) * 4u)
#define GPIO_MODE_MASK		0x0fu

#define I2C5_SLAVE_ADDR		0x6bu

#define I2C_DATA_PORT		0x0000u
#define I2C_SLAVE_ADDR1		0x0094u
#define I2C_INTR_MASK		0x0008u
#define I2C_INTR_STAT		0x000cu
#define I2C_CONTROL		0x0010u
#define I2C_TRANSFER_LEN	0x0014u
#define I2C_TRANSAC_LEN		0x0018u
#define I2C_DELAY_LEN		0x001cu
#define I2C_TIMING		0x0020u
#define I2C_START		0x0024u
#define I2C_EXT_CONF		0x0028u
#define I2C_LTIMING		0x002cu
#define I2C_HS			0x0030u
#define I2C_IO_CONFIG		0x0034u
#define I2C_FIFO_ADDR_CLR	0x0038u
#define I2C_SDA_TIMING		0x003cu
#define I2C_TRANSFER_LEN_AUX	0x0044u
#define I2C_CLOCK_DIV		0x0048u
#define I2C_SOFTRESET		0x0050u
#define I2C_SCL_MIS_COMP_POINT	0x0090u
#define I2C_FIFO_STAT		0x00f4u
#define I2C_DEBUGSTAT		0x00e4u

#define I2C_CTRL_RS		(1u << 1)
#define I2C_CTRL_DMA_EN		(1u << 2)
#define I2C_CTRL_CLK_EXT	(1u << 3)
#define I2C_CTRL_DIR_CHANGE	(1u << 4)
#define I2C_CTRL_ACKERR		(1u << 5)
#define I2C_CTRL_DMAACK		(1u << 8)
#define I2C_CTRL_ASYNC		(1u << 9)

#define I2C_INTR_ARB_LOST	(1u << 3)
#define I2C_INTR_HS_NACKERR	(1u << 2)
#define I2C_INTR_ACKERR		(1u << 1)
#define I2C_INTR_COMP		(1u << 0)
#define I2C_INTR_ALL		0x001fu

#define I2C_START_TRANSAC	(1u << 0)
#define I2C_SOFT_RST		(1u << 0)
#define I2C_FIFO_ADDR_CLR_VAL	(1u << 0)
#define I2C_IO_OPEN_DRAIN	0x0003u
#define I2C_CONTROL_FIFO	(I2C_CTRL_ACKERR | I2C_CTRL_CLK_EXT)
#define I2C_DELAY_LEN_VAL	0x0002u

/* 400 kHz from 124.8 MHz (mtk_i2c_calculate_speed + check_ac_timing). */
#define I2C5_TIMING_400K	0x0229u
#define I2C5_LTIMING_400K	0x00bdu
#define I2C5_EXT_CONF_400K	0x5e01u
#define I2C5_SDA_TIMING_400K	0x100fu

#define I2C5_TIMEOUT_US		100000u

static u64
ticks(void)
{
	u64 t;

	__asm__ volatile("isb; mrs %0, cntvct_el0" : "=r"(t));
	return t;
}

static u64
timer_freq(void)
{
	u64 f;

	__asm__ volatile("mrs %0, cntfrq_el0" : "=r"(f));
	return f ? f : 19200000u;
}

static void
delay_us(u64 us)
{
	u64 start = ticks();
	u64 delta = us * timer_freq() / 1000000u;

	while (ticks() - start < delta)
		;
}

static u32
pwrap_rd(u32 off)
{
	u32 v = *(volatile u32 *)(KL7_PWRAP_PA + off);

	__asm__ volatile("dmb oshld" ::: "memory");
	return v;
}

static void
pwrap_wr(u32 off, u32 val)
{
	__asm__ volatile("dmb oshst" ::: "memory");
	*(volatile u32 *)(KL7_PWRAP_PA + off) = val;
	__asm__ volatile("dsb osh" ::: "memory");
}

static int
pwrap_wait_state(u32 want)
{
	u64 start = ticks();
	u64 timeout = (u64)PWRAP_TIMEOUT_US * timer_freq() / 1000000u;

	for (;;) {
		u32 v = pwrap_rd(PWRAP_WACS2_RDATA);
		if (((v >> 1) & 7u) == want)
			return 0;
		if (ticks() - start >= timeout)
			return -1;
		delay_us(1);
	}
}

static int
pwrap_read16(u16 adr, u16 *val)
{
	if (pwrap_wait_state(PWRAP_FSM_READY))
		return -1;
	pwrap_wr(PWRAP_WACS2_CMD, adr);
	if (pwrap_wait_state(PWRAP_FSM_READ_DONE)) {
		pwrap_wr(PWRAP_WACS2_VLDCLR, 1);
		return -2;
	}
	*val = (u16)(pwrap_rd(PWRAP_SWINF_2_RDATA) & 0xffffu);
	pwrap_wr(PWRAP_WACS2_VLDCLR, 1);
	return 0;
}

static int
pwrap_write16(u16 adr, u16 val)
{
	if (pwrap_wait_state(PWRAP_FSM_READY))
		return -1;
	pwrap_wr(PWRAP_SWINF_2_WDATA, val);
	pwrap_wr(PWRAP_WACS2_CMD, (1u << 29) | adr);
	return 0;
}

static int
pmic_read(const char *name, u16 adr, u16 *val)
{
	int rc = pwrap_read16(adr, val);

	if (rc == 0)
		return 0;
	logf("BAT: %s read failed rc=%d", name, (u64)(rc < 0 ? -rc : rc));
	return rc;
}

static void
auxadc_stop(void)
{
	pwrap_write16(MT6358_AUXADC_RQST0, 0);
	/* Let the stop command leave the PWRAP FSM before the next request. */
	delay_us(10);
}

static int
auxadc_read(const char *name, u32 req_bit, u16 out_reg, u32 res, u32 avg_num,
	    u32 *out)
{
	u16 v = 0;
	u32 i;
	int rc;

	rc = pwrap_write16(MT6358_AUXADC_RQST0, (u16)(1u << req_bit));
	if (rc) {
		logf("BAT: %s request failed rc=%d", name,
		     (u64)(rc < 0 ? -rc : rc));
		return rc;
	}

	delay_us((u64)avg_num * AUXADC_AVG_TIME_US);
	for (i = 0; i < AUXADC_MAX_POLLS; i++) {
		rc = pwrap_read16(out_reg, &v);
		if (rc) {
			logf("BAT: %s result read failed rc=%d", name,
			     (u64)(rc < 0 ? -rc : rc));
			auxadc_stop();
			return rc;
		}
		if ((v >> AUXADC_RDY_SHIFT) & 1u) {
			*out = v & ((1u << res) - 1u);
			auxadc_stop();
			return 0;
		}
		delay_us(AUXADC_POLL_US);
	}

	*out = v & ((1u << res) - 1u);
	auxadc_stop();
	logf("BAT: %s timeout last=%x", name, (u64)v);
	return -3;
}

static u64
auxadc_mv10(u32 raw, u32 r0, u32 r1, u32 res)
{
	return (u64)raw * r0 * (AUXADC_VOLT_FULL * 10u) /
	       ((u64)r1 << res);
}

static void
log_adc_mv(const char *name, u32 raw, u32 r0, u32 r1, u32 res)
{
	u64 mv10 = auxadc_mv10(raw, r0, r1, res);

	logf("BAT: %s raw=%x mv=%d.%d", name, (u64)raw,
	     mv10 / 10u, mv10 % 10u);
}

static u32
reg_rd32(u64 base, u32 off)
{
	u32 v = *(volatile u32 *)(base + off);

	__asm__ volatile("dmb oshld" ::: "memory");
	return v;
}

static void
reg_wr32(u64 base, u32 off, u32 val)
{
	__asm__ volatile("dmb oshst" ::: "memory");
	*(volatile u32 *)(base + off) = val;
	__asm__ volatile("dsb osh" ::: "memory");
}

static u16
i2c5_rd16(u32 off)
{
	u16 v = *(volatile u16 *)(I2C5_BASE + off);

	__asm__ volatile("dmb oshld" ::: "memory");
	return v;
}

static void
i2c5_wr16(u32 off, u16 val)
{
	__asm__ volatile("dmb oshst" ::: "memory");
	*(volatile u16 *)(I2C5_BASE + off) = val;
	__asm__ volatile("dsb osh" ::: "memory");
}

static u8
i2c5_rd8(u32 off)
{
	u8 v = *(volatile u8 *)(I2C5_BASE + off);

	__asm__ volatile("dmb oshld" ::: "memory");
	return v;
}

static void
i2c5_wr8(u32 off, u8 val)
{
	__asm__ volatile("dmb oshst" ::: "memory");
	*(volatile u8 *)(I2C5_BASE + off) = val;
	__asm__ volatile("dsb osh" ::: "memory");
}

static void
i2c5_reset(void)
{
	i2c5_wr16(I2C_START, 0);
	i2c5_wr16(I2C_INTR_STAT, I2C_INTR_ALL);
	i2c5_wr16(I2C_SOFTRESET, I2C_SOFT_RST);
	delay_us(50);
	i2c5_wr16(I2C_SOFTRESET, 0);
	delay_us(50);
	i2c5_wr16(I2C_INTR_MASK, 0xffffu);
	i2c5_wr16(I2C_FIFO_ADDR_CLR, I2C_FIFO_ADDR_CLR_VAL);
}

static void
i2c5_init(void)
{
	i2c5_reset();
	i2c5_wr16(I2C_IO_CONFIG, I2C_IO_OPEN_DRAIN);
	i2c5_wr16(I2C_TIMING, I2C5_TIMING_400K);
	i2c5_wr16(I2C_HS, 0);
	i2c5_wr16(I2C_LTIMING, I2C5_LTIMING_400K);
	i2c5_wr16(I2C_CLOCK_DIV, 0);
	i2c5_wr16(I2C_SCL_MIS_COMP_POINT, 0);
	i2c5_wr16(I2C_SDA_TIMING, I2C5_SDA_TIMING_400K);
	i2c5_wr16(I2C_EXT_CONF, I2C5_EXT_CONF_400K);
	i2c5_wr16(I2C_CONTROL, I2C_CONTROL_FIFO);
	i2c5_wr16(I2C_DELAY_LEN, I2C_DELAY_LEN_VAL);
	i2c5_wr16(I2C_TRANSFER_LEN, 0);
	i2c5_wr16(I2C_TRANSFER_LEN_AUX, 0);
	i2c5_wr16(I2C_TRANSAC_LEN, 0);
	i2c5_wr16(I2C_INTR_MASK, 0xffffu);
	i2c5_wr16(I2C_FIFO_ADDR_CLR, I2C_FIFO_ADDR_CLR_VAL);
}

/* Read one SGM41516D register.  This is a combined write-address/read-data
 * transfer: the only byte sent to the charger is the register number. */
static int
i2c5_read_reg(u8 addr, u8 reg, u8 *val, u16 *stat_out)
{
	u16 ctl, stat;
	u64 start, timeout;

	ctl = i2c5_rd16(I2C_CONTROL);
	ctl &= (u16)~(I2C_CTRL_DMA_EN | I2C_CTRL_DMAACK | I2C_CTRL_ASYNC);
	ctl |= (u16)(I2C_CTRL_DIR_CHANGE | I2C_CTRL_RS | I2C_CTRL_ACKERR |
		     I2C_CTRL_CLK_EXT);
	i2c5_wr16(I2C_CONTROL, ctl);
	i2c5_wr16(I2C_SLAVE_ADDR1, (u16)((u16)addr << 1));
	i2c5_wr16(I2C_INTR_STAT, I2C_INTR_ALL);
	i2c5_wr16(I2C_FIFO_ADDR_CLR, I2C_FIFO_ADDR_CLR_VAL);
	i2c5_wr16(I2C_INTR_MASK, I2C_INTR_ALL);
	i2c5_wr16(I2C_TRANSFER_LEN, 1);
	i2c5_wr16(I2C_TRANSFER_LEN_AUX, 1);
	i2c5_wr16(I2C_TRANSAC_LEN, 2);
	i2c5_wr8(I2C_DATA_PORT, reg);
	i2c5_wr16(I2C_START, I2C_START_TRANSAC);
	__asm__ volatile("dsb sy" ::: "memory");

	timeout = (u64)I2C5_TIMEOUT_US * timer_freq() / 1000000u;
	start = ticks();
	for (;;) {
		stat = i2c5_rd16(I2C_INTR_STAT);
		if (stat & (I2C_INTR_COMP | I2C_INTR_ACKERR |
			    I2C_INTR_HS_NACKERR | I2C_INTR_ARB_LOST))
			break;
		if (ticks() - start >= timeout) {
			*stat_out = stat;
			i2c5_wr16(I2C_INTR_MASK, 0xffffu);
			i2c5_reset();
			return -1;
		}
	}
	*stat_out = stat;
	i2c5_wr16(I2C_INTR_MASK, 0xffffu);
	if (stat & (I2C_INTR_ACKERR | I2C_INTR_HS_NACKERR |
		    I2C_INTR_ARB_LOST)) {
		i2c5_reset();
		return -2;
	}
	if (!(stat & I2C_INTR_COMP)) {
		i2c5_reset();
		return -3;
	}
	*val = i2c5_rd8(I2C_DATA_PORT);
	i2c5_wr16(I2C_INTR_STAT, I2C_INTR_ALL);
	i2c5_wr16(I2C_START, 0);
	i2c5_wr16(I2C_FIFO_ADDR_CLR, I2C_FIFO_ADDR_CLR_VAL);
	return 0;
}

#if CHARGER_WRITE_TEST
/* Write one SGM41516D register.  This is a plain two-byte FIFO write:
 * register address followed by the value, one I2C transaction. */
static int
i2c5_write_reg(u8 addr, u8 reg, u8 val, u16 *stat_out)
{
	u16 ctl, stat;
	u64 start, timeout;

	ctl = i2c5_rd16(I2C_CONTROL);
	ctl &= (u16)~(I2C_CTRL_DMA_EN | I2C_CTRL_DMAACK | I2C_CTRL_ASYNC |
		      I2C_CTRL_DIR_CHANGE | I2C_CTRL_RS);
	ctl |= (u16)(I2C_CTRL_ACKERR | I2C_CTRL_CLK_EXT);
	i2c5_wr16(I2C_CONTROL, ctl);
	i2c5_wr16(I2C_SLAVE_ADDR1, (u16)((u16)addr << 1));
	i2c5_wr16(I2C_INTR_STAT, I2C_INTR_ALL);
	i2c5_wr16(I2C_FIFO_ADDR_CLR, I2C_FIFO_ADDR_CLR_VAL);
	i2c5_wr16(I2C_INTR_MASK, I2C_INTR_ALL);
	i2c5_wr16(I2C_TRANSFER_LEN, 2);
	i2c5_wr16(I2C_TRANSFER_LEN_AUX, 0);
	i2c5_wr16(I2C_TRANSAC_LEN, 1);
	i2c5_wr8(I2C_DATA_PORT, reg);
	i2c5_wr8(I2C_DATA_PORT, val);
	i2c5_wr16(I2C_START, I2C_START_TRANSAC);
	__asm__ volatile("dsb sy" ::: "memory");

	timeout = (u64)I2C5_TIMEOUT_US * timer_freq() / 1000000u;
	start = ticks();
	for (;;) {
		stat = i2c5_rd16(I2C_INTR_STAT);
		if (stat & (I2C_INTR_COMP | I2C_INTR_ACKERR |
			    I2C_INTR_HS_NACKERR | I2C_INTR_ARB_LOST))
			break;
		if (ticks() - start >= timeout) {
			*stat_out = stat;
			i2c5_wr16(I2C_INTR_MASK, 0xffffu);
			i2c5_reset();
			return -1;
		}
	}
	*stat_out = stat;
	i2c5_wr16(I2C_INTR_MASK, 0xffffu);
	if (stat & (I2C_INTR_ACKERR | I2C_INTR_HS_NACKERR |
		    I2C_INTR_ARB_LOST)) {
		i2c5_reset();
		return -2;
	}
	if (!(stat & I2C_INTR_COMP)) {
		i2c5_reset();
		return -3;
	}
	i2c5_wr16(I2C_INTR_STAT, I2C_INTR_ALL);
	i2c5_wr16(I2C_START, 0);
	i2c5_wr16(I2C_FIFO_ADDR_CLR, I2C_FIFO_ADDR_CLR_VAL);
	return 0;
}

static u32
sgm_cv_mv(u8 reg04, u8 reg0f)
{
	u32 n = ((u32)reg04 >> 3) & 0x1fu;
	u32 mv;

	if (n == 15u)
		mv = 4352u;
	else {
		if (n > 24u)
			n = 24u;
		mv = 3856u + 32u * n;
	}
	switch ((reg0f >> 6) & 3u) {
	case 1u:
		mv += 8u;
		break;
	case 2u:
		mv -= 8u;
		break;
	case 3u:
		mv -= 16u;
		break;
	default:
		break;
	}
	return mv;
}
#endif /* CHARGER_WRITE_TEST */

static void
charger_test(void)
{
	u32 top_pre, ifrao_pre, impc_pre, gpio_pre, gpio_now, gpio_new;
	u32 gpio_off, scl_shift, sda_shift;
	u16 stat = 0;
	u8 regs[16];
	u32 i, ok = 0;
	int rc;

	__asm__ volatile("msr daifset, #0xf" ::: "memory");
#if CHARGER_WRITE_TEST
	logf("CHG: SGM41516D write/readback probe over i2c5 addr=6b");
	logf("CHG: targets 4.200 V, 480 mA, IINDPM 500 mA, OTG off");
	logf("CHG: IRQs masked; watchdog 160 s petted; boot-arg gated in XNU");
#else
	logf("CHG: read-only SGM41516D probe over i2c5 addr=6b");
	logf("CHG: IRQs masked; charger data writes disabled");
#endif

	gpio_off = GPIO_MODE_REG(I2C5_SCL_GPIO);
	scl_shift = GPIO_MODE_SHIFT(I2C5_SCL_GPIO);
	sda_shift = GPIO_MODE_SHIFT(I2C5_SDA_GPIO);

	top_pre = reg_rd32(TOPCKGEN_BASE, TOP_CLK_CFG_10);
	ifrao_pre = reg_rd32(INFRACFG_AO_BASE, IFRAO_GATE_STA);
	impc_pre = reg_rd32(IMP_IIC_WRAP_C_BASE, IMPC_GATE_STA);
	gpio_pre = reg_rd32(GPIO_BASE, gpio_off);
	logf("CHG: pre top=%x mux=%d gate=%d ifrao=%x impc=%x gpio=%x",
	     (u64)top_pre, (u64)((top_pre >> TOP_I2C_MUX_SHIFT) & 3u),
	     (u64)((top_pre >> TOP_I2C_MUX_GATE) & 1u), (u64)ifrao_pre,
	     (u64)impc_pre, (u64)gpio_pre);
	logf("CHG: gpio old scl=%d sda=%d",
	     (u64)((gpio_pre >> scl_shift) & GPIO_MODE_MASK),
	     (u64)((gpio_pre >> sda_shift) & GPIO_MODE_MASK));

	if (((top_pre >> TOP_I2C_MUX_SHIFT) & 3u) != TOP_I2C_PARENT_MAINPLL) {
		reg_wr32(TOPCKGEN_BASE, TOP_CLK_CFG_10_CLR, TOP_I2C_MUX_MASK);
		reg_wr32(TOPCKGEN_BASE, TOP_CLK_CFG_10_SET,
			 TOP_I2C_PARENT_MAINPLL << TOP_I2C_MUX_SHIFT);
		reg_wr32(TOPCKGEN_BASE, TOP_CLK_CFG_UPDATE1, I2C_MUX_UPDATE_BIT);
		delay_us(20);
	}
	reg_wr32(TOPCKGEN_BASE, TOP_CLK_CFG_10_CLR, I2C_MUX_GATE_BIT);
	reg_wr32(TOPCKGEN_BASE, TOP_CLK_CFG_UPDATE1, I2C_MUX_UPDATE_BIT);
	reg_wr32(INFRACFG_AO_BASE, IFRAO_GATE_CLR, IFRAO_I2C_PSEUDO_BIT);
	reg_wr32(IMP_IIC_WRAP_C_BASE, IMPC_GATE_CLR, IMPC_I2C5_BIT);
	delay_us(50);

	top_pre = reg_rd32(TOPCKGEN_BASE, TOP_CLK_CFG_10);
	ifrao_pre = reg_rd32(INFRACFG_AO_BASE, IFRAO_GATE_STA);
	impc_pre = reg_rd32(IMP_IIC_WRAP_C_BASE, IMPC_GATE_STA);
	logf("CHG: clk post top=%x mux=%d gate=%d ifrao=%x impc=%x",
	     (u64)top_pre, (u64)((top_pre >> TOP_I2C_MUX_SHIFT) & 3u),
	     (u64)((top_pre >> TOP_I2C_MUX_GATE) & 1u), (u64)ifrao_pre,
	     (u64)impc_pre);
	if (((top_pre >> TOP_I2C_MUX_SHIFT) & 3u) != TOP_I2C_PARENT_MAINPLL ||
	    ((top_pre >> TOP_I2C_MUX_GATE) & 1u) != 0u ||
	    (ifrao_pre & IFRAO_I2C_PSEUDO_BIT) != 0u ||
	    (impc_pre & IMPC_I2C5_BIT) != 0u) {
		logf("CHG: clock enable not confirmed; i2c untouched");
		return;
	}

	gpio_new = gpio_pre;
	gpio_new &= ~(GPIO_MODE_MASK << scl_shift);
	gpio_new |= I2C5_PIN_FUNC << scl_shift;
	gpio_new &= ~(GPIO_MODE_MASK << sda_shift);
	gpio_new |= I2C5_PIN_FUNC << sda_shift;
	if (gpio_new != gpio_pre)
		reg_wr32(GPIO_BASE, gpio_off, gpio_new);
	delay_us(20);
	gpio_now = reg_rd32(GPIO_BASE, gpio_off);
	logf("CHG: gpio new scl=%d sda=%d reg=%x",
	     (u64)((gpio_now >> scl_shift) & GPIO_MODE_MASK),
	     (u64)((gpio_now >> sda_shift) & GPIO_MODE_MASK),
	     (u64)gpio_now);
	if (((gpio_now >> scl_shift) & GPIO_MODE_MASK) != I2C5_PIN_FUNC ||
	    ((gpio_now >> sda_shift) & GPIO_MODE_MASK) != I2C5_PIN_FUNC) {
		logf("CHG: pinmux not confirmed; restoring and aborting");
		reg_wr32(GPIO_BASE, gpio_off, gpio_pre);
		return;
	}

	logf("CHG: i2c pre sa=%x ctl=%x tl=%x aux=%x txn=%x start=%x stat=%x mask=%x",
	     (u64)i2c5_rd16(I2C_SLAVE_ADDR1), (u64)i2c5_rd16(I2C_CONTROL),
	     (u64)i2c5_rd16(I2C_TRANSFER_LEN),
	     (u64)i2c5_rd16(I2C_TRANSFER_LEN_AUX),
	     (u64)i2c5_rd16(I2C_TRANSAC_LEN), (u64)i2c5_rd16(I2C_START),
	     (u64)i2c5_rd16(I2C_INTR_STAT), (u64)i2c5_rd16(I2C_INTR_MASK));
	logf("CHG: i2c pre timing=%x ltiming=%x hs=%x ext=%x clkdiv=%x sda=%x io=%x fifo=%x dbg=%x",
	     (u64)i2c5_rd16(I2C_TIMING), (u64)i2c5_rd16(I2C_LTIMING),
	     (u64)i2c5_rd16(I2C_HS), (u64)i2c5_rd16(I2C_EXT_CONF),
	     (u64)i2c5_rd16(I2C_CLOCK_DIV), (u64)i2c5_rd16(I2C_SDA_TIMING),
	     (u64)i2c5_rd16(I2C_IO_CONFIG), (u64)i2c5_rd16(I2C_FIFO_STAT),
	     (u64)i2c5_rd16(I2C_DEBUGSTAT));

	i2c5_init();
	logf("CHG: i2c init ctl=%x timing=%x ltiming=%x hs=%x ext=%x clkdiv=%x sda=%x io=%x",
	     (u64)i2c5_rd16(I2C_CONTROL), (u64)i2c5_rd16(I2C_TIMING),
	     (u64)i2c5_rd16(I2C_LTIMING), (u64)i2c5_rd16(I2C_HS),
	     (u64)i2c5_rd16(I2C_EXT_CONF), (u64)i2c5_rd16(I2C_CLOCK_DIV),
	     (u64)i2c5_rd16(I2C_SDA_TIMING), (u64)i2c5_rd16(I2C_IO_CONFIG));

	for (i = 0; i < 16u; i++) {
		rc = i2c5_read_reg(I2C5_SLAVE_ADDR, (u8)i, &regs[i], &stat);
		if (rc) {
			logf("CHG: reg[%x] FAILED rc=%d stat=%x", (u64)i,
			     (u64)(rc < 0 ? -rc : rc), (u64)stat);
			break;
		}
		ok++;
		logf("CHG: reg[%x]=%x stat=%x", (u64)i, (u64)regs[i],
		     (u64)stat);
		delay_us(100);
	}

	if (ok == 16u) {
		logf("CHG: read 16/16");
		logf("CHG: regs00-07 %x %x %x %x %x %x %x %x",
		     (u64)regs[0], (u64)regs[1], (u64)regs[2], (u64)regs[3],
		     (u64)regs[4], (u64)regs[5], (u64)regs[6], (u64)regs[7]);
		logf("CHG: regs08-0f %x %x %x %x %x %x %x %x",
		     (u64)regs[8], (u64)regs[9], (u64)regs[10], (u64)regs[11],
		     (u64)regs[12], (u64)regs[13], (u64)regs[14], (u64)regs[15]);
		logf("CHG: android ref 04 9b 88 73 98 bf b7 4b 34 00 a0 74 75 01 00 c0");
		logf("CHG: status8=%x chgstat=%d pg=%d vbus=%d therm=%d vsys=%d",
		     (u64)regs[8], (u64)((regs[8] >> 3) & 3u),
		     (u64)((regs[8] >> 2) & 1u), (u64)((regs[8] >> 5) & 7u),
		     (u64)((regs[8] >> 1) & 1u), (u64)(regs[8] & 1u));
		logf("CHG: fault9=%x id_a=%x id_b=%x",
		     (u64)regs[9], (u64)regs[10], (u64)regs[11]);
#if CHARGER_WRITE_TEST
		if (regs[9] != 0u) {
			logf("CHG: fault9=%x; refusing write test",
			     (u64)regs[9]);
		} else {
			u8 exp00 = (u8)((regs[0] & 0x60u) | 0x04u);
			u8 exp01 = (u8)((regs[1] & (u8)~0x60u) | 0x10u);
			u8 exp02 = (u8)((regs[2] & 0xc0u) | 0x08u);
			u8 exp04 = (u8)((regs[4] & 0x07u) | (11u << 3));
			u8 exp05 = (u8)((regs[5] & (u8)~0x30u) | 0x30u);
			u8 exp0f = (u8)((regs[0x0f] & 0x3fu) | 0x80u);
			const u8 wr_reg[6] = { 0x01u, 0x04u, 0x0fu,
					       0x02u, 0x05u, 0x00u };
			const u8 wr_val[6] = { exp01, exp04, exp0f,
					       exp02, exp05, exp00 };
			u8 rb[16];
			u32 wr_ok = 0, rb_ok = 0, verify = 0, cv_mv;

			logf("CHG: expected 00=%x 01=%x 02=%x 04=%x 05=%x 0f=%x",
			     (u64)exp00, (u64)exp01, (u64)exp02,
			     (u64)exp04, (u64)exp05, (u64)exp0f);
			for (i = 0; i < 6u; i++) {
				rc = i2c5_write_reg(I2C5_SLAVE_ADDR,
						    wr_reg[i], wr_val[i],
						    &stat);
				if (rc) {
					logf("CHG: wr[%x]=%x FAILED rc=%d stat=%x",
					     (u64)wr_reg[i], (u64)wr_val[i],
					     (u64)(rc < 0 ? -rc : rc),
					     (u64)stat);
					break;
				}
				wr_ok++;
				logf("CHG: wr[%x]=%x ok stat=%x",
				     (u64)wr_reg[i], (u64)wr_val[i],
				     (u64)stat);
				delay_us(1000u);
			}

			if (wr_ok == 6u) {
				rc = i2c5_write_reg(I2C5_SLAVE_ADDR, 0x01u,
						    (u8)(exp01 | 0x40u),
						    &stat);
				logf("CHG: wdt pet rc=%d stat=%x",
				     (u64)(rc < 0 ? -rc : rc), (u64)stat);
				delay_us(1000u);
				rc = i2c5_write_reg(I2C5_SLAVE_ADDR, 0x01u,
						    exp01, &stat);
				logf("CHG: wdt pet clear rc=%d stat=%x",
				     (u64)(rc < 0 ? -rc : rc), (u64)stat);
				delay_us(1000u);
			}

			for (i = 0; i < 16u; i++) {
				rc = i2c5_read_reg(I2C5_SLAVE_ADDR, (u8)i,
						   &rb[i], &stat);
				if (rc) {
					logf("CHG: rb[%x] FAILED rc=%d stat=%x",
					     (u64)i,
					     (u64)(rc < 0 ? -rc : rc),
					     (u64)stat);
					break;
				}
				rb_ok++;
				delay_us(100u);
			}
			if (rb_ok == 16u) {
				logf("CHG: rb regs00-07 %x %x %x %x %x %x %x %x",
				     (u64)rb[0], (u64)rb[1], (u64)rb[2],
				     (u64)rb[3], (u64)rb[4], (u64)rb[5],
				     (u64)rb[6], (u64)rb[7]);
				logf("CHG: rb regs08-0f %x %x %x %x %x %x %x %x",
				     (u64)rb[8], (u64)rb[9], (u64)rb[10],
				     (u64)rb[11], (u64)rb[12], (u64)rb[13],
				     (u64)rb[14], (u64)rb[15]);
				cv_mv = sgm_cv_mv(rb[4], rb[0x0f]);
				verify += (rb[0] == exp00);
				verify += ((rb[1] & (u8)~0x40u) ==
					   (exp01 & (u8)~0x40u));
				verify += (rb[2] == exp02);
				verify += (rb[4] == exp04);
				verify += (rb[5] == exp05);
				verify += (rb[0x0f] == exp0f);
				verify += (rb[9] == 0u);
				logf("CHG: verify cv=%d mV ichg=%d mA iindpm=%d mA otg=%d chg=%d wdt=%d wdrst=%d fault=%x",
				     (u64)cv_mv,
				     (u64)(((u32)rb[2] & 0x3fu) * 60u),
				     (u64)(100u + 100u * ((u32)rb[0] & 0x1fu)),
				     (u64)(((u32)rb[1] >> 5) & 1u),
				     (u64)(((u32)rb[1] >> 4) & 1u),
				     (u64)(((u32)rb[5] >> 4) & 3u),
				     (u64)(((u32)rb[1] >> 6) & 1u),
				     (u64)rb[9]);
				logf("CHG: write test done wr=%d/6 rb=%d/16 verify=%d/7",
				     (u64)wr_ok, (u64)rb_ok, (u64)verify);
			} else {
				logf("CHG: write test readback %d/16",
				     (u64)rb_ok);
			}
		}
#endif /* CHARGER_WRITE_TEST */
	} else {
		logf("CHG: read %d/16; partial dump above", (u64)ok);
	}

	i2c5_reset();
	reg_wr32(GPIO_BASE, gpio_off, gpio_pre);
	delay_us(20);
	logf("CHG: gpio restored=%x", (u64)reg_rd32(GPIO_BASE, gpio_off));
	logf("CHG: i2c post start=%x stat=%x fifo=%x dbg=%x",
	     (u64)i2c5_rd16(I2C_START), (u64)i2c5_rd16(I2C_INTR_STAT),
	     (u64)i2c5_rd16(I2C_FIFO_STAT), (u64)i2c5_rd16(I2C_DEBUGSTAT));
	logf("CHG: probe done ok=%d/16, clocks left for PSCI reset", (u64)ok);
}

int
battery_get_info(struct battery_info *out)
{
    if (out) {
        out->valid = 0;
        out->pmic_id = 0;
        out->battery_adc_raw = 0;
        out->battery_temp_adc_raw = 0;
        out->chip_temp_adc_raw = 0;
        out->vcdt_adc_raw = 0;
    }

	u32 init, rdata;
	u16 swcid = 0, topstatus = 0;
	u16 clk0 = 0, clk1 = 0, con0 = 0, rqst0 = 0, ana0 = 0, rst0 = 0;
	u16 adc0 = 0, adc2 = 0, adc3 = 0, adc4 = 0;
	u32 vbat = 0, battemp = 0, chiptemp = 0, vcdt = 0;
	u32 chip;
	int vbat_ok, battemp_ok, chiptemp_ok, vcdt_ok;

	charger_test();

	init = pwrap_rd(PWRAP_INIT_DONE2);
	rdata = pwrap_rd(PWRAP_WACS2_RDATA);
	logf("BAT: pwrap init2=%x rdata=%x fsm=%d sync=%d",
	     (u64)init, (u64)rdata, (u64)((rdata >> 1) & 7u),
	     (u64)((rdata >> 20) & 1u));

	if (pmic_read("SWCID", MT6358_SWCID, &swcid))
		return -1;
	if (pmic_read("TOPSTATUS", MT6358_TOPSTATUS, &topstatus))
		return -1;
	chip = (swcid >> 8) & 0xffu;
	logf("BAT: swcid=%x chip=%x topstatus=%x", (u64)swcid,
	     (u64)chip, (u64)topstatus);
	if (chip != 0x66u && chip != 0x58u)
		logf("BAT: unexpected PMIC chip id");

	if (pmic_read("CLK_CON0", MT6358_HK_TOP_CLK_CON0, &clk0) ||
	    pmic_read("CLK_CON1", MT6358_HK_TOP_CLK_CON1, &clk1) ||
	    pmic_read("AUXADC_CON0", MT6358_AUXADC_CON0, &con0) ||
	    pmic_read("AUXADC_RQST0", MT6358_AUXADC_RQST0, &rqst0) ||
	    pmic_read("AUXADC_ANA_CON0", MT6358_AUXADC_ANA_CON0, &ana0) ||
	    pmic_read("HK_TOP_RST_CON0", MT6358_HK_TOP_RST_CON0, &rst0))
		return -1;

	logf("BAT: clk0=%x clk1=%x auxcon0=%x rqst0=%x ana0=%x rst0=%x",
	     (u64)clk0, (u64)clk1, (u64)con0, (u64)rqst0, (u64)ana0,
	     (u64)rst0);
	logf("BAT: clk_pdn=%d clk_hwen=%d ck_on_extd=%x ck_aon=%d",
	     (u64)((clk0 >> 4) & 1u), (u64)((clk0 >> 3) & 1u),
	     (u64)(con0 & 0x3fu), (u64)((con0 >> 15) & 1u));

	if (pmic_read("ADC0", MT6358_AUXADC_ADC0, &adc0) ||
	    pmic_read("ADC2", MT6358_AUXADC_ADC2, &adc2) ||
	    pmic_read("ADC3", MT6358_AUXADC_ADC3, &adc3) ||
	    pmic_read("ADC4", MT6358_AUXADC_ADC4, &adc4))
		return -1;

	logf("BAT: pre adc0=%x adc2=%x adc3=%x adc4=%x", (u64)adc0,
	     (u64)adc2, (u64)adc3, (u64)adc4);

	vbat_ok = auxadc_read("BATADC", 0u, MT6358_AUXADC_ADC0, 15u, 0x80u,
			      &vbat) == 0;
	battemp_ok = auxadc_read("BAT_TEMP", 3u, MT6358_AUXADC_ADC3, 12u, 8u,
				 &battemp) == 0;
	chiptemp_ok = auxadc_read("CHIP_TEMP", 4u, MT6358_AUXADC_ADC4, 12u, 8u,
				  &chiptemp) == 0;
	vcdt_ok = auxadc_read("VCDT", 0u, MT6358_AUXADC_ADC2, 12u, 8u,
			      &vcdt) == 0;

	if (vbat_ok)
		log_adc_mv("BATADC", vbat, 3u, 1u, 15u);
	if (battemp_ok)
		log_adc_mv("BAT_TEMP", battemp, 2u, 1u, 12u);
	if (chiptemp_ok)
		log_adc_mv("CHIP_TEMP", chiptemp, 1u, 1u, 12u);
	if (vcdt_ok)
		log_adc_mv("VCDT", vcdt, 1u, 1u, 12u);

	if (pmic_read("post CLK_CON0", MT6358_HK_TOP_CLK_CON0, &clk0) == 0 &&
	    pmic_read("post AUXADC_CON0", MT6358_AUXADC_CON0, &con0) == 0 &&
	    pmic_read("post AUXADC_RQST0", MT6358_AUXADC_RQST0, &rqst0) == 0)
		logf("BAT: post clk0=%x auxcon0=%x rqst0=%x", (u64)clk0,
		     (u64)con0, (u64)rqst0);

	logf("BAT: probe done ok=%d/%d/%d/%d (read-only)", (u64)vbat_ok,
	     (u64)battemp_ok, (u64)chiptemp_ok, (u64)vcdt_ok);

    if (out) {
        out->pmic_id = swcid;
        out->battery_adc_raw = vbat;
        out->battery_temp_adc_raw = battemp;
        out->chip_temp_adc_raw = chiptemp;
        out->vcdt_adc_raw = vcdt;
        if (vbat_ok) out->valid |= BATTERY_VALID_VOLTAGE;
        if (battemp_ok) out->valid |= BATTERY_VALID_BAT_TEMP;
        if (chiptemp_ok) out->valid |= BATTERY_VALID_CHIP_TEMP;
        if (vcdt_ok) out->valid |= BATTERY_VALID_VCDT;
    }
    return vbat_ok ? 0 : -2;
}
