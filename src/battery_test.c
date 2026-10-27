/*
 * Read-only battery telemetry probe for KL7 (MT6789 + MT6366 PMIC).
 *
 * The PMIC is reached through the MT6789 PMIC wrapper at 0x10026000, using
 * the same WACS2 sequence as the working key/vibrator code in XNU.  This
 * probe only reads PMIC state and issues a normal AUXADC conversion request
 * for the battery, battery-temperature and chip-temperature channels.  It
 * does not touch the SGM41516D charger, the charger watchdog or any charging
 * setting.
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
#include "battery_test.h"

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

void
battery_test(void)
{
	u32 init, rdata;
	u16 swcid = 0, topstatus = 0;
	u16 clk0 = 0, clk1 = 0, con0 = 0, rqst0 = 0, ana0 = 0, rst0 = 0;
	u16 adc0 = 0, adc2 = 0, adc3 = 0, adc4 = 0;
	u32 vbat = 0, battemp = 0, chiptemp = 0, vcdt = 0;
	u32 chip;
	int vbat_ok, battemp_ok, chiptemp_ok, vcdt_ok;

	init = pwrap_rd(PWRAP_INIT_DONE2);
	rdata = pwrap_rd(PWRAP_WACS2_RDATA);
	logf("BAT: pwrap init2=%x rdata=%x fsm=%d sync=%d",
	     (u64)init, (u64)rdata, (u64)((rdata >> 1) & 7u),
	     (u64)((rdata >> 20) & 1u));

	if (pmic_read("SWCID", MT6358_SWCID, &swcid))
		return;
	if (pmic_read("TOPSTATUS", MT6358_TOPSTATUS, &topstatus))
		return;
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
		return;

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
		return;

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
}
