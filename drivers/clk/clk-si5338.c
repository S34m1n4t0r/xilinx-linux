/*
 * clk-si5338.c: Silicon Labs Si5338 I2C Clock Generator
 *
 * Copyright 2015 Freescale Semiconductor
 * York Sun <yorksun@freescale.com>
 *
 * Some code is taken from si5338.c by Andrey Filippov  <andrey@elphel.com>
 * Copyright 2013 Elphel, Inc.
 *
 * SI5338 has several blocks, including
 *   Inputs (IN1/IN2, IN3, IN4, IN5/IN6, XTAL)
 *   PLL (Synthesis stage 1)
 *   MultiSynth (Synthesis state 2)
 *   Outputs (OUT0/1/2/3)
 * Each block is registered as a clock device to form a tree structure.
 * See Documentation/devicetree/bindings/clock/silabs,si5338.txt for details.
 *
 * This driver uses regmap to cache register values to reduce transactions
 * on I2C bus. Volatile registers are specified.
 *
 * This program is free software; you can redistribute  it and/or modify it
 * under  the terms of  the GNU General  Public License as published by the
 * Free Software Foundation;  either version 2 of the  License, or (at your
 * option) any later version.
 */

#include <dt-bindings/clock/clk-si5338.h>
#include <linux/bsearch.h>
#include <linux/clk.h>
#include <linux/clkdev.h>
#include <linux/clk-provider.h>
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/i2c.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/platform_data/si5338.h>
#include <linux/regmap.h>
#include <linux/slab.h>
#include <linux/string.h>

#define REG5338_PAGE			255
#define REG5338_PAGE_MASK		1
#define REG5338_DEV_CONFIG2		2
#define REG5338_DEV_CONFIG2_MASK	0x3f
#define REG5338_DEV_CONFIG2_VAL		38 /* last 2 digits of part number */
#define LAST_REG			347

#define FVCOMIN			2200000000LL
#define FVCOMAX			2840000000LL
#define XTAL_FREQMIN		8000000LL
#define XTAL_FREQMAX		30000000LL
#define INFREQMIN		5000000LL
#define INFREQMAX		710000000LL
#define INFREQMAX34		200000000LL
#define INFREQDIV		40000000LL /* divide input frequency if above */

#define MSINT_MIN		4 /* need to exclude 5, 7 in the code */
#define MSINT_MAX		567

#define AWE_INT_MASK		0x061d

#define AWE_IN_MUX		0x1d18
#define AWE_IN_MUX1		0x1c1c
#define AWE_FB_MUX		0x1e18
#define AWE_FB_MUX1		0x1c20

#define AWE_XTAL_FREQ		0x1c03
#define AWE_PFD_REF		0x1de0
#define AWE_PFD_FB		0x1ee0
#define AWE_P1DIV		0x1d07
#define AWE_P2DIV		0x1e07
#define AWE_DRV0_PDN		0x1f01
#define AWE_MS0_PDN		0x1f02
#define AWE_R0DIV		0x1f1c
#define AWE_R0DIV_IN		0x1fe0
#define AWE_DRV1_PDN		0x2001
#define AWE_MS1_PDN		0x2002
#define AWE_R1DIV		0x201c
#define AWE_R1DIV_IN		0x20e0
#define AWE_DRV2_PDN		0x2101
#define AWE_MS2_PDN		0x2102
#define AWE_R2DIV		0x211c
#define AWE_R2DIV_IN		0x21e0
#define AWE_DRV3_PDN		0x2201
#define AWE_MS3_PDN		0x2202
#define AWE_R3DIV		0x221c
#define AWE_R3DIV_IN		0x22e0

#define AWE_DRV0_VDDO		0x2303
#define AWE_DRV1_VDDO		0x230c
#define AWE_DRV2_VDDO		0x2330
#define AWE_DRV3_VDDO		0x23c0
#define AWE_DRV0_FMT		0x2407
#define AWE_DRV0_INV		0x2418
#define AWE_DRV1_FMT		0x2507
#define AWE_DRV1_INV		0x2518
#define AWE_DRV2_FMT		0x2607
#define AWE_DRV2_INV		0x2618
#define AWE_DRV3_FMT		0x2707
#define AWE_DRV3_INV		0x2718

#define AWE_DRV0_TRIM		0x281f
#define AWE_DRV1_TRIM_A		0x28e0
#define AWE_DRV1_TRIM_B		0x2903
#define AWE_DRV2_TRIM		0x297c
#define AWE_DRV3_TRIM		0x2a1f

#define AWE_FCAL_OVRD_07_00	0x2dff
#define AWE_FCAL_OVRD_15_08	0x2eff
#define AWE_FCAL_OVRD_17_15	0x2f03
#define AWE_REG47_72		0x2ffc
#define AWE_PFD_EXTFB		0x3080
#define AWE_PLL_KPHI		0x307f
#define AWE_FCAL_OVRD_EN	0x3180
#define AWE_VCO_GAIN		0x3170
#define AWE_RSEL		0x310c
#define AWE_BWSEL		0x3103
#define AWE_VCO_GAIN_RSEL_BWSEL	0x317f

#define AWE_PLL_EN		0x32c0
#define AWE_MSCAL		0x323f
#define AWE_MS3_HS		0x3380
#define AWE_MS2_HS		0x3340
#define AWE_MS1_HS		0x3320
#define AWE_MS0_HS		0x3310
#define AWE_MS_PEC		0x3307

#define AWE_MS0_P1_07_00	0x35ff
#define AWE_MS0_P1_15_08	0x36ff
#define AWE_MS0_P1_17_16	0x3703
#define AWE_MS0_P2_05_00	0x37fc
#define AWE_MS0_P2_13_06	0x38ff
#define AWE_MS0_P2_21_14	0x39ff
#define AWE_MS0_P2_29_22	0x3aff
#define AWE_MS0_P3_07_00	0x3bff
#define AWE_MS0_P3_15_08	0x3cff
#define AWE_MS0_P3_23_16	0x3dff
#define AWE_MS0_P3_29_24	0x3e3f

#define AWE_MS1_P1_07_00	0x40ff
#define AWE_MS1_P1_15_08	0x41ff
#define AWE_MS1_P1_17_16	0x4203
#define AWE_MS1_P2_05_00	0x42fc
#define AWE_MS1_P2_13_06	0x43ff
#define AWE_MS1_P2_21_14	0x44ff
#define AWE_MS1_P2_29_22	0x45ff
#define AWE_MS1_P3_07_00	0x46ff
#define AWE_MS1_P3_15_08	0x47ff
#define AWE_MS1_P3_23_16	0x48ff
#define AWE_MS1_P3_29_24	0x493f

#define AWE_MS2_P1_07_00	0x4bff
#define AWE_MS2_P1_15_08	0x4cff
#define AWE_MS2_P1_17_16	0x4d03
#define AWE_MS2_P2_05_00	0x4dfc
#define AWE_MS2_P2_13_06	0x4eff
#define AWE_MS2_P2_21_14	0x4fff
#define AWE_MS2_P2_29_22	0x50ff
#define AWE_MS2_P3_07_00	0x51ff
#define AWE_MS2_P3_15_08	0x52ff
#define AWE_MS2_P3_23_16	0x53ff
#define AWE_MS2_P3_29_24	0x543f

#define AWE_MS3_P1_07_00	0x56ff
#define AWE_MS3_P1_15_08	0x57ff
#define AWE_MS3_P1_17_16	0x5803
#define AWE_MS3_P2_05_00	0x58fc
#define AWE_MS3_P2_13_06	0x59ff
#define AWE_MS3_P2_21_14	0x5aff
#define AWE_MS3_P2_29_22	0x5bff
#define AWE_MS3_P3_07_00	0x5cff
#define AWE_MS3_P3_15_08	0x5dff
#define AWE_MS3_P3_23_16	0x5eff
#define AWE_MS3_P3_29_24	0x5f3f

#define AWE_MSN_P1_07_00	0x61ff
#define AWE_MSN_P1_15_08	0x62ff
#define AWE_MSN_P1_17_16	0x6303
#define AWE_MSN_P2_05_00	0x63fc
#define AWE_MSN_P2_13_06	0x64ff
#define AWE_MSN_P2_21_14	0x65ff
#define AWE_MSN_P2_29_22	0x66ff
#define AWE_MSN_P3_07_00	0x67ff
#define AWE_MSN_P3_15_08	0x68ff
#define AWE_MSN_P3_23_16	0x69ff
#define AWE_MSN_P3_29_24	0x6a3f

#define AWE_OUT0_DIS_STATE	0x6ec0
#define AWE_OUT1_DIS_STATE	0x72c0
#define AWE_OUT2_DIS_STATE	0x76c0
#define AWE_OUT3_DIS_STATE	0x7ac0

#define AWE_STATUS			0xdaff
#define AWE_STATUS_PLL_LOL		0xda10
#define AWE_STATUS_PLL_LOS_FDBK		0xda08
#define AWE_STATUS_PLL_LOS_CLKIN	0xda04
#define AWE_STATUS_PLL_SYS_CAL		0xda01

#define AWE_MS_RESET		0xe204

#define AWE_OUT0_DIS		0xe601
#define AWE_OUT1_DIS		0xe602
#define AWE_OUT2_DIS		0xe604
#define AWE_OUT3_DIS		0xe608
#define AWE_OUT_ALL_DIS		0xe610

#define AWE_FCAL_07_00		0xebff
#define AWE_FCAL_15_08		0xecff
#define AWE_FCAL_17_16		0xed03

#define AWE_DIS_LOS		0xf180
#define AWE_REG241		0xf1ff

#define AWE_SOFT_RESET		0xf602

#define AWE_MISC_47		0x2ffc /* write 0x5 */
#define AWE_MISC_106		0x6a80 /* write 0x1 */
#define AWE_MISC_116		0x7480 /* write 0x1 */
#define AWE_MISC_42		0x2a20 /* write 0x1 */
#define AWE_MISC_06A		0x06e0 /* write 0x0 */
#define AWE_MISC_06B		0x0602 /* write 0x0 */
#define AWE_MISC_28		0x1cc0 /* write 0x0 */

#define MS_POWER_DOWN		1
#define MS_POWER_UP		0
#define OUT_DISABLE		1
#define OUT_ENABLE		0
#define DRV_POWERDOWN		1
#define DRV_POWERUP		0

struct si5338_drv_t {
	const char *description;
	u8 fmt;
	u8 vdd;
	u8 trim;
	/* bits [1:0} data,
	 * [3:2] - don't care ([3]==1 - [1] - any, [2]==1 - [0] - any
	 */
	u8 invert;
};

#define MAX_NAME_PREFIX 30 /* max 30 characters for the name_prefix */
#define MAX_NAME_LENGTH 40 /* max 40 charactors for the internal names */

struct si5338_driver_data;

/*
 * Internal parameters used by PLL and MS
 * They are used in recalc rate functions before being
 * written to the device.
 */
struct si5338_parameters {
	u32	p[3];
	bool	valid;
};

/*
 * This structure saves params and num variable for clocks
 * Internal clocks with parameters of multiple input/output
 * use this structure.
 */
struct si5338_hw_data {
	struct clk_hw			hw;
	struct si5338_driver_data	*drvdata;
	/* params is only used for PLL and multisynth clocks */
	struct si5338_parameters	params;
	/*
	 * For clkin, clkout, multisynth: index of itself
	 * For refclk, fbclk, pll: index of its source
	 */
	u8				num;
};
#define HWDATA(x) \
	((struct si5338_hw_data *)container_of(x, struct si5338_hw_data, hw))

struct si5338_driver_data {
	struct i2c_client	*client;
	struct regmap		*regmap;
	struct clk_onecell_data onecell;

	/*
	 * The structure of clocks are
	 * Input clocks: pclkin12 - IN1/2
	 *		 pclkin3  - IN3
	 *		 pclkin4  - IN4
	 *		 pclkin56 - IN5/6
	 *		 pxtal    - IN1/2 XTAL
	 * Internal clocks:
	 *		 xoclk		- from pxtal
	 *		 refclk		- from one of IN1/2, IN3, XTAL
	 *		 divrefclk	- from refclk with divider
	 *		 fbclk		- from IN4 or IN5/6
	 *		 divfbclk	- from fbclk
	 *		 MS0/1/2/3	- from one of xoclk, refclk
	 *				  diverefclk, fbclk, divfbclk
	 * Output clocks:
	 *		 clkout0/1/2/3	- from one of internal clocks
	 */
	/* parent clocks */
	struct clk		*pxtal;
	const char		*pxtal_name;
	struct clk		*pclkin[4];
	const char		*pclkin_name[4];

	/* internal and output clocks */
	char name_prefix[MAX_NAME_PREFIX];
	struct clk_hw		xtal;
	struct si5338_hw_data	clkin[4];
	struct si5338_hw_data	refclk;
	struct clk_hw		divrefclk;
	struct si5338_hw_data	fbclk;
	struct clk_hw		divfbclk;
	struct si5338_hw_data	pll;
	struct si5338_hw_data	*msynth;
	struct si5338_hw_data	*clkout;
	struct clk_lookup	*lookup[4];
};

static const char * const si5338_input_names[] = {
	"in1/in2", "in3", "in4", "in5/in6", "xtal", "noclk"
};

static const char * const si5338_pll_src_names[] = {
	"refclk", "fbclk", "divrefclk", "divfbclk", "xtal", "noclk"
};

static const char * const si5338_msynth_src_names[] = {
	"pll"
};

static const char * const si5338_msynth_names[] = {
	"ms0", "ms1", "ms2", "ms3"
};
static const char * const si5338_clkout_names[] = {
	"clkout0", "clkout1", "clkout2", "clkout3"
};
static const char * const si5338_clkout_src_names[] = {
	"fbclk", "refclk", "divfbclk", "divrefclk", "xtal",
	"ms0",
	"msn", /* it is actually ms0, ms1, ms2, ms3 dependings on clkout */
	"noclk",
};

/*
 * This array is used to determine if a register is writable. The mask is
 * not used in this driver. The data is in format of 0xAAAMM where AAA is
 * address, MM is bit mask. 1 means the corresponding bit is writable.
 * Created from SiLabs ClockBuilder output.
 * Note: Register 226, 230, 241, 246, 255 are not included in header file
 *	 from ClockBuilder v2.7 or later. Manually added here.
 */
static const u32 register_masks[] = {
	0x61d, 0x1b80, 0x1cff, 0x1dff, 0x1eff, 0x1fff, 0x20ff, 0x21ff,
	0x22ff, 0x23ff, 0x241f, 0x251f, 0x261f, 0x271f, 0x28ff, 0x297f,
	0x2a3f, 0x2dff, 0x2eff, 0x2f3f, 0x30ff, 0x31ff, 0x32ff, 0x33ff,
	0x34ff, 0x35ff, 0x36ff, 0x37ff, 0x38ff, 0x39ff, 0x3aff, 0x3bff,
	0x3cff, 0x3dff, 0x3e3f, 0x3fff, 0x40ff, 0x41ff, 0x42ff, 0x43ff,
	0x44ff, 0x45ff, 0x46ff, 0x47ff, 0x48ff, 0x493f, 0x4aff, 0x4bff,
	0x4cff, 0x4dff, 0x4eff, 0x4fff, 0x50ff, 0x51ff, 0x52ff, 0x53ff,
	0x543f, 0x55ff, 0x56ff, 0x57ff, 0x58ff, 0x59ff, 0x5aff, 0x5bff,
	0x5cff, 0x5dff, 0x5eff, 0x5f3f, 0x61ff, 0x62ff, 0x63ff, 0x64ff,
	0x65ff, 0x66ff, 0x67ff, 0x68ff, 0x69ff, 0x6abf, 0x6bff, 0x6cff,
	0x6dff, 0x6eff, 0x6fff, 0x70ff, 0x71ff, 0x72ff, 0x73ff, 0x74ff,
	0x75ff, 0x76ff, 0x77ff, 0x78ff, 0x79ff, 0x7aff, 0x7bff, 0x7cff,
	0x7dff, 0x7eff, 0x7fff, 0x80ff, 0x810f, 0x820f, 0x83ff, 0x84ff,
	0x85ff, 0x86ff, 0x87ff, 0x88ff, 0x89ff, 0x8aff, 0x8bff, 0x8cff,
	0x8dff, 0x8eff, 0x8fff, 0x90ff, 0x98ff, 0x99ff, 0x9aff, 0x9bff,
	0x9cff, 0x9dff, 0x9e0f, 0x9f0f, 0xa0ff, 0xa1ff, 0xa2ff, 0xa3ff,
	0xa4ff, 0xa5ff, 0xa6ff, 0xa7ff, 0xa8ff, 0xa9ff, 0xaaff, 0xabff,
	0xacff, 0xadff, 0xaeff, 0xafff, 0xb0ff, 0xb1ff, 0xb2ff, 0xb3ff,
	0xb4ff, 0xb50f, 0xb6ff, 0xb7ff, 0xb8ff, 0xb9ff, 0xbaff, 0xbbff,
	0xbcff, 0xbdff, 0xbeff, 0xbfff, 0xc0ff, 0xc1ff, 0xc2ff, 0xc3ff,
	0xc4ff, 0xc5ff, 0xc6ff, 0xc7ff, 0xc8ff, 0xc9ff, 0xcaff, 0xcb0f,
	0xccff, 0xcdff, 0xceff, 0xcfff, 0xd0ff, 0xd1ff, 0xd2ff, 0xd3ff,
	0xd4ff, 0xd5ff, 0xd6ff, 0xd7ff, 0xd8ff, 0xd9ff, 0xe204, 0xe6ff,
	0xf1ff, 0xf202, 0xf6ff, 0xffff, 0x11fff,
	0x120ff, 0x121ff, 0x122ff, 0x123ff, 0x124ff, 0x125ff, 0x126ff, 0x127ff,
	0x128ff, 0x129ff, 0x12aff, 0x12b0f, 0x12fff, 0x130ff, 0x131ff, 0x132ff,
	0x133ff, 0x134ff, 0x135ff, 0x136ff, 0x137ff, 0x138ff, 0x139ff, 0x13aff,
	0x13b0f, 0x13fff, 0x140ff, 0x141ff, 0x142ff, 0x143ff, 0x144ff, 0x145ff,
	0x146ff, 0x147ff, 0x148ff, 0x149ff, 0x14aff, 0x14b0f, 0x14fff, 0x150ff,
	0x151ff, 0x152ff, 0x153ff, 0x154ff, 0x155ff, 0x156ff, 0x157ff, 0x158ff,
	0x159ff, 0x15aff, 0x15b0f
};

/*
 * Si5338 i2c regmap
 */
static inline u8 si5338_reg_read(struct si5338_driver_data *drvdata,
				 u16 reg, u8 *data)
{
	u32 val;
	int ret;

	ret = regmap_read(drvdata->regmap, reg, &val);
	*data = (u8)val;	/* si5338 has u8 value */

	return ret;
}

static inline int si5338_reg_write(struct si5338_driver_data *drvdata,
				   u16 reg, u8 val, u8 mask)
{
	if (mask != 0xff)
		return regmap_update_bits(drvdata->regmap, reg, mask, val);

	return regmap_write(drvdata->regmap, reg, val);
}

static int write_field(struct si5338_driver_data *drvdata, u8 data, u32 awe)
{
	int rc, nshift;
	u8 mask, reg_data;
	u16 reg;

	reg = awe >> 8;
	mask = awe & 0xff;
	if (mask) {
		nshift = 0;
		while (!((1 << nshift) & mask))
			nshift++;
		reg_data = (data & 0xff) << nshift;
		rc = si5338_reg_write(drvdata, reg, reg_data, mask);
		if (rc < 0)
			return rc;
	}

	return 0;
}

static int write_multireg64(struct si5338_driver_data *drvdata,
				u64 data, const u32 *awe)
{
	int i, rc, nshift, nbits;
	u8 mask, reg_data;
	u16 reg;

	for (i = 0; awe[i]; i++) {
		reg = awe[i] >> 8;
		mask = awe[i] & 0xff;
		if (mask) {
			nshift = 0;
			nbits = 1;
			while (!((1 << nshift) & mask))
				nshift++;
			while ((1 << (nshift + nbits)) & mask)
				nbits++;
			/*
			 * may have some garbage in high bits,
			 * will be cut of by mask
			 */
			reg_data = (data & 0xff) << nshift;
			data >>= nbits;
			rc = si5338_reg_write(drvdata, reg, reg_data, mask);
			if (rc < 0)
				return rc;
		}
	}

	return 0;
}

/*
 * The function forms a 64-bit value from multiple registers.
 * The lastest value used by si5338 is 48-bit.
 */
static int read_multireg64(struct si5338_driver_data *drvdata,
			   const u32 *awe, u64 *data)
{
	int i, rc, nshift, nbits, full_shift = 0;
	u8 val, mask;
	u16 reg;

	*data = 0;

	for (i = 0; awe[i]; i++) {
		reg = awe[i] >> 8;
		mask = awe[i] & 0xff;
		if (mask) {
			nshift = 0;
			nbits = 1;
			while (!((1 << nshift) & mask))
				nshift++;
			while ((1 << (nshift + nbits)) & mask)
				nbits++;
			rc = si5338_reg_read(drvdata, reg, &val);
			if (rc < 0)
				return rc;

			*data |= (((s64)val & mask) >> nshift) << full_shift;
			full_shift += nbits;
		}
	}

	return 0;
}

static int read_field(struct si5338_driver_data *drvdata, u32 awe)
{
	int rc, nshift;
	u8 val, mask;
	u16 reg;

	reg = awe >> 8;
	mask = awe & 0xff;

	if (mask) {
		nshift = 0;
		while (!((1 << nshift) & mask))
			nshift++;
		rc = si5338_reg_read(drvdata, reg, &val);
		if (rc < 0)
			return rc;

		return (val & mask) >> nshift;
	}

	return 0;
}

static int si5338_find_mask(const void *key, const void *elt)
{
	const u32 *reg = key;
	const u32 *register_mask = elt;

	if (*reg > *register_mask >> 8)
		return 1;
	if (*reg < *register_mask >> 8)
		return -1;

	return 0;
}

static bool si5338_regmap_is_writeable(struct device *dev, unsigned int reg)
{
	return bsearch(&reg, register_masks, ARRAY_SIZE(register_masks),
		       sizeof(u32), si5338_find_mask) != NULL;
}

static bool si5338_regmap_is_volatile(struct device *dev, unsigned int reg)
{
	switch (reg) {
	case (AWE_STATUS >> 8):
	case (AWE_SOFT_RESET >> 8):
	case (AWE_FCAL_07_00 >> 8):
	case (AWE_FCAL_15_08 >> 8):
	case (AWE_FCAL_17_16 >> 8):
		return true;
	}

	return false;
}
static const struct regmap_range_cfg si5338_regmap_range[] = {
	{
		.selector_reg = REG5338_PAGE,		/* 255 */
		.selector_mask  = REG5338_PAGE_MASK,	/* 1 */
		.selector_shift = 0,
		.window_start = 0,
		.window_len = 256,
		.range_min = 0,
		.range_max = 347,
	},
};

static const struct regmap_config si5338_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.cache_type = REGCACHE_RBTREE,
	.max_register = 347,
	.ranges = si5338_regmap_range,
	.num_ranges = ARRAY_SIZE(si5338_regmap_range),
	.writeable_reg = si5338_regmap_is_writeable,
	.volatile_reg = si5338_regmap_is_volatile,
};

/*
 * SI5338 register access
 */
static int _verify_output_channel(int chn)
{
	if (chn < 0 || chn > 3) {
		pr_err("Invalid output channel: %d (only 0..3 are allowed)\n",
			chn);
		return -EINVAL;
	}

	return 0;
}

static int set_in_mux(struct si5338_driver_data *drvdata, int data)
{
	int data1, rc;

	switch (data) {
	case 0:
		data1 = 0;
		break;
	case 1:
		data1 = 2;
		break;
	case 2:
		data1 = 5;
		break;
	default:
		dev_err(&drvdata->client->dev,
			"Invalid value for input multiplexer %d\n", data);
		return -EINVAL;
	}
	rc = write_field(drvdata, data, AWE_IN_MUX);
	if (rc < 0)
		return rc;

	rc = write_field(drvdata, data1, AWE_IN_MUX1);
	if (rc < 0)
		return rc;

	return 0;
}

static int set_fb_mux(struct si5338_driver_data *drvdata, int data)
{
	int data1, rc;

	switch (data) {
	case 0:
		data1 = 0;
		break;
	case 1:
		data1 = 1;
		break;
	case 2:
		data1 = 0;
		break;
	default:
		dev_err(&drvdata->client->dev,
			"Invalid value for feedback multiplexer %d\n", data);
		return -EINVAL;
	}
	rc = write_field(drvdata, data, AWE_FB_MUX);
	if (rc < 0)
		return rc;

	rc = write_field(drvdata, data1, AWE_FB_MUX1);
	if (rc < 0)
		return rc;

	return 0;
}

/*
 * PLL has two inputs, each has multiple sources
 * 0 - pfd_in_ref
 * 1 - pfd_in_fb
 */
static int get_in_pfd_ref_fb(struct si5338_driver_data *drvdata, int chn)
{
	return read_field(drvdata, chn ? AWE_PFD_FB : AWE_PFD_REF);
}

static int set_in_pfd_ref_fb(struct si5338_driver_data *drvdata,
				u8 val, int chn)
{
	int rc;

	if (val > SI5338_PFD_IN_REF_NOCLK) {
		dev_err(&drvdata->client->dev,
			"Invalid value for input pfd selector: %d\n", val);
		return -EINVAL;
	}
	rc = write_field(drvdata, val, chn ? AWE_PFD_FB : AWE_PFD_REF);
	if (rc < 0)
		return rc;

	return 0;
}

/*
 * Set div for the two dividers
 * 0 - p1div
 * 1 - p2div
 * The dividers have value of 1, 2, 4, 8, 16, 32
 */
static int set_in_pdiv(struct si5338_driver_data *drvdata, int div, int chn)
{
	u8 val;
	u32 awe = chn ? AWE_P2DIV : AWE_P1DIV;

	for (val = 0; val < 6; val++) {
		if ((1 << val) == div)
			return write_field(drvdata, val, awe);
	}
	dev_err(&drvdata->client->dev,
		"Invalid value for input divider: %d\n", div);

	return -EINVAL;
}

/*
 * Si5338 xtal clock input
 * The clock needs to be within [8MHz .. 30MHz]
 */
static int si5338_xtal_prepare(struct clk_hw *hw)
{
	struct si5338_driver_data *drvdata =
		container_of(hw, struct si5338_driver_data, xtal);
	unsigned long rate = clk_hw_get_rate(hw);
	int xtal_mode;

	if (rate < XTAL_FREQMIN) {
		dev_err(&drvdata->client->dev,
			"Xtal input frequency too low: %lu < %llu\n",
			rate, XTAL_FREQMIN);
		return -EINVAL;
	}
	if (rate > XTAL_FREQMAX) {
		dev_err(&drvdata->client->dev,
			"Xtal input frequency too high: %lu > %llu\n",
			rate, XTAL_FREQMAX);
		return -EINVAL;
	}

	if (rate > 26000000ll)
		xtal_mode = 3;
	else if (rate > 19000000ll)
		xtal_mode = 2;
	else if (rate > 11000000ll)
		xtal_mode = 1;
	else
		xtal_mode = 0;

	return write_field(drvdata, xtal_mode, AWE_XTAL_FREQ);
}

static const struct clk_ops si5338_xtal_ops = {
	.prepare = si5338_xtal_prepare,
};

static unsigned long si5338_clkin_recalc_rate(struct clk_hw *hw,
						unsigned long parent_rate)
{
	struct si5338_hw_data *hwdata = HWDATA(hw);
	struct si5338_driver_data *drvdata = hwdata->drvdata;
	unsigned long max_rate;

	max_rate = (hwdata->num == SI5338_INPUT_CLK12 ||
		    hwdata->num == SI5338_INPUT_CLK56) ?
			INFREQMAX : INFREQMAX34;
	if (parent_rate  < INFREQMIN) {
		dev_err(&drvdata->client->dev,
			"Input frequency too low: %lu < %llu\n",
			parent_rate, INFREQMIN);
		return -EINVAL;
	}
	if (parent_rate > max_rate) {
		dev_err(&drvdata->client->dev,
			"Input frequency too high: %lu > %lu\n",
			parent_rate, max_rate);
		return -EINVAL;
	}

	return parent_rate;
}

static const struct clk_ops si5338_clkin_ops = {
	.recalc_rate = si5338_clkin_recalc_rate,
};

/*
 * Si5338 refclk inputs
 * Input frequency range
 *	IN1/IN2 differential clock [5MHz..710MHz]
 *	IN3 single-ended clock [5MHz..200MHz]
 * Enforced by si5338_clkin_recalc_rate
 */
static int si5338_refclk_reparent(struct si5338_driver_data *drvdata, u8 index)
{
	struct si5338_hw_data *hwdata = &drvdata->refclk;

	hwdata->num = SI5338_FB_SRC_NOCLK;
	switch (index) {
	case SI5338_REF_SRC_XTAL:
		/* in mux to XO */
		hwdata->num = 2;
		return set_in_mux(drvdata, 2);
	case SI5338_REF_SRC_CLKIN12:
		/* in mux to IN12 */
		hwdata->num = 0;
		return set_in_mux(drvdata, 0);
	case SI5338_REF_SRC_CLKIN3:
		hwdata->num = 1;
		return set_in_mux(drvdata, 1);
	}
	dev_err(&drvdata->client->dev,
		"Invalid parent (%d) for refclk\n", index);

	return -EINVAL;
}

/*
 * refclk's parent
 * 0 - IN1/IN2
 * 1 - IN3
 * 2 - XTAL
 */
static int si5338_refclk_set_parent(struct clk_hw *hw, u8 index)
{
	struct si5338_hw_data *hwdata = HWDATA(hw);
	struct si5338_driver_data *drvdata = hwdata->drvdata;

	switch (index) {
	case 0:
		return si5338_refclk_reparent(drvdata, SI5338_REF_SRC_CLKIN12);
	case 1:
		return si5338_refclk_reparent(drvdata, SI5338_REF_SRC_CLKIN3);
	case 2:
		return si5338_refclk_reparent(drvdata, SI5338_REF_SRC_XTAL);
	}
	dev_err(&drvdata->client->dev,
		"Invalid parent index for refclk: %d\n", index);

	return -EINVAL;
}

static u8 si5338_refclk_get_parent(struct clk_hw *hw)
{
	struct si5338_hw_data *hwdata = HWDATA(hw);
	struct si5338_driver_data *drvdata = hwdata->drvdata;

	/* get input mux */
	return read_field(drvdata, AWE_IN_MUX);
}

static const struct clk_ops si5338_refclk_ops = {
	.set_parent = si5338_refclk_set_parent,
	.get_parent = si5338_refclk_get_parent,
};

/*
 * divrefclk's parent is refclk
 */
static int si5338_divrefclk_prepare(struct clk_hw *hw)
{
	struct si5338_driver_data *drvdata =
		container_of(hw, struct si5338_driver_data, divrefclk);
	int idiv;
	unsigned long parent_rate = clk_hw_get_rate(clk_hw_get_parent(hw));

	/*
	 * Calculate the lowest ratio to divide the input frequency to 40MHz
	 * or less
	 */
	for (idiv = 0; idiv < 5; idiv++) {
		if ((parent_rate >> idiv) <= INFREQDIV)
			break;
	}

	return set_in_pdiv(drvdata, 1 << idiv, 0);
}

static unsigned long si5338_divrefclk_recalc_rate(struct clk_hw *hw,
					   unsigned long parent_rate)
{
	int idiv;

	/*
	 * Calculate the lowest ratio to divide the input frequency to 40MHz
	 * or less
	 */
	for (idiv = 0; idiv < 5; idiv++) {
		if ((parent_rate >> idiv) <= INFREQDIV)
			break;
	}

	return parent_rate >> idiv;
}

static const struct clk_ops si5338_divrefclk_ops = {
	.recalc_rate = si5338_divrefclk_recalc_rate,
	.prepare = si5338_divrefclk_prepare,
};

/*
 * Si5338 fbclk inputs
 * Input frequency range
 *	IN4 single-ended clock [5MHz..200MHz]
 *	IN5/IN6 differential clock [5MHz..710MHz]
 * Enforced by si5338_clkin_recalc_rate
 */
static int si5338_fbclk_reparent(struct si5338_driver_data *drvdata, u8 index)
{
	struct si5338_hw_data *hwdata = &drvdata->fbclk;

	hwdata->num = SI5338_FB_SRC_NOCLK;
	switch (index) {
	case SI5338_FB_SRC_CLKIN4:
		/* in mux to IN4 */
		hwdata->num = 0;
		return set_fb_mux(drvdata, 1);
	case SI5338_FB_SRC_CLKIN56:
		/* in mux to IN56 */
		hwdata->num = 1;
		return set_fb_mux(drvdata, 0);
	case SI5338_FB_SRC_NOCLK:
		hwdata->num = 2;
		return set_fb_mux(drvdata, 2);
	}
	dev_err(&drvdata->client->dev,
		"Invalid parent (%d) for fbclk\n", index);

	return -EINVAL;
}

/*
 * fbclk's parent can be
 * 0 - IN4
 * 1 - IN5/IN6
 * 2 - NOCLK
 */
static int si5338_fbclk_set_parent(struct clk_hw *hw, u8 index)
{
	struct si5338_hw_data *hwdata = HWDATA(hw);
	struct si5338_driver_data *drvdata = hwdata->drvdata;

	switch (index) {
	case 0:
		return si5338_fbclk_reparent(drvdata, SI5338_FB_SRC_CLKIN4);
	case 1:
		return si5338_fbclk_reparent(drvdata, SI5338_FB_SRC_CLKIN56);
	case 2:
		return si5338_fbclk_reparent(drvdata, SI5338_FB_SRC_NOCLK);
	}
	dev_err(&drvdata->client->dev,
		"Invalid parent index for fbclk\n");

	return -EINVAL;
}

static u8 si5338_fbclk_get_parent(struct clk_hw *hw)
{
	struct si5338_hw_data *hwdata = HWDATA(hw);
	struct si5338_driver_data *drvdata = hwdata->drvdata;
	int rc;

	/* Return value 0: IN5/IN6
	 *		1: IN4
	 *		2: noclk
	 */
	rc = read_field(drvdata, AWE_FB_MUX);
	switch (rc) {
	case 0:
		return 1;
	case 1:
		return 0;
	case 2:
		return 2;
	default:
		break;
	}

	return rc;
}

static const struct clk_ops si5338_fbclk_ops = {
	.set_parent = si5338_fbclk_set_parent,
	.get_parent = si5338_fbclk_get_parent,
};

/*
 * divfbclk's parent is fbclk
 */
static int si5338_divfbclk_prepare(struct clk_hw *hw)
{
	struct si5338_driver_data *drvdata =
		container_of(hw, struct si5338_driver_data, divfbclk);
	int idiv;
	unsigned long parent_rate = clk_hw_get_rate(clk_hw_get_parent(hw));

	for (idiv = 0; idiv < 5; idiv++) {
		if ((parent_rate >> idiv) <= INFREQDIV)
			break;
	}

	return set_in_pdiv(drvdata, 1 << idiv, 1);
}

static unsigned long si5338_divfbclk_recalc_rate(struct clk_hw *hw,
					   unsigned long parent_rate)
{
	int idiv;

	for (idiv = 0; idiv < 5; idiv++) {
		if ((parent_rate >> idiv) <= INFREQDIV)
			break;
	}

	return parent_rate >> idiv;
}

static const struct clk_ops si5338_divfbclk_ops = {
	.recalc_rate = si5338_divfbclk_recalc_rate,
	.prepare = si5338_divfbclk_prepare,
};

/*
 * PLL and MultiSynth
 */
static int remove_common_factor(u64 *num_denom)
{
	u64 a, b, r;

	if (!num_denom[1])
		return -1; /* zero denominator */

	if (!num_denom[0]) {
		num_denom[1] = 1;
		return 1;
	}

	a = max(num_denom[0], num_denom[1]);
	b = min(num_denom[0], num_denom[1]);
	r = b;
	while (r > 1) {
		r = a - b * div64_u64(a, b);
		if (!r) {
			num_denom[0] = div64_u64(num_denom[0], b);
			num_denom[1] = div64_u64(num_denom[1], b);
			return 1;
		}
		a = b;
		b = r;
	}

	return 0; /* nothing done */
}

static const u32 awe_msx[5][3][5] = {
	{
		{
			AWE_MS0_P1_07_00,
			AWE_MS0_P1_15_08,
			AWE_MS0_P1_17_16,
			0,
			0
		},
		{
			AWE_MS0_P2_05_00,
			AWE_MS0_P2_13_06,
			AWE_MS0_P2_21_14,
			AWE_MS0_P2_29_22,
			0
		},
		{
			AWE_MS0_P3_07_00,
			AWE_MS0_P3_15_08,
			AWE_MS0_P3_23_16,
			AWE_MS0_P3_29_24,
			0
		}
	},
	{
		{
			AWE_MS1_P1_07_00,
			AWE_MS1_P1_15_08,
			AWE_MS1_P1_17_16,
			0,
			0
		},
		{
			AWE_MS1_P2_05_00,
			AWE_MS1_P2_13_06,
			AWE_MS1_P2_21_14,
			AWE_MS1_P2_29_22,
			0
		},
		{
			AWE_MS1_P3_07_00,
			AWE_MS1_P3_15_08,
			AWE_MS1_P3_23_16,
			AWE_MS1_P3_29_24,
			0
		}
	},
	{
		{
			AWE_MS2_P1_07_00,
			AWE_MS2_P1_15_08,
			AWE_MS2_P1_17_16,
			0,
			0
		},
		{
			AWE_MS2_P2_05_00,
			AWE_MS2_P2_13_06,
			AWE_MS2_P2_21_14,
			AWE_MS2_P2_29_22,
			0
		},
		{
			AWE_MS2_P3_07_00,
			AWE_MS2_P3_15_08,
			AWE_MS2_P3_23_16,
			AWE_MS2_P3_29_24,
			0
		}
	},
	{
		{
			AWE_MS3_P1_07_00,
			AWE_MS3_P1_15_08,
			AWE_MS3_P1_17_16,
			0,
			0
		},
		{
			AWE_MS3_P2_05_00,
			AWE_MS3_P2_13_06,
			AWE_MS3_P2_21_14,
			AWE_MS3_P2_29_22,
			0
		},
		{
			AWE_MS3_P3_07_00,
			AWE_MS3_P3_15_08,
			AWE_MS3_P3_23_16,
			AWE_MS3_P3_29_24,
			0
		}
	},
	{
		{
			AWE_MSN_P1_07_00,
			AWE_MSN_P1_15_08,
			AWE_MSN_P1_17_16,
			0,
			0
		},
		{
			AWE_MSN_P2_05_00,
			AWE_MSN_P2_13_06,
			AWE_MSN_P2_21_14,
			AWE_MSN_P2_29_22,
			0
		},
		{
			AWE_MSN_P3_07_00,
			AWE_MSN_P3_15_08,
			AWE_MSN_P3_23_16,
			AWE_MSN_P3_29_24,
			0
		}
	}
};

static int _verify_ms_channel(struct device *dev, int chn)
{
	if (chn < 0 || chn > 4) {
		dev_err(dev,
			"Invalid channel %d. Only 0,1,2,3 and 4 (for MSN) are supported\n",
			chn);
		return -EINVAL;
	}

	return 0;
}

/*
 * Read parameters of
 * 0 - MS0
 * 1 - MS1
 * 2 - MS2
 * 3 - MS3
 * 4 - MSN (PLL)
 */
static int get_ms_p(struct si5338_driver_data *drvdata, u32 *p, int chn)
{
	int i, rc;
	u64 data;

	rc = _verify_ms_channel(&drvdata->client->dev, chn);
	if (rc < 0)
		return rc;

	for (i = 0; i < 3; i++) {
		rc = read_multireg64(drvdata, awe_msx[chn][i], &data);
		if (rc < 0)
			return rc;

		p[i] = (u32)data;	/* only use up to 30 bit here */
	}

	return 0;
}

/*
 * Calculte MS ratio from parameters
 * ms = a + b / c, where
 *	a = ms[0], b = ms[1], c = ms[2]
 * SI5338 RM states the formula of parameters as:
 *	p1 = floor(((a * c + b) * 128) / c - 512)
 *	p2 = mod((b * 128), c)
 *	p3 = c
 * To reverse the formula, we have
 *	b * 128 = k * c + p2; k < 128, p2 < c
 *	p1 = floor(((a * c + b) * 128) / c - 512)
 *	   = a * 128 + floor((b * 128) / c) - 512
 *	   = a * 128 + k - 512
 *	k = mod(p1, 128) = p1 & 0x7f
 *	c = p3
 *	b = (k * c + p2) / 128 = ((p1 & 0x7f) * p3 + p2) >> 7
 *	a = (p1 + 512) >> 7 = (p1 >> 7) + 4
 */
static int p_to_ms(u64 *ms, u32 *p)
{
	if (!p[0] && !p[1] && !p[2]) {
		/* uninitialized parameters in device */
		ms[0] = 0;
		ms[1] = 0;
		ms[2] = 1;
	} else {
		/* c = p3 */
		ms[2] = p[2];
		/* b = (c * (p1 & 0x7f) + p2) >> 7 */
		ms[1] = (ms[2] * (p[0] & 0x7f) + p[1]) >> 7;
		/* a = (p1 >> 7) + 4 */
		ms[0] = (p[0] >> 7) + 4;
	}
	pr_debug("ms[]=%llu + %llu/%llu, p=%u %u %u\n",
		 ms[0], ms[1], ms[2], p[0], p[1], p[2]);

	return 0;
}

static const u32 awe_ms_hs[] = {
	AWE_MS0_HS,
	AWE_MS1_HS,
	AWE_MS2_HS,
	AWE_MS3_HS
};

/*
 * Read parameters of
 * 0 - MS0
 * 1 - MS1
 * 2 - MS2
 * 3 - MS3
 * 4 - MSN (PLL)
 */
static int set_ms_p(struct si5338_driver_data *drvdata,
			u32 *p, int chn)
{
	int i, rc, hs = 0;

	rc = _verify_ms_channel(&drvdata->client->dev, chn);
	if (rc < 0)
		return rc;

	/* high speed bit programming */
	if (p[0] < 512) { /* div less than 8 */
		if (p[0] < 128)
			p[0] = 0;
		else
			p[0] = 256;
		p[1] = 0;
		p[2] = 1;
		hs = 1;
		dev_dbg(&drvdata->client->dev,
			"Using high speed divider option on ms%d",
			chn);
	}

	rc = write_field(drvdata, hs, awe_ms_hs[chn]);
	if (rc < 0)
		return rc;

	for (i = 0; i < 3; i++) {
		rc = write_multireg64(drvdata, (u64)p[i],
				      awe_msx[chn][i]);
		if (rc < 0)
			return rc;
	}

	return 0;
}

/*
 * Calculate parameters
 * ms = ms[0] + ms[1] / ms[2]
 *
 * SI5338 RM stats the fomula of parameters as
 *	p[0] = floor(((ms[0] * ms[2] + ms[1]) * 128) / ms[2] - 512)
 *	p[1] = mod((ms[1] * 128), ms[2])
 *	p[2] = ms[2]
 */
static int ms_to_p(u64 *ms, u32 *p)
{
	u64 d;
	u64 ms_denom = ms[2], ms_num = ms[1], ms_int = ms[0];

	while (ms_denom >= (1 << 30) || !((ms_denom | ms_num) & 1)) {
		ms_denom >>= 1;
		ms_num >>= 1;
	}
	if (!ms_num || !ms_denom) {
		ms_denom = 1;
		ms_num = 0;
	}
	d = (ms_int * ms_denom + ms_num) << 7;
	p[0] = (u32)(div64_u64(d, ms_denom) - 512);
	d = div64_u64((ms_num << 7), ms_denom);
	p[1] = (u32)((ms_num << 7) - d * ms_denom);
	p[2] = ms_denom;
	pr_debug("ms[]=%llu + %llu/%llu Hz, ms_int=%llu, ms_num=%llu, ms_denom=%llu p=%u %u %u\n",
		 ms[0], ms[1], ms[2], ms_int, ms_num, ms_denom,
		 p[0], p[1], p[2]);

	return 0;
}

/*
 * Calculate MultiSynth divider (MS0..MS3) for specified output frequency
 */
static void cal_ms_p(unsigned long numerator,
			unsigned long denominator,
			u32 *p)
{
	u64 ms[3];

	ms[1] = numerator;
	ms[2] = denominator;
	ms[0] = div64_u64(ms[1], ms[2]);
	ms[1] -= ms[0] * ms[2];
	while (ms[2] >= (1 << 30)) { /* trim */
		ms[2] >>= 1;
		ms[1] >>= 1;
	}
	remove_common_factor(&ms[1]);

	if (ms[0] < MSINT_MIN) {
		pr_warn("Calculated MSN ratio is too low: %llu < %u\n",
			ms[0], MSINT_MIN);
		ms[0] = MSINT_MIN;
	} else if (ms[0] == 5 || ms[0] == 7) {
		pr_warn("MSN ratio %llu is invalid\n", ms[0]);
		ms[0] += 1;
	} else if (ms[0] > MSINT_MAX) {
		pr_warn("Calculated MSN ratio is too high: %llu > %u\n",
			ms[0], MSINT_MAX);
		ms[0] = MSINT_MAX;
	}
	pr_debug("MS divider: %llu+%llu/%llu\n", ms[0], ms[1], ms[2]);

	ms_to_p(ms, p);
}

/*
 * Si5338 pll section
 */
static int si5338_pll_prepare(struct clk_hw *hw)
{
	struct si5338_hw_data *hwdata = HWDATA(hw);
	struct si5338_driver_data *drvdata = hwdata->drvdata;
	int rc;
	s64 pll_in_freq;
	s64 K, Q, kphi_num, kphi_denom, fvco_mhz, fpfd_mhz;
	int rsel, bwsel, vco_gain, pll_kphi, mscal, ms_pec;
	u8 vco_gain_rsel_bwsel;

	pll_in_freq = clk_hw_get_rate(clk_hw_get_parent(hw));
	if (!pll_in_freq) {
		dev_err(&drvdata->client->dev, "Invalid input clock for pll\n");
		return -EINVAL;
	}
	if (!clk_get_rate(hw->clk)) {
		dev_err(&drvdata->client->dev, "Invalid clock rate for pll\n");
		return -EINVAL;
	}

	fvco_mhz = div64_u64(clk_hw_get_rate(hw), 1000000ll);
	fpfd_mhz = div64_u64(pll_in_freq, 1000000ll);
	if (fpfd_mhz >= 15) {
		K = 925;
		rsel = 0;
		bwsel = 0;
	} else if (fpfd_mhz >= 8) {
		K = 325;
		rsel = 1;
		bwsel = 1;
	} else {
		K = 185;
		rsel = 3;
		bwsel = 2;
	}
	if (fvco_mhz > 2425) {
		Q = 3;
		vco_gain = 0;
	} else {
		Q = 4;
		vco_gain = 1;
	}
	kphi_num = K * 2500LL * 2500LL * 2500LL;
	kphi_denom = 533LL * Q * fpfd_mhz * fvco_mhz * fvco_mhz;
	pll_kphi = (int)div64_u64(kphi_num + (kphi_denom >> 1), kphi_denom);
	if (pll_kphi < 1 || pll_kphi > 127) {
		dev_warn(&drvdata->client->dev,
			"Calculated PLL_KPHI does not fit 1<=%d<=127\n",
			pll_kphi);
		if (pll_kphi < 1)
			pll_kphi = 1;
		else if (pll_kphi > 127)
			pll_kphi = 127;
	}
	mscal = (int)div64_u64(2067000 - 667 * fvco_mhz + 50000, 100000ll);
	if (mscal < 0 || mscal > 63) {
		dev_warn(&drvdata->client->dev,
			"Calculated MSCAL does not fit 0<=%d<=63\n",
			mscal);
		if (mscal < 0)
			mscal = 0;
		else if (mscal > 63)
			mscal = 63;
	}
	ms_pec = 7;
	dev_dbg(&drvdata->client->dev,
		"Calculated values: PLL_KPHI=%d K=%lld RSEL=%d BWSEL=%d VCO_GAIN=%d MSCAL=%d MS_PEC=%d\n",
		pll_kphi, K, rsel, bwsel, vco_gain, mscal, ms_pec);

	/* setting actual registers */
	rc = write_field(drvdata, pll_kphi, AWE_PLL_KPHI);
	if (rc < 0)
		return rc;

	vco_gain_rsel_bwsel = (vco_gain & 7) << 4;
	vco_gain_rsel_bwsel |= (rsel & 3) << 2;
	vco_gain_rsel_bwsel |= bwsel & 3;
	rc = write_field(drvdata, vco_gain_rsel_bwsel, AWE_VCO_GAIN_RSEL_BWSEL);
	if (rc < 0)
		return rc;

	rc = write_field(drvdata, mscal, AWE_MSCAL);
	if (rc < 0)
		return rc;

	rc = write_field(drvdata, ms_pec, AWE_MS_PEC);
	if (rc < 0)
		return rc;

	rc = write_field(drvdata, 3, AWE_PLL_EN);
	if (rc < 0)
		return rc; /* enable PLL */

	return 0;
}

static int si5338_pll_reparent(struct si5338_driver_data *drvdata,
				u8 index)
{
	struct si5338_hw_data *hwdata = &drvdata->pll;
	int rc = -EINVAL;

	hwdata->num = SI5338_PFD_IN_REF_NOCLK;
	switch (index) {
	case SI5338_PFD_IN_REF_REFCLK:
	case SI5338_PFD_IN_REF_FBCLK:
	case SI5338_PFD_IN_REF_DIVREFCLK:
	case SI5338_PFD_IN_REF_DIVFBCLK:
	case SI5338_PFD_IN_REF_XOCLK:
	case SI5338_PFD_IN_REF_NOCLK:
		/* pfd_in_ref mux */
		rc = set_in_pfd_ref_fb(drvdata, index, 0);
		break;
	default:
		dev_err(&drvdata->client->dev,
			"Invalid pfd_in_ref mux selection %d\n",
			index);
		break;
	}

	if (!rc)
		hwdata->num = index;	/* record the source of pll */

	return rc;
}

static unsigned char si5338_pll_get_parent(struct clk_hw *hw)
{
	struct si5338_hw_data *hwdata = HWDATA(hw);
	struct si5338_driver_data *drvdata = hwdata->drvdata;
	int pfd_in_ref;

	/* Get pfd_in_ref mux value */
	pfd_in_ref = get_in_pfd_ref_fb(drvdata, 0);
	if (pfd_in_ref < 0) {
		dev_err(&drvdata->client->dev,
			"Error getting pfd_in_ref mux\n");
		/* In case reading register fails, set to 0 */
		pfd_in_ref = 0;
	}
	hwdata->num = pfd_in_ref;

	return pfd_in_ref;
}

static int si5338_pll_set_parent(struct clk_hw *hw, u8 index)
{
	struct si5338_hw_data *hwdata = HWDATA(hw);

	return si5338_pll_reparent(hwdata->drvdata, index);
}

static unsigned long si5338_pll_recalc_rate(struct clk_hw *hw,
					    unsigned long parent_rate)
{
	struct si5338_hw_data *hwdata = HWDATA(hw);
	struct si5338_driver_data *drvdata = hwdata->drvdata;
	int rc;
	u64 rate[3], ms[3], ms_scaled;

	if (!hwdata->params.valid) {
		rc = get_ms_p(drvdata, hwdata->params.p, 4);
		if (rc < 0) {
			dev_err(&drvdata->client->dev,
				"Error reading ms register\n");
			return 0;
		}
		hwdata->params.valid = true;
	}

	p_to_ms(ms, hwdata->params.p);
	if (unlikely(!ms[2])) {
		/*
		 * This should not happen. Instead of crashing the system,
		 * set divisor to 1 and let the calculation continue.
		 */
		dev_warn(&drvdata->client->dev,
			"Error %s calculating pll\n", __func__);
		ms[2] = 1;
	}
	ms_scaled = ms[0] * ms[2] + ms[1];
	if (!ms_scaled)	/* uninitialzied */
		return 0;

	rate[2] = ms[2];
	rate[1] = parent_rate * ms_scaled;
	rate[0] = div64_u64(rate[1], rate[2]);
	rate[1] -= rate[0] * rate[2];
	remove_common_factor(&rate[1]);
	dev_dbg(&drvdata->client->dev,
		"PLL output frequency: %llu+%llu/%llu Hz\n",
		rate[0], rate[1], rate[2]);

	return rate[0];
}

static long si5338_pll_round_rate(struct clk_hw *hw, unsigned long rate,
				  unsigned long *parent_rate)
{
	struct si5338_hw_data *hwdata = HWDATA(hw);
	struct si5338_driver_data *drvdata = hwdata->drvdata;
	u64 ms[] = {0, 0, 1};
	u64 new_rate[3], ms_scaled;

	if (unlikely(rate < FVCOMIN))
		rate = FVCOMIN;
	else if (unlikely(rate > FVCOMAX))
		rate = FVCOMAX;

	cal_ms_p(rate, *parent_rate, hwdata->params.p);
	hwdata->params.valid = true;

	p_to_ms(ms, hwdata->params.p);
	ms_scaled = ms[0] * ms[2] + ms[1];

	new_rate[2] = ms[2];
	new_rate[1] = *parent_rate * ms_scaled;
	new_rate[0] = div64_u64(new_rate[1], new_rate[2]);
	new_rate[1] -= new_rate[0] * new_rate[2];
	remove_common_factor(&new_rate[1]);
	dev_dbg(&drvdata->client->dev,
		"PLL output frequency: %llu+%llu/%llu Hz\n",
		new_rate[0], new_rate[1], new_rate[2]);

	return new_rate[0];
}

static int si5338_pll_set_rate(struct clk_hw *hw, unsigned long rate,
				unsigned long parent_rate)
{
	struct si5338_hw_data *hwdata = HWDATA(hw);
	struct si5338_driver_data *drvdata = hwdata->drvdata;

	if (unlikely(rate < FVCOMIN))
		rate = FVCOMIN;
	else if (unlikely(rate > FVCOMAX))
		rate = FVCOMAX;
	cal_ms_p(rate, parent_rate, hwdata->params.p);
	hwdata->params.valid = true;

	return set_ms_p(drvdata, hwdata->params.p, 4);
}

static const struct clk_ops si5338_pll_ops = {
	.prepare = si5338_pll_prepare,
	.set_parent = si5338_pll_set_parent,
	.get_parent = si5338_pll_get_parent,
	.recalc_rate = si5338_pll_recalc_rate,
	.round_rate = si5338_pll_round_rate,
	.set_rate = si5338_pll_set_rate,
};

/*
 * Si5338 multisynth divider
 */

static const u32 awe_ms_powerdown[] = {
	AWE_MS0_PDN,
	AWE_MS1_PDN,
	AWE_MS2_PDN,
	AWE_MS3_PDN
};

static int set_ms_powerdown(struct si5338_driver_data *drvdata,
			     int down, int chn)
{
	if (chn < 0 || chn > 3)
		return -EINVAL;

	return write_field(drvdata, down, awe_ms_powerdown[chn]);
}

static int si5338_msynth_prepare(struct clk_hw *hw)
{
	struct si5338_hw_data *hwdata = HWDATA(hw);
	struct si5338_driver_data *drvdata = hwdata->drvdata;

	return set_ms_powerdown(drvdata, MS_POWER_UP, hwdata->num);
}

static void si5338_msynth_unprepare(struct clk_hw *hw)
{
	struct si5338_hw_data *hwdata = HWDATA(hw);
	struct si5338_driver_data *drvdata = hwdata->drvdata;

	set_ms_powerdown(drvdata, MS_POWER_DOWN, hwdata->num);
}

static unsigned long si5338_msynth_recalc_rate(struct clk_hw *hw,
					       unsigned long parent_rate)
{
	struct si5338_hw_data *hwdata = HWDATA(hw);
	struct si5338_driver_data *drvdata = hwdata->drvdata;
	int rc;
	u64 rate[3], ms[3], ms_scaled;

	if (!hwdata->params.valid) {
		rc = get_ms_p(drvdata, hwdata->params.p, hwdata->num);
		if (rc < 0)
			return 0;
		hwdata->params.valid = true;
	}

	p_to_ms(ms, hwdata->params.p);
	if (unlikely(!ms[2])) {
		/* This should not happen */
		dev_warn(&drvdata->client->dev,
			"Error %s calculating MS%d\n", __func__, hwdata->num);
		ms[2] = 1;
	}
	/* trim MS divider fraction */
	while (ms[2] >= 0x1000) {
		ms[1] >>= 1;
		ms[2] >>= 1;
	}
	ms_scaled = ms[0] * ms[2] + ms[1];
	if (!ms_scaled)	/* uninitialized */
		return 0;

	rate[2] = ms_scaled;
	rate[1] = parent_rate * ms[2];
	rate[0] = div64_u64(rate[1], rate[2]);
	rate[1] -= rate[0] * rate[2];
	remove_common_factor(&rate[1]);
	dev_dbg(&drvdata->client->dev,
		"MS%d output frequency: %llu+%llu/%llu Hz\n",
		hwdata->num, rate[0], rate[1], rate[2]);

	return rate[0];
}

/*
 * Based on PLL input clock, estimate best ratio for desired output
 * if pll vco is not specified.
 */
static long si5338_msynth_round_rate(struct clk_hw *hw, unsigned long rate,
				     unsigned long *parent_rate)
{
	struct si5338_hw_data *hwdata = HWDATA(hw);
	struct si5338_driver_data *drvdata = hwdata->drvdata;
	s64 rate_scaled, pll_in_freq;
	s64 center, center_diff, best_center_diff = 0;
	s64 out_div, best_out_div = 1;
	s64 d, in_div, best_in_div;
	s64 err, best_err = 0;
	s64 synth_out;
	u64 ms[3];

	if (__clk_get_flags(hw->clk) & CLK_SET_RATE_PARENT) {
		/*
		 * Get rate of the parent of PLL
		 * (could be refclk, fbclk, etc.)
		 */
		pll_in_freq = clk_hw_get_rate(
				clk_hw_get_parent(clk_hw_get_parent(hw)));
		if (!pll_in_freq) {
			dev_err(&drvdata->client->dev,
				"Invalid input clock for MS%d\n",
				hwdata->num);
			return -EINVAL;
		}

		center = (FVCOMAX + FVCOMIN) >> 1;

		best_in_div = 0;
		for (out_div = 4; out_div <= MSINT_MAX; out_div++) {
			if (out_div == 5 || out_div == 7)
				continue;

			/* here scaled by denominator */
			rate_scaled = rate * out_div;
			if (rate_scaled < FVCOMIN || rate_scaled > FVCOMAX)
				continue;

			in_div = div64_u64(rate_scaled +
					   (pll_in_freq >> 1),
					   pll_in_freq); /* round */

			/* actual pll frequency scaled by out_denom */
			d = pll_in_freq * in_div;
			synth_out = div64_u64(d + (out_div >> 1), out_div);
			center_diff = d - center;
			if (center_diff < 0)
				center_diff = -center_diff;
			err = synth_out - rate;
			if (err < 0)
				err = -err;
			if (!best_in_div || err < best_err ||
			    (err == best_err &&
			     center_diff < best_center_diff)) {
				dev_dbg(&drvdata->client->dev,
					"synth_out: %lld center: %lld rate:%lu err: %lld (%lld) center_diff:%lld(%lld)\n",
					synth_out, center, rate, err, best_err,
					center_diff, best_center_diff);
				best_err = err;
				best_in_div = in_div;
				best_out_div = out_div;
				best_center_diff = center_diff;
			}
		}
		if (!best_in_div) {
			dev_warn(&drvdata->client->dev,
				 "Failed to find suitable integer coefficients for pll input %lld Hz\n",
				 pll_in_freq);
		}
		*parent_rate = pll_in_freq * best_in_div;
		rate = *parent_rate / (unsigned long)best_out_div;
		dev_dbg(&drvdata->client->dev,
			"Best MS output frequency: %lu Hz, MS input divider: %lld, MS output divider: %lld\n",
			rate, best_in_div, best_out_div);
	} else {
		ms[1] = *parent_rate;
		ms[2] = rate;
		if (!rate) {
			dev_err(&drvdata->client->dev,
				"Invalid rate for MS%d\n", hwdata->num);
			return -EINVAL;
		}
		ms[0] = div64_u64(ms[1], ms[2]);
		ms[1] -= ms[0] * ms[2];
		remove_common_factor(&ms[1]);
		if (ms[0]  == 5 || ms[0] == 7)
			out_div++;
		rate = div64_u64(*parent_rate * ms[2], ms[1] + ms[0] * ms[2]);
		dev_dbg(&drvdata->client->dev,
			"Cloest MS output frequency: %lu Hz, output divider %llu+%llu/%llu\n",
			rate, ms[0], ms[1], ms[2]);
	}

	cal_ms_p(*parent_rate, rate, hwdata->params.p);
	hwdata->params.valid = true;

	return rate;
}

/*
 * multisynth's parent is PLL
 */
static int si5338_msynth_set_rate(struct clk_hw *hw, unsigned long rate,
				  unsigned long parent_rate)
{
	struct si5338_hw_data *hwdata = HWDATA(hw);
	struct si5338_driver_data *drvdata = hwdata->drvdata;

	if (!rate)
		rate = div64_u64(parent_rate + MSINT_MAX - 1, MSINT_MAX);

	cal_ms_p(parent_rate, rate, hwdata->params.p);
	hwdata->params.valid = true;

	return set_ms_p(drvdata, hwdata->params.p, hwdata->num);
}

static const struct clk_ops si5338_msynth_ops = {
	.prepare = si5338_msynth_prepare,
	.unprepare = si5338_msynth_unprepare,
	.recalc_rate = si5338_msynth_recalc_rate,
	.round_rate = si5338_msynth_round_rate,
	.set_rate = si5338_msynth_set_rate,
};

/*
 * Si5338 clkout
 */

static const u32 awe_out_disable[] = {
	AWE_OUT0_DIS,
	AWE_OUT1_DIS,
	AWE_OUT2_DIS,
	AWE_OUT3_DIS,
	AWE_OUT_ALL_DIS
};

static int set_out_disable(struct si5338_driver_data *drvdata,
			   int dis, int chn)
{
	int rc;

	rc = _verify_output_channel(chn);
	if (rc < 0)
		return rc;

	return write_field(drvdata, dis, awe_out_disable[chn]);
}

static const u32 awe_drv_dis_state[] = {
	AWE_OUT0_DIS_STATE,
	AWE_OUT1_DIS_STATE,
	AWE_OUT2_DIS_STATE,
	AWE_OUT3_DIS_STATE
};

static int si5338_clkout_set_disable_state(struct si5338_driver_data *drvdata,
					   int chn, int typ)
{
	int rc;

	rc = _verify_output_channel(chn);
	if (rc < 0)
		return rc;

	if (typ < 0 || typ > 3) {
		dev_err(&drvdata->client->dev,
			"Invalid disabled state %d. Only 0..3 are supported\n",
			typ);
		return -EINVAL;
	}

	return write_field(drvdata, typ, awe_drv_dis_state[chn]);
}

static const u32 awe_rdiv_in[] = {
	AWE_R0DIV_IN,
	AWE_R1DIV_IN,
	AWE_R2DIV_IN,
	AWE_R3DIV_IN
};

/*
 * src	0: fbclk
 *	1: refclk
 *	2: divfbclk
 *	3: divrefclk
 *	4: xoclk
 *	5: MS0
 *	6: MS1/2/3 respetivelly
 */
static int set_out_mux(struct si5338_driver_data *drvdata, int chn, int src)
{
	int rc;

	rc = _verify_output_channel(chn);
	if (rc < 0)
		return rc;

	if (src < 0 || src > 7) {
		dev_err(&drvdata->client->dev,
			"Invalid source %d. Only 0...7 are supported\n",
			src);
		return -EINVAL;
	}

	return write_field(drvdata, src, awe_rdiv_in[chn]);
}

static int get_out_mux(struct si5338_driver_data *drvdata, int chn)
{
	int rc;

	rc = _verify_output_channel(chn);
	if (rc < 0)
		return rc;

	return read_field(drvdata, awe_rdiv_in[chn]);
}

static const u32 awe_drv_fmt[] = {
	AWE_DRV0_FMT,
	AWE_DRV1_FMT,
	AWE_DRV2_FMT,
	AWE_DRV3_FMT
};

static int set_drv_type(struct si5338_driver_data *drvdata, int typ, int chn)
{
	int rc;

	rc = _verify_output_channel(chn);
	if (rc < 0)
		return rc;

	if (typ < 0 || typ > 7) {
		dev_err(&drvdata->client->dev,
			"Invalid output type %d. Only 0..7 are supported\n",
			typ);
		return -EINVAL;
	}

	return write_field(drvdata, typ, awe_drv_fmt[chn]);
}

static const u32 awe_drv_vddo[] = {
	AWE_DRV0_VDDO,
	AWE_DRV1_VDDO,
	AWE_DRV2_VDDO,
	AWE_DRV3_VDDO
};

static int set_drv_vdd(struct si5338_driver_data *drvdata, int vdd, int chn)
{
	int rc;

	rc = _verify_output_channel(chn);
	if (rc < 0)
		return rc;

	if (vdd < 0 || vdd > 7) {
		dev_err(&drvdata->client->dev,
			"Invalid output type %d. Only 0..3 are supported\n",
			vdd);
		return -EINVAL;
	}

	return write_field(drvdata, vdd, awe_drv_vddo[chn]);
}

static const u32 awe_drv_trim[][3] = {
	{ AWE_DRV0_TRIM, 0, 0 },
	{ AWE_DRV1_TRIM_A, AWE_DRV1_TRIM_B, 0},
	{ AWE_DRV2_TRIM, 0, 0},
	{ AWE_DRV3_TRIM, 0, 0}
};

static int set_drv_trim_any(struct si5338_driver_data *drvdata,
			    int trim, int chn)
{
	int rc;

	rc = _verify_output_channel(chn);
	if (rc < 0)
		return rc;

	if (trim < 0 || trim > 31) {
		dev_err(&drvdata->client->dev,
			"Invalid output type %d. Only 0..31 are supported\n",
			trim);
		return -EINVAL;
	}

	return write_multireg64(drvdata, trim, awe_drv_trim[chn]);
}

static const u32 awe_drv_invert[] = {
	AWE_DRV0_INV,
	AWE_DRV1_INV,
	AWE_DRV2_INV,
	AWE_DRV3_INV
};

static int set_drv_invert(struct si5338_driver_data *drvdata, int typ, int chn)
{
	int rc;

	rc = _verify_output_channel(chn);
	if (rc < 0)
		return rc;

	if (typ < 0 || typ > 3) {
		dev_err(&drvdata->client->dev,
			"Invalid invert drivers %d. Only 0..3 are supported\n",
			typ);
		return -EINVAL;
	}

	return write_field(drvdata, typ, awe_drv_invert[chn]);
}

static const u32 awe_drv_powerdown[] = {
	AWE_DRV0_PDN,
	AWE_DRV1_PDN,
	AWE_DRV2_PDN,
	AWE_DRV3_PDN
};

static int set_drv_powerdown(struct si5338_driver_data *drvdata,
			     int typ, int chn)
{
	int rc;

	rc = _verify_output_channel(chn);
	if (rc < 0)
		return rc;

	return write_field(drvdata, typ, awe_drv_powerdown[chn]);
}

static const struct si5338_drv_t const drv_configs[] = {
	{"3V3_CMOS_A+",	0x1, 0x0, 0x17, 0x8}, /* bX0 */
	{"3V3_CMOS_A-",	0x1, 0x0, 0x17, 0x9}, /* bX1 */
	{"3V3_CMOS_B+",	0x2, 0x0, 0x17, 0x4}, /* b0X */
	{"3V3_CMOS_B-",	0x2, 0x0, 0x17, 0x6}, /* b1X */
	{"3V3_CMOS_A+B+", 0x3, 0x0, 0x17, 0x8},
	{"3V3_CMOS_A-B+", 0x3, 0x0, 0x17, 0x9},
	{"3V3_CMOS_A+B-", 0x3, 0x0, 0x17, 0x4},
	{"3V3_CMOS_A-B-", 0x3, 0x0, 0x17, 0x6},

	{"2V5_CMOS_A+",	0x1, 0x1, 0x13, 0x8},
	{"2V5_CMOS_A-",	0x1, 0x1, 0x13, 0x9},
	{"2V5_CMOS_B+",	0x2, 0x1, 0x13, 0x4},
	{"2V5_CMOS_B-",	0x2, 0x1, 0x13, 0x6},
	{"2V5_CMOS_A+B+", 0x3, 0x1, 0x13, 0x8},
	{"2V5_CMOS_A-B+", 0x3, 0x1, 0x13, 0x9},
	{"2V5_CMOS_A+B-", 0x3, 0x1, 0x13, 0x4},
	{"2V5_CMOS_A-B-", 0x3, 0x1, 0x13, 0x6},

	{"1V8_CMOS_A+",	0x1, 0x2, 0x15, 0x8},
	{"1V8_CMOS_A-",	0x1, 0x2, 0x15, 0x9},
	{"1V8_CMOS_B+",	0x2, 0x2, 0x15, 0x4},
	{"1V8_CMOS_B-",	0x2, 0x2, 0x15, 0x6},
	{"1V8_CMOS_A+B+", 0x3, 0x2, 0x15, 0x8},
	{"1V8_CMOS_A-B+", 0x3, 0x2, 0x15, 0x9},
	{"1V8_CMOS_A+B-", 0x3, 0x2, 0x15, 0x4},
	{"1V8_CMOS_A-B-", 0x3, 0x2, 0x15, 0x6},

	{"1V5_HSTL_A+",	0x1, 0x3, 0x1f, 0x8},
	{"1V5_HSTL_A-",	0x1, 0x3, 0x1f, 0x9},
	{"1V5_HSTL_B+",	0x2, 0x3, 0x1f, 0x4},
	{"1V5_HSTL_B-",	0x2, 0x3, 0x1f, 0x6},
	{"1V5_HSTL_A+B+", 0x3, 0x3, 0x1f, 0x8},
	{"1V5_HSTL_A-B+", 0x3, 0x3, 0x1f, 0x9},
	{"1V5_HSTL_A+B-", 0x3, 0x3, 0x1f, 0x4},
	{"1V5_HSTL_A-B-", 0x3, 0x3, 0x1f, 0x6},

	{"3V3_SSTL_A+",	0x1, 0x0, 0x04, 0x8},
	{"3V3_SSTL_A-",	0x1, 0x0, 0x04, 0x9},
	{"3V3_SSTL_B+",	0x2, 0x0, 0x04, 0x4},
	{"3V3_SSTL_B-",	0x2, 0x0, 0x04, 0x6},
	{"3V3_SSTL_A+B+", 0x3, 0x0, 0x04, 0x8},
	{"3V3_SSTL_A-B+", 0x3, 0x0, 0x04, 0x9},
	{"3V3_SSTL_A+B-", 0x3, 0x0, 0x04, 0x5},
	{"3V3_SSTL_A-B-", 0x3, 0x0, 0x04, 0x6},

	{"2V5_SSTL_A+",	0x1, 0x1, 0x0d, 0x8},
	{"2V5_SSTL_A-",	0x1, 0x1, 0x0d, 0x9},
	{"2V5_SSTL_B+",	0x2, 0x1, 0x0d, 0x4},
	{"2V5_SSTL_B-",	0x2, 0x1, 0x0d, 0x6},
	{"2V5_SSTL_A+B+", 0x3, 0x1, 0x0d, 0x8},
	{"2V5_SSTL_A-B+", 0x3, 0x1, 0x0d, 0x9},
	{"2V5_SSTL_A+B-", 0x3, 0x1, 0x0d, 0x5},
	{"2V5_SSTL_A-B-", 0x3, 0x1, 0x0d, 0x6},

	{"1V8_SSTL_A+",	0x1, 0x2, 0x17, 0x8},
	{"1V8_SSTL_A-",	0x1, 0x2, 0x17, 0x9},
	{"1V8_SSTL_B+",	0x2, 0x2, 0x17, 0x4},
	{"1V8_SSTL_B-",	0x2, 0x2, 0x17, 0x6},
	{"1V8_SSTL_A+B+", 0x3, 0x2, 0x17, 0x8},
	{"1V8_SSTL_A-B+", 0x3, 0x2, 0x17, 0x9},
	{"1V8_SSTL_A+B-", 0x3, 0x2, 0x17, 0x4},
	{"1V8_SSTL_A-B-", 0x3, 0x2, 0x17, 0x6},

	{"3V3_LVPECL",	0x4, 0x0, 0x0f, 0xc},
	{"2V5_LVPECL",	0x4, 0x1, 0x10, 0xc},
	{"3V3_LVDS",	0x6, 0x0, 0x03, 0xc},
	{"2V5_LVDS",	0x6, 0x1, 0x04, 0xc},
	{"1V8_LVDS",	0x6, 0x2, 0x04, 0xc},

	{},
};

static int find_drive_config(const char *name)
{
	int i;

	for (i = 0; drv_configs[i].description; i++) {
		if (!strcmp(name, drv_configs[i].description))
			return i;
	}

	return -EINVAL;
}

static int si5338_clkout_set_drive_config(struct si5338_driver_data *drvdata,
				   int chn, const char *name)
{
	int i, rc;

	rc = _verify_output_channel(chn);
	if (rc < 0)
		return rc;

	i = find_drive_config(name);
	if (i < 0) {
		dev_err(&drvdata->client->dev,
			"Invalid driver configuration\n");
		return -EINVAL;
	}

	rc = set_drv_type(drvdata, drv_configs[i].fmt, chn);
	if (rc < 0)
		return rc;

	rc = set_drv_vdd(drvdata, drv_configs[i].vdd, chn);
	if (rc < 0)
		return rc;

	rc = set_drv_trim_any(drvdata, drv_configs[i].trim, chn);
	if (rc < 0)
		return rc;

	rc = set_drv_invert(drvdata,
			    drv_configs[i].invert & 3, chn);
	if (rc < 0)
		return rc;

	return 0;
}

static const u32 awe_rdiv_k[] = {
	AWE_R0DIV,
	AWE_R1DIV,
	AWE_R2DIV,
	AWE_R3DIV
};

static const u8 out_div_values[] = {
	1, 2, 4, 8, 16, 32
};

static int get_out_div(struct si5338_driver_data *drvdata, int chn)
{
	int rc;

	rc = _verify_output_channel(chn);
	if (rc < 0)
		return rc;

	rc = read_field(drvdata, awe_rdiv_k[chn]);
	if (rc < 0)
		return rc;

	if (rc >= ARRAY_SIZE(out_div_values)) {
		dev_err(&drvdata->client->dev,
			"Invalid value for output divider: %d\n",
			rc);
		return -EINVAL;
	}

	return out_div_values[rc];
}

static int set_out_div(struct si5338_driver_data *drvdata, int div, int chn)
{
	int rc;
	u8 val;

	rc = _verify_output_channel(chn);
	if (rc < 0)
		return rc;

	for (val = 0; val < ARRAY_SIZE(out_div_values); val++) {
		if (out_div_values[val] == div) {
			rc = write_field(drvdata, val, awe_rdiv_k[chn]);
			if (rc < 0)
				return rc;

			return 0;
		}
	}
	dev_err(&drvdata->client->dev,
		"Invalid value for output divider: %d\n",
		div);

	return -EINVAL;
}

static int get_status(struct si5338_driver_data *drvdata)
{
	return read_field(drvdata, AWE_STATUS);
}

static int power_up_down_needed_ms(struct si5338_driver_data *drvdata)
{
	int rc, chn, out_src;
	int ms_used = 0;

	for (chn = 0; chn < 4; chn++) {
		out_src = get_out_mux(drvdata, chn);
		if (out_src < 0)
			return out_src;

		switch (out_src) {
		case 5:
			ms_used |= 1;
			break;
		case 6:
			ms_used |= (1 << chn);
			break;
		}
	}
	for (chn = 0; chn < 4; chn++) {
		rc = set_ms_powerdown(drvdata,
				      (ms_used & (1 << chn)) ? MS_POWER_UP :
							       MS_POWER_DOWN,
				      chn);
		if (rc < 0)
			return rc;
	}

	return 0;
}

static const u32 awe_fcal[] = {
	AWE_FCAL_07_00,
	AWE_FCAL_15_08,
	AWE_FCAL_17_16,
	0
};

static const u32 awe_fcal_ovrd[] = {
	AWE_FCAL_OVRD_07_00,
	AWE_FCAL_OVRD_15_08,
	AWE_FCAL_OVRD_17_15,
	0
};

static int reset_ms(struct si5338_driver_data *drvdata)
{
	int rc;

	dev_dbg(&drvdata->client->dev, "Resetting MS dividers");
	/* SET MS RESET = 1 */
	rc = write_field(drvdata, 1, AWE_MS_RESET);
	if (rc)
		return rc;

	/* Wait for 20ms */
	msleep(20);

	/* SET MS RESET = 0 */
	rc = write_field(drvdata, 0, AWE_MS_RESET);
	if (rc)
		return rc;

	return 0;
}

static int set_misc_registers(struct si5338_driver_data *drvdata)
{
	/* ST52238 Reference Manual R1.2 p.28 */
	int rc;

	rc = write_field(drvdata, 0x5, AWE_MISC_47);
	if (rc)
		return rc;

	rc = write_field(drvdata, 0x1, AWE_MISC_106);
	if (rc)
		return rc;

	rc = write_field(drvdata, 0x1, AWE_MISC_116);
	if (rc)
		return rc;

	rc = write_field(drvdata, 0x1, AWE_MISC_42);
	if (rc)
		return rc;

	rc = write_field(drvdata, 0x0, AWE_MISC_06A);
	if (rc)
		return rc;

	rc = write_field(drvdata, 0x0, AWE_MISC_06B);
	if (rc)
		return rc;

	rc = write_field(drvdata, 0x0, AWE_MISC_28);
	if (rc)
		return rc;

	return 0;
}

/* Disable interrupt, all outputs */
static int pre_init(struct si5338_driver_data *drvdata)
{
	int rc, chn;

	/* Disable interrupts */
	rc = write_field(drvdata, 0x1d, AWE_INT_MASK);
	if (rc)
		return rc;

	/* setup miscelalneous registers */
	rc = set_misc_registers(drvdata);
	if (rc)
		return rc;

	/* disable all outputs */
	rc = write_field(drvdata, 1, AWE_OUT_ALL_DIS);
	if (rc)
		return rc;

	/* pause LOL */
	rc = write_field(drvdata, 1, AWE_DIS_LOS);
	if (rc)
		return rc;

	/* clears outputs pll input/fb muxes to be set later */
	for (chn = 0; chn < 4; chn++) {
		rc = set_ms_powerdown(drvdata, MS_POWER_DOWN, chn);
		if (rc)
			return rc;
		rc = set_out_disable(drvdata, OUT_DISABLE, chn);
		if (rc)
			return rc;
	}
	/* to be explicitly enabled if needed */
	rc = set_in_pfd_ref_fb(drvdata, 5, 0);	/* noclk */
	if (rc)
		return rc;

	rc = set_in_pfd_ref_fb(drvdata, 5, 1);	/* noclk */
	if (rc)
		return rc;

	return 0;
}

#define INIT_TIMEOUT		10	/* max 10 loops */

/* See SI5338 RM for programming procedure */
static int post_init(struct si5338_driver_data *drvdata)
{
	int rc = 0, i, in_src, fb_src, ext_fb, check_los = 0;
	int timeout = INIT_TIMEOUT;
	u64 fcal;

	/* validate input clock status */
	in_src = get_in_pfd_ref_fb(drvdata, 0);
	if (in_src < 0)
		return in_src;

	switch (in_src) {
	case SI5338_PFD_IN_REF_REFCLK:
	case SI5338_PFD_IN_REF_DIVREFCLK:
	case SI5338_PFD_IN_REF_XOCLK:
		check_los |= AWE_STATUS_PLL_LOS_CLKIN;
		break;
	case SI5338_PFD_IN_REF_FBCLK:
	case SI5338_PFD_IN_REF_DIVFBCLK:
		check_los |= AWE_STATUS_PLL_LOS_FDBK;
		break;
	}
	ext_fb = read_field(drvdata, AWE_PFD_EXTFB);
	if (ext_fb < 0)
		return ext_fb;

	if (ext_fb) {
		fb_src = get_in_pfd_ref_fb(drvdata, 1);
		if (fb_src < 0)
			return fb_src;

		switch (in_src) {
		case SI5338_PFD_IN_FB_REFCLK:
		case SI5338_PFD_IN_FB_DIVREFCLK:
			check_los |= AWE_STATUS_PLL_LOS_CLKIN;
			break;
		case SI5338_PFD_IN_FB_FBCLK:
		case SI5338_PFD_IN_FB_DIVFBCLK:
			check_los |= AWE_STATUS_PLL_LOS_FDBK;
			break;
		}
	}
	check_los &= 0xf;
	for (i = 0; i < timeout; i++) {
		rc = get_status(drvdata);
		if (rc < 0)
			return rc;

		if (!(rc & check_los))
			break; /* inputs OK */
		msleep(100);	/* wait for 100ms before polling */
	}

	if (i >= timeout) {
		dev_err(&drvdata->client->dev,
			"Timeout waiting for input clocks, status=0x%x, mask=0x%x\n",
			rc, check_los);
		return -ETIMEDOUT;
	}
	dev_dbg(&drvdata->client->dev,
		"Validated input clocks, t=%d cycles (timeout= %d cycles), status =0x%x, mask=0x%x\n",
		i, timeout, rc, check_los);

	/* Configure PLL for locking, set FCAL_OVRD_EN = 0 */
	rc = write_field(drvdata, 0, AWE_FCAL_OVRD_EN);
	if (rc < 0)
		return rc;

	/* Configure PLL for locking, set SOFT_RESET = 1 (ignore i2c error) */
	write_field(drvdata, 1, AWE_SOFT_RESET);
	msleep(25);

	/* re-enable LOL, set reg 241 = 0x65 */
	rc = write_field(drvdata, 0x65, AWE_REG241);
	if (rc < 0)
		return rc;

	check_los |= AWE_STATUS_PLL_LOL | AWE_STATUS_PLL_SYS_CAL;
	check_los &= 0xf;
	for (i = 0; i < timeout; i++) {
		rc = get_status(drvdata);
		if (rc < 0)
			return rc;

		if (!(rc & check_los))
			break; /* alarms not set OK */
		msleep(100);	/* wait for 100ms before polling */
	}
	if (i >= timeout) {
		dev_err(&drvdata->client->dev,
			"Timeout (%d) waiting for PLL lock, status=0x%x, mask=0x%x\n",
			i, rc, check_los);
		return -ETIMEDOUT;
	}
	dev_dbg(&drvdata->client->dev,
		"Validated PLL locked, t=%d cycles (timeout= %d cycles), status =0x%x, mask=0x%x\n",
		i, timeout, rc, check_los);

	/* copy FCAL values to active registers */
	rc = read_multireg64(drvdata, awe_fcal, &fcal);
	if (rc)
		return rc;

	rc = write_multireg64(drvdata, fcal, awe_fcal_ovrd);
	if (rc)
		return rc;

	dev_dbg(&drvdata->client->dev, "Copied FCAL data 0x%llx\n", fcal);
	/* Set 47[7:2] to 000101b */
	rc = write_field(drvdata, 5, AWE_REG47_72);
	if (rc)
		return rc;

	/* SET PLL to use FCAL values, set FCAL_OVRD_EN=1 */
	rc = write_field(drvdata, 1, AWE_FCAL_OVRD_EN);
	if (rc)
		return rc;

	/* only needed if using down-spread. Won't hurt to do anyway */
	rc = reset_ms(drvdata);
	if (rc)
		return rc;

	/* Enable all (enabled individually) outputs */
	rc = write_field(drvdata, 0, AWE_OUT_ALL_DIS);
	if (rc)
		return rc;

	/* Clearing */
	write_field(drvdata, 0, AWE_SOFT_RESET);

	/* Power up MS if used, otherwise power down */
	rc = power_up_down_needed_ms(drvdata);
	if (rc)
		return rc;

	return 0;
}

static int si5338_clkout_prepare(struct clk_hw *hw)
{
	struct si5338_hw_data *hwdata = HWDATA(hw);
	struct si5338_driver_data *drvdata = hwdata->drvdata;
	int rc;

	rc = set_drv_powerdown(drvdata, DRV_POWERUP, hwdata->num);
	if (rc) {
		dev_err(&drvdata->client->dev,
			"Error power up clkout%d\n", hwdata->num);
	}
	dev_dbg(&drvdata->client->dev, "Clkout%d prepared\n", hwdata->num);

	return rc;
}

static int si5338_clkout_enable(struct clk_hw *hw)
{

	struct si5338_hw_data *hwdata = HWDATA(hw);
	struct si5338_driver_data *drvdata = hwdata->drvdata;
	int rc;

	rc = set_out_disable(drvdata, OUT_ENABLE, hwdata->num);
	if (rc) {
		dev_err(&drvdata->client->dev,
			"Error enabling clkout%d\n", hwdata->num);
	}
	dev_dbg(&drvdata->client->dev, "Clkout%d enabled\n", hwdata->num);

	return rc;
}

static void si5338_clkout_disable(struct clk_hw *hw)
{
	struct si5338_hw_data *hwdata = HWDATA(hw);
	struct si5338_driver_data *drvdata = hwdata->drvdata;

	set_out_disable(drvdata, OUT_DISABLE, hwdata->num);
	dev_dbg(&drvdata->client->dev, "Clkout%d disable\n", hwdata->num);
}

static void si5338_clkout_unprepare(struct clk_hw *hw)
{
	struct si5338_hw_data *hwdata = HWDATA(hw);
	struct si5338_driver_data *drvdata = hwdata->drvdata;

	set_drv_powerdown(drvdata, DRV_POWERDOWN, hwdata->num);
	dev_dbg(&drvdata->client->dev, "Clkout%d unprepared\n", hwdata->num);
}

static int si5338_clkout_reparent(struct si5338_driver_data *drvdata,
				   int num, u8 parent)
{
	return set_out_mux(drvdata, num, parent);
}

static u8 si5338_clkout_get_parent(struct clk_hw *hw)
{
	struct si5338_hw_data *hwdata = HWDATA(hw);
	struct si5338_driver_data *drvdata = hwdata->drvdata;

	return get_out_mux(drvdata, hwdata->num);
}

static int si5338_clkout_set_parent(struct clk_hw *hw, u8 index)
{
	struct si5338_hw_data *hwdata = HWDATA(hw);
	struct si5338_driver_data *drvdata = hwdata->drvdata;

	return si5338_clkout_reparent(drvdata, hwdata->num, index);
}

static unsigned long si5338_clkout_recalc_rate(struct clk_hw *hw,
					       unsigned long parent_rate)
{
	struct si5338_hw_data *hwdata = HWDATA(hw);
	struct si5338_driver_data *drvdata = hwdata->drvdata;
	unsigned long rate = parent_rate;
	int rc;

	rc = get_out_div(drvdata, hwdata->num);
	if (rc <= 0) {
		rate = 0;
		dev_warn(&drvdata->client->dev,
			"Error recalculating rate for clk%d\n", hwdata->num);
	} else {
		rate /= rc;
	}
	dev_dbg(&drvdata->client->dev, "Recalculated clkout%d rate %lu\n",
		hwdata->num, rate);

	return rate;
}

static long si5338_clkout_round_rate(struct clk_hw *hw, unsigned long rate,
				     unsigned long *parent_rate)
{
	struct si5338_hw_data *hwdata = HWDATA(hw);
	struct si5338_driver_data *drvdata = hwdata->drvdata;
	u64 out_freq_scaled, scaled_max;
	unsigned long err, new_rate, new_err;
	u8 r_div = 1;

	out_freq_scaled = rate;
	/* Request frequency if multisynth master */
	if (__clk_get_flags(hw->clk) & CLK_SET_RATE_PARENT) {
		scaled_max = div64_u64(FVCOMAX,  MSINT_MAX);
		while (r_div < 32 && out_freq_scaled < scaled_max) {
			out_freq_scaled <<= 1;
			r_div <<= 1;
		}
		if (out_freq_scaled < scaled_max) {
			dev_warn(&drvdata->client->dev,
				 "Specified output frequency is too low: %lu < %lld\n",
				 rate, scaled_max >> 5);
			r_div = 32;
			*parent_rate = scaled_max;
		} else {
			*parent_rate = out_freq_scaled;
		}
	} else {
		/* round to closest r_div */
		new_rate = *parent_rate;
		new_err = abs(new_rate - rate);
		do {
			err = new_err;
			new_rate >>= 1;
			r_div <<= 1;
			new_err = abs(new_rate - rate);
		} while (new_err < err && r_div < 32);
		r_div >>= 1;
	}
	rate = *parent_rate / r_div;

	dev_dbg(&drvdata->client->dev,
		"%s - %s: r_div = %u, rate = %lu, requesting parent_rate = %lu\n",
		__func__, __clk_get_name(hwdata->hw.clk), r_div,
		rate, *parent_rate);

	return rate;
}

static int si5338_clkout_set_rate(struct clk_hw *hw, unsigned long rate,
				  unsigned long parent_rate)
{
	struct si5338_hw_data *hwdata = HWDATA(hw);
	struct si5338_driver_data *drvdata = hwdata->drvdata;
	unsigned long err, new_rate, new_err;
	int r_div = 1;

	/* round to closest r_div */
	new_rate = parent_rate;
	new_err = abs(new_rate - rate);
	do {
		err = new_err;
		new_rate >>= 1;
		r_div <<= 1;
		new_err = abs(new_rate - rate);
	} while (new_err < err && r_div < 32);
	r_div >>= 1;

	dev_dbg(&drvdata->client->dev,
		"%s - %s: r_div = %u, parent_rate = %lu, rate = %lu\n",
		__func__, __clk_get_name(hwdata->hw.clk), r_div,
		parent_rate, rate);

	return set_out_div(drvdata, r_div, hwdata->num);
}

static const struct clk_ops si5338_clkout_ops = {
	.prepare = si5338_clkout_prepare,
	.unprepare = si5338_clkout_unprepare,
	.enable = si5338_clkout_enable,
	.disable = si5338_clkout_disable,
	.set_parent = si5338_clkout_set_parent,
	.get_parent = si5338_clkout_get_parent,
	.recalc_rate = si5338_clkout_recalc_rate,
	.round_rate = si5338_clkout_round_rate,
	.set_rate = si5338_clkout_set_rate,
};

#ifdef CONFIG_DEBUG_FS

static int get_ms_powerdown(struct si5338_driver_data *drvdata, int chn)
{
	int rc;

	rc = _verify_output_channel(chn);
	if (rc < 0)
		return rc;

	return read_field(drvdata, awe_ms_powerdown[chn]);
}

static int get_out_disable(struct si5338_driver_data *drvdata, int chn)
{
	int rc;

	rc = _verify_output_channel(chn);
	if (chn != 4 && rc < 0)
		return rc;

	return read_field(drvdata, awe_out_disable[chn]);
}

static int get_drv_disabled_state(struct si5338_driver_data *drvdata, int chn)
{
	int rc;

	rc = _verify_output_channel(chn);
	if (rc < 0)
		return rc;

	return read_field(drvdata, awe_drv_dis_state[chn]);
}

static int get_drv_type(struct si5338_driver_data *drvdata, int chn)
{
	int rc;

	rc = _verify_output_channel(chn);
	if (rc < 0)
		return rc;

	return read_field(drvdata, awe_drv_fmt[chn]);
}

static int get_drv_vdd(struct si5338_driver_data *drvdata, int chn)
{
	int rc;

	rc = _verify_output_channel(chn);
	if (rc < 0)
		return rc;

	return read_field(drvdata, awe_drv_vddo[chn]);
}

static int get_drv_trim(struct si5338_driver_data *drvdata, int chn)
{
	int rc;
	u64 data;

	rc = _verify_output_channel(chn);
	if (rc < 0)
		return rc;

	rc = read_multireg64(drvdata, awe_drv_trim[chn], &data);
	if (rc < 0)
		return rc;

	return data;	/* 5-bit data */
}

static int get_drv_invert(struct si5338_driver_data *drvdata, int chn)
{
	int rc;

	rc = _verify_output_channel(chn);
	if (rc < 0)
		return rc;

	return read_field(drvdata, awe_drv_invert[chn]);
}

static int get_drv_powerdown(struct si5338_driver_data *drvdata, int chn)
{
	int rc;

	rc = _verify_output_channel(chn);
	if (rc < 0)
		return rc;

	return read_field(drvdata, awe_drv_powerdown[chn]);
}

/*
 * Create debugfs files for status for each si5338 clkout
 */

static int clkout_status_show(struct seq_file *s, void *data)
{
	struct si5338_hw_data *clkout = s->private;
	struct si5338_driver_data *drvdata = clkout->drvdata;
	struct si5338_drv_t const *config;
	int i, j, match = 0;
	int drv_type, drv_vdd, drv_trim, drv_invert;
	int out_src, src_group = 0, src = 0;
	const int in_numbers[] = {
		12, 3, 4, 56
	};

	i = clkout->num;
	seq_printf(s, "%d: ", i);
	if (get_out_disable(drvdata, i)) {
		seq_puts(s, "disabled");
		switch (get_drv_disabled_state(drvdata, i)) {
		case SI5338_OUT_DIS_HIZ:
			seq_puts(s, " (high-Z)\n");
			break;
		case SI5338_OUT_DIS_LOW:
			seq_puts(s, " (low)\n");
			break;
		case SI5338_OUT_DIS_HI:
			seq_puts(s, " (high)\n");
			break;
		case SI5338_OUT_DIS_ALWAYS_ON:
			seq_puts(s, " (always on)\n");
			break;
		}
		return 0;
	}

	seq_puts(s, "enabled ");
	drv_type = get_drv_type(drvdata, i);
	if (drv_type < 0)
		return drv_type;

	drv_vdd =  get_drv_vdd(drvdata, i);
	if (drv_vdd < 0)
		return drv_vdd;

	drv_trim = get_drv_trim(drvdata, i);
	if (drv_trim < 0)
		return drv_trim;

	drv_invert = get_drv_invert(drvdata, i);
	if (drv_invert < 0)
		return drv_invert;

	for (j = 0; drv_configs[j].description; j++) {
		config = &drv_configs[j];
		if (config->fmt != drv_type)
			continue;
		if (config->vdd != drv_vdd)
			continue;
		if (config->trim != drv_trim)
			continue;
		if (((config->invert >> 2) | drv_invert) !=
		    ((config->invert >> 2) | (config->invert & 3)))
			continue;

		seq_puts(s, drv_configs[j].description);
		match = 1;
		break;
	}

	if (!match) {
		seq_printf(s, "Invalid output configuration: type = %d, vdd=%d, trim=%d, invert=%d\n",
			   drv_type, drv_vdd, drv_trim, drv_invert);
	}

	seq_printf(s, ", R%d and out %d power %s", i, i,
		  get_drv_powerdown(drvdata, i) ? "down" : "up");
	seq_puts(s, ", Output route ");

	out_src = get_out_mux(drvdata, i);
	if (out_src < 0)
		return out_src;

	switch (out_src) {
	case 0: /* p2div in */
	case 2: /* p2div out */
		src = read_field(drvdata, AWE_FB_MUX);
		if (src < 0)
			return src;

		src_group = 0;
		src = src ? 2 : 3; /* mod src: 0 - IN56, 1 - IN4 */
		break;
	case 1: /* p1div in */
	case 3: /* p1div out */
		src = read_field(drvdata, AWE_IN_MUX);
		if (src < 0)
			return src;

		if (src == 2) {
			src_group = 1;
			src = 0;
		} else {
			src_group = 0; /* keep src: 0 - IN12, 1 - IN3 */
		}
		break;
	case 4:
		src_group = 1;
		break;
	case 5:
		src_group = 2;
		src = 0;
		break;
	case 6:
		src_group = 2;
		src = i;
		break;
	case 7:
		src_group = 3;
		break;
	}
	switch (src_group) {
	case 0:
		seq_printf(s, "IN%d", in_numbers[src]);
		break;
	case 1:
		seq_puts(s, "XO");
		break;
	case 2:
		seq_printf(s, "MS%d", src);
		break;
	case 3:
		seq_puts(s, "No clock");
		break;
	}

	if (out_src == 5 || out_src == 6) {
		seq_printf(s, " power %s",
			   get_ms_powerdown(drvdata, i) ? "down" : "up");
	}

	seq_puts(s, "\n");

	return 0;
}

static int clkout_status_open(struct inode *inode, struct file *file)
{
	return single_open(file, clkout_status_show, inode->i_private);
}

static const struct file_operations clkout_status_fops = {
	.open		= clkout_status_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};

static int register_debugfs_status(struct si5338_hw_data *clkout)
{
	struct dentry *d;

	d = debugfs_create_file("output_status", S_IRUGO, d, clkout,
				&clkout_status_fops);
	if (!d)
		return -ENOMEM;

	return 0;
}

#else
static int register_debugfs_status(struct si5338_hw_data *clkout)
{
	return 0;
}
#endif /* CONFIG_DEBUG_FS */

/*
 * Si5351 i2c probe and device tree parsing
 */
#ifdef CONFIG_OF
static const struct of_device_id si5338_dt_ids[] = {
	{ .compatible = "silabs,si5338" },
	{ }
};
MODULE_DEVICE_TABLE(of, si5338_dt_ids);

static int si5338_dt_parse(struct i2c_client *client)
{
	struct device_node *child, *np = client->dev.of_node;
	struct si5338_platform_data *pdata;
	u32 val, num;

	if (np == NULL)
		return 0;

	pdata = devm_kzalloc(&client->dev, sizeof(*pdata), GFP_KERNEL);
	if (!pdata)
		return -ENOMEM;

	/* property silab,name-prefix */
	of_property_read_string(np, "silab,name-prefix", &pdata->name_prefix);

	/* property silab,ref-source */
	if (!of_property_read_u32(np, "silab,ref-source", &val)) {
		switch (val) {
		case SI5338_REF_SRC_CLKIN12:
		case SI5338_REF_SRC_CLKIN3:
		case SI5338_REF_SRC_XTAL:
			pdata->ref_src = val;
			dev_dbg(&client->dev, "ref-source = %d\n", val);
			break;
		default:
			dev_err(&client->dev,
				"Invalid source for refclk %u\n", val);
			return -EINVAL;
		}
	}

	/* property silab,fb-source */
	if (!of_property_read_u32(np, "silab,fb-source", &val)) {
		switch (val) {
		case SI5338_FB_SRC_CLKIN4:
		case SI5338_FB_SRC_CLKIN56:
		case SI5338_FB_SRC_NOCLK:
			pdata->fb_src = val;
			dev_dbg(&client->dev, "fb-source = %d\n", val);
			break;
		default:
			dev_err(&client->dev,
				"Invalid source for fbclk %u\n", val);
			return -EINVAL;
		}
	}

	/* property silab,pll-source */
	if (!of_property_read_u32(np, "silab,pll-source", &val)) {
		switch (val) {
		case SI5338_PFD_IN_REF_REFCLK:
		case SI5338_PFD_IN_REF_FBCLK:
		case SI5338_PFD_IN_REF_DIVREFCLK:
		case SI5338_PFD_IN_REF_DIVFBCLK:
		case SI5338_PFD_IN_REF_XOCLK:
		case SI5338_PFD_IN_REF_NOCLK:
			pdata->pll_src = val;
			dev_dbg(&client->dev, "pll-source = %d\n", val);
			break;
		default:
			dev_err(&client->dev,
				"Invalid source for pll %u\n", val);
			return -EINVAL;
		}
	}

	/* property silab,pll-vco */
	if (!of_property_read_u32(np, "silab,pll-vco", &val)) {
		if (val < FVCOMIN || val > FVCOMAX) {
			dev_err(&client->dev,
				"pll-vco out of range [%lldu..%lldu]\n",
				FVCOMIN, FVCOMAX);
			return -EINVAL;
		}
		pdata->pll_vco = val;
	}

	if (!of_property_read_u32(np, "silab,pll-master", &val)) {
		if (val > 3) {
			dev_err(&client->dev,
				"Invalid pll-master %u\n", val);
			return -EINVAL;
		}
		pdata->pll_master = val;
		dev_dbg(&client->dev, "pll-master = %d\n", val);
	}

	/* per clock out */
	for_each_child_of_node(np, child) {
		if (of_property_read_u32(child, "reg", &num)) {
			dev_err(&client->dev, "Missing reg property of %s\n",
				child->name);
			return -EINVAL;
		}
		if (num > 4) {
			dev_err(&client->dev, "Invalid clkout %u\n", num);
			return -EINVAL;
		}

		of_property_read_string(child, "name",
					&pdata->clkout[num].name);

		if (!of_property_read_u32(child, "silabs,clock-source", &val)) {
			switch (val) {
			case SI5338_OUT_MUX_FBCLK:
			case SI5338_OUT_MUX_REFCLK:
			case SI5338_OUT_MUX_DIVFBCLK:
			case SI5338_OUT_MUX_DIVREFCLK:
			case SI5338_OUT_MUX_XOCLK:
			case SI5338_OUT_MUX_MS0:
			case SI5338_OUT_MUX_MSN:
			case SI5338_OUT_MUX_NOCLK:
				pdata->clkout[num].clkout_src = val;
				dev_dbg(&client->dev, "clkout_src = %d\n", val);
				break;
			default:
				dev_err(&client->dev,
					"Invalid source for output %u\n", num);
				return -EINVAL;
			}
		}
		if (!of_property_read_string(child, "silabs,drive-config",
					     &pdata->clkout[num].drive)) {
			if (find_drive_config(pdata->clkout[num].drive) < 0) {
				dev_err(&client->dev,
					"Invalid drive config for output %u\n",
					num);
				return -EINVAL;
			}
			dev_dbg(&client->dev, "drive-config = %s\n",
				pdata->clkout[num].drive);
		}
		if (!of_property_read_u32(child,
					  "silabs,disable-state",
					  &val)) {
			switch (val) {
			case SI5338_OUT_DIS_HIZ:
			case SI5338_OUT_DIS_LOW:
			case SI5338_OUT_DIS_HI:
			case SI5338_OUT_DIS_ALWAYS_ON:
				pdata->clkout[num].disable_state = val;
				dev_dbg(&client->dev,
					"disable-state = %d\n", val);
				break;
			default:
				dev_err(&client->dev,
					"Invalid disable state for output %u\n",
					num);
				return -EINVAL;
			}
		}
		if (!of_property_read_u32(child, "clock-frequency", &val)) {
			pdata->clkout[num].rate = val;
			dev_dbg(&client->dev, "clock-frequency = %d\n", val);
		}
		if (of_find_property(child, "enabled", NULL))
			pdata->clkout[num].enabled = true;
	}
	/* Replace platform data with device tree */
	client->dev.platform_data = pdata;

	return 0;
}
#else
static int si5338_dt_parse(struct i2c_client *client)
{
	return 0;
}
#endif /* CONFIG_OF */

/*
 * Returns the clk registered, or an error code. If successful, the clk pointer
 * is also save in hw->clk.
 */
static struct clk *si5338_register_clock(struct device *dev,
			      struct clk_hw *hw,
			      const char *name,
			      const char **parent_names,
			      u8 num_parents,
			      const struct clk_ops *ops,
			      unsigned long flags)
{
	struct clk *clk;
	struct clk_init_data init;

	memset(&init, 0, sizeof(init));
	init.name = name;
	init.ops = ops;
	init.flags = flags;
	init.parent_names = parent_names;
	init.num_parents = num_parents;
	hw->init = &init;
	dev_dbg(dev, "Registering %s\n", name);
	clk = devm_clk_register(dev, hw);

	if (IS_ERR(clk))
		dev_err(dev, "unable to register %s\n", name);

	return clk;
}

#define STRNCAT_LENGTH (MAX_NAME_LENGTH - name_prefix_length)
static int si5338_i2c_probe(struct i2c_client *client,
			    const struct i2c_device_id *id)
{
	struct si5338_platform_data *pdata;
	struct si5338_driver_data *drvdata;
	struct clk *clk = NULL;
	char name_buf[8][MAX_NAME_LENGTH];
	char register_name[MAX_NAME_LENGTH];
	const char *pclkin[4] = {"in12", "in3", "in4", "in56"};
	const char *parent_names[8] = {
		name_buf[0], name_buf[1], name_buf[2], name_buf[3],
		name_buf[4], name_buf[5], name_buf[6], name_buf[7]
	};
	int ret, n, name_prefix_length;
	bool require_xtal = false;
	bool require_ref = false;
	bool require_fb = false;
	bool require_pll = false;
	unsigned long flags;

	ret = si5338_dt_parse(client);
	if (ret)
		return ret;

	pdata = client->dev.platform_data;
	if (!pdata)
		return -EINVAL;

	drvdata = devm_kzalloc(&client->dev, sizeof(*drvdata), GFP_KERNEL);
	if (drvdata == NULL)
		return -ENOMEM;

	i2c_set_clientdata(client, drvdata);
	drvdata->client = client;
	if (!pdata->name_prefix) {
		strlcpy(drvdata->name_prefix,
			dev_name(&client->dev), MAX_NAME_PREFIX - 2);
		strncat(drvdata->name_prefix, "-", 1);
	} else {
		strlcpy(drvdata->name_prefix,
			pdata->name_prefix, MAX_NAME_PREFIX);
	}
	name_prefix_length = strlen(drvdata->name_prefix);

	/* Check if clkout config is valid */
	for (n = 0; n < 4; n++) {
		/* check clkout source config */
		switch (pdata->clkout[n].clkout_src) {
		case SI5338_OUT_MUX_NOCLK:
			if (pdata->clkout[n].rate)
				pdata->clkout[n].rate = 0;
			break;
		case SI5338_OUT_MUX_REFCLK:
		case SI5338_OUT_MUX_DIVREFCLK:
			require_ref = true;
			break;
		case SI5338_OUT_MUX_FBCLK:
		case SI5338_OUT_MUX_DIVFBCLK:
			require_fb = true;
			break;
		case SI5338_OUT_MUX_XOCLK:
			require_xtal = true;
			break;
		case SI5338_OUT_MUX_MS0:
		case SI5338_OUT_MUX_MSN:
			require_pll = true;
			break;
		default:
			dev_err(&client->dev, "Invalid clkout source\n");
			return -EINVAL;
		}

		/* check clkout drive config */
		if (find_drive_config(pdata->clkout[n].drive) < 0) {
			dev_err(&client->dev,
				"Invalid drive config for output %u\n", n);
			return -EINVAL;
		}

		/* check clkout disable state config */
		switch (pdata->clkout[n].disable_state) {
		case SI5338_OUT_DIS_HIZ:
		case SI5338_OUT_DIS_LOW:
		case SI5338_OUT_DIS_HI:
		case SI5338_OUT_DIS_ALWAYS_ON:
			break;
		default:
			dev_err(&client->dev,
				"Invalid disable state for output %u\n", n);
			return -EINVAL;
		}

	}
	/* check pll source */
	if (require_pll) {
		switch (pdata->pll_src) {
		case SI5338_PFD_IN_REF_XOCLK:
			require_xtal = true;
			break;
		case SI5338_PFD_IN_REF_REFCLK:
		case SI5338_PFD_IN_REF_DIVREFCLK:
			require_ref = true;
			break;
		case SI5338_PFD_IN_REF_FBCLK:
		case SI5338_PFD_IN_REF_DIVFBCLK:
			require_fb = true;
			break;
		case SI5338_PFD_IN_REF_NOCLK:
		default:
			dev_err(&client->dev, "Invalid pll source\n");
			return -EINVAL;
		}
	}
	/* check refclk source */
	if (require_ref) {
		switch (pdata->ref_src) {
		case SI5338_REF_SRC_CLKIN12:
			if (require_xtal) {
				dev_err(&client->dev,
					"Error in configuration: IN1/IN2 and XTAL are mutually exclusive\n");
				return -EINVAL;
			}
			drvdata->pclkin[0] = devm_clk_get(&client->dev,
							  pclkin[0]);
			if (PTR_ERR(drvdata->pclkin[0]) == -EPROBE_DEFER)
				return -EPROBE_DEFER;
			if (IS_ERR_OR_NULL(drvdata->pclkin[0])) {
				dev_err(&client->dev,
					"IN1/IN2 doesn't a have source\n");
				return -EINVAL;
			}
			break;
		case SI5338_REF_SRC_CLKIN3:
			drvdata->pclkin[1] = devm_clk_get(&client->dev,
							  pclkin[1]);
			if (PTR_ERR(drvdata->pclkin[1]) == -EPROBE_DEFER)
				return -EPROBE_DEFER;
			if (IS_ERR_OR_NULL(drvdata->pclkin[1])) {
				dev_err(&client->dev,
					"IN3 doesn't have a source\n");
				return -EINVAL;
			}
			break;
		default:
			dev_err(&client->dev,
				"Invalid source for refclk\n");
			return -EINVAL;
		}
	}
	/* check fbclk source */
	if (require_fb) {
		switch (pdata->fb_src) {
		case SI5338_FB_SRC_CLKIN4:
			drvdata->pclkin[2] = devm_clk_get(&client->dev,
							  pclkin[2]);
			if (PTR_ERR(drvdata->pclkin[2]) == -EPROBE_DEFER)
				return -EPROBE_DEFER;
			if (IS_ERR_OR_NULL(drvdata->pclkin[2])) {
				dev_err(&client->dev,
					"IN4 doesn't have a source\n");
				return -EINVAL;
			}
			break;
		case SI5338_FB_SRC_CLKIN56:
			drvdata->pclkin[3] = devm_clk_get(&client->dev,
							  pclkin[3]);
			if (PTR_ERR(drvdata->pclkin[3]) == -EPROBE_DEFER)
				return -EPROBE_DEFER;
			if (IS_ERR_OR_NULL(drvdata->pclkin[3])) {
				dev_err(&client->dev,
					"IN5/IN6 doesn't have a source\n");
				return -EINVAL;
			}
			break;
		case SI5338_FB_SRC_NOCLK:
		default:
			dev_err(&client->dev,
				"Invalid source for fbclk\n");
			return -EINVAL;
		}
	}
	/* check xtal */
	if (require_xtal) {
		drvdata->pxtal = devm_clk_get(&client->dev, "xtal");
		if (PTR_ERR(drvdata->pxtal) == -EPROBE_DEFER)
			return -EPROBE_DEFER;
		if (IS_ERR_OR_NULL(drvdata->pxtal)) {
			dev_err(&client->dev,
				"XTAL doesn't have a source\n");
			return -EINVAL;
		}
	}

	/* Register regmap */
	drvdata->regmap = devm_regmap_init_i2c(client, &si5338_regmap_config);
	if (IS_ERR(drvdata->regmap)) {
		dev_err(&client->dev, "failed to allocate register map\n");
		return PTR_ERR(drvdata->regmap);
	}

	ret = regmap_read(drvdata->regmap, REG5338_DEV_CONFIG2, &n);
	if (ret) {
		dev_err(&client->dev, "Failed to access regmap\n");
		return ret;
	}

	/* Check if si5338 exists */
	if ((n & REG5338_DEV_CONFIG2_MASK) != REG5338_DEV_CONFIG2_VAL) {
		dev_err(&client->dev,
			"Chip returned unexpected value from reg 0x%x: 0x%x, expected 0x%x. It is not %s\n",
			REG5338_DEV_CONFIG2, n, REG5338_DEV_CONFIG2_VAL,
			id->name);
		return -ENODEV;
	}

	dev_dbg(&client->dev, "Chip %s is found\n", id->name);

	ret = pre_init(drvdata);		/* Disable all */
	if (ret)
		return ret;

	/*
	 * Set up clock structure
	 * These clocks have fixed parent
	 *	xtal => xoclk
	 *	refclk => divrefclk
	 *	fbclk => divfbclk
	 *	pll => multisynth
	 */

	/* setup refclk parent */
	ret = si5338_refclk_reparent(drvdata, pdata->ref_src);
	if (ret) {
		dev_err(&client->dev,
			"failed to reparent refclk to %d\n", pdata->ref_src);
		return ret;
	}

	/* setup fbclk parent */
	ret = si5338_fbclk_reparent(drvdata, pdata->fb_src);
	if (ret) {
		dev_err(&client->dev,
			"failed to reparent fbclk to %d\n", pdata->fb_src);
		return ret;
	}

	/* setup pll parent */
	ret = si5338_pll_reparent(drvdata, pdata->pll_src);
	if (ret) {
		dev_err(&client->dev,
			"failed to reparent pll %d to %d\n",
			n, pdata->pll_src);
		return ret;
	}

	for (n = 0; n < 4; n++) {
		ret = si5338_clkout_reparent(drvdata, n,
					      pdata->clkout[n].clkout_src);
		if (ret) {
			dev_err(&client->dev,
				"failed to reparent clkout %d to %d\n",
				n, pdata->clkout[n].clkout_src);
			return ret;
		}

		ret = si5338_clkout_set_drive_config(drvdata, n,
					      pdata->clkout[n].drive);
		if (ret) {
			dev_err(&client->dev,
				"failed set drive config of clkout%d to %s\n",
				n, pdata->clkout[n].drive);
			return ret;
		}

		ret = si5338_clkout_set_disable_state(drvdata, n,
						pdata->clkout[n].disable_state);
		if (ret) {
			dev_err(&client->dev,
				"failed set disable state of clkout%d to %d\n",
				n, pdata->clkout[n].disable_state);
			return ret;
		}
	}

	/*
	 * To form clock names, concatentate name prefix with each name.
	 * The result string is up to MAX_NAME_LENGTH including termination.
	 */

	/* Register xtal input clock */
	if (!IS_ERR_OR_NULL(drvdata->pxtal)) {
		strlcpy(register_name, drvdata->name_prefix, MAX_NAME_PREFIX);
		strncat(register_name, si5338_input_names[4], STRNCAT_LENGTH);
		drvdata->pxtal_name = __clk_get_name(drvdata->pxtal);
		clk = si5338_register_clock(&client->dev, &drvdata->xtal,
					 register_name, &drvdata->pxtal_name, 1,
					 &si5338_xtal_ops, 0);
		if (IS_ERR(clk))
			return PTR_ERR(clk);
	}

	/* Register clkin input clock */
	for (n = 0; n < 4; n++) {
		if (IS_ERR_OR_NULL(drvdata->pclkin[n]))
			continue;

		drvdata->clkin[n].drvdata = drvdata;
		drvdata->clkin[n].num = n;
		strlcpy(register_name, drvdata->name_prefix, MAX_NAME_PREFIX);
		strncat(register_name, si5338_input_names[n], STRNCAT_LENGTH);
		drvdata->pclkin_name[n] = __clk_get_name(drvdata->pclkin[n]);

		clk = si5338_register_clock(&client->dev,
					    &drvdata->clkin[n].hw,
					    register_name,
					    &drvdata->pclkin_name[n],
					    1,
					    &si5338_clkin_ops,
					    0);
		if (IS_ERR(clk))
			return PTR_ERR(clk);
	}

	/*
	 * Create unique internal names in case multiple devices exist
	 *
	 * Register refclk, parents can be in1/in2, in3, xtal, noclk
	 */
	drvdata->refclk.drvdata = drvdata;
	strlcpy(name_buf[0], drvdata->name_prefix, MAX_NAME_PREFIX);
	strlcpy(name_buf[1], drvdata->name_prefix, MAX_NAME_PREFIX);
	strlcpy(name_buf[2], drvdata->name_prefix, MAX_NAME_PREFIX);
	strlcpy(name_buf[3], drvdata->name_prefix, MAX_NAME_PREFIX);
	strncat(name_buf[0], si5338_input_names[0], STRNCAT_LENGTH);
	strncat(name_buf[1], si5338_input_names[1], STRNCAT_LENGTH);
	strncat(name_buf[2], si5338_input_names[4], STRNCAT_LENGTH);
	strncat(name_buf[3], si5338_input_names[5], STRNCAT_LENGTH);
	strlcpy(register_name, drvdata->name_prefix, MAX_NAME_PREFIX);
	strncat(register_name, si5338_pll_src_names[0], STRNCAT_LENGTH);

	clk = si5338_register_clock(&client->dev, &drvdata->refclk.hw,
				 register_name, parent_names, 4,
				 &si5338_refclk_ops, 0);
	if (IS_ERR(clk))
		return PTR_ERR(clk);

	/* Register divrefclk, parent is refclk */
	strlcpy(name_buf[0], drvdata->name_prefix, MAX_NAME_PREFIX);
	strncat(name_buf[0], si5338_pll_src_names[0], STRNCAT_LENGTH);
	strlcpy(register_name, drvdata->name_prefix, MAX_NAME_PREFIX);
	strncat(register_name, si5338_pll_src_names[2], STRNCAT_LENGTH);

	clk = si5338_register_clock(&client->dev, &drvdata->divrefclk,
				 register_name, parent_names, 1,
				 &si5338_divrefclk_ops, 0);
	if (IS_ERR(clk))
		return PTR_ERR(clk);

	/* Register fbclk, parents can be in4, in5/in6, noclk */
	drvdata->fbclk.drvdata = drvdata;
	strlcpy(name_buf[0], drvdata->name_prefix, MAX_NAME_PREFIX);
	strlcpy(name_buf[1], drvdata->name_prefix, MAX_NAME_PREFIX);
	strlcpy(name_buf[2], drvdata->name_prefix, MAX_NAME_PREFIX);
	strncat(name_buf[0], si5338_input_names[2], STRNCAT_LENGTH);
	strncat(name_buf[1], si5338_input_names[3], STRNCAT_LENGTH);
	strncat(name_buf[2], si5338_input_names[5], STRNCAT_LENGTH);
	strlcpy(register_name, drvdata->name_prefix, MAX_NAME_PREFIX);
	strncat(register_name, si5338_pll_src_names[1], STRNCAT_LENGTH);

	clk = si5338_register_clock(&client->dev, &drvdata->fbclk.hw,
				 register_name, parent_names, 3,
				 &si5338_fbclk_ops, 0);
	if (IS_ERR(clk))
		return PTR_ERR(clk);

	/* Register divfbclk, parent is fbclk */
	strlcpy(name_buf[0], drvdata->name_prefix, MAX_NAME_PREFIX);
	strncat(name_buf[0], si5338_pll_src_names[1], STRNCAT_LENGTH);
	strlcpy(register_name, drvdata->name_prefix, MAX_NAME_PREFIX);
	strncat(register_name, si5338_pll_src_names[3], STRNCAT_LENGTH);

	clk = si5338_register_clock(&client->dev, &drvdata->divfbclk,
				 register_name, parent_names, 1,
				 &si5338_divfbclk_ops, 0);
	if (IS_ERR(clk))
		return PTR_ERR(clk);

	/* register PLL */
	drvdata->pll.drvdata = drvdata;
	for (n = 0; n < ARRAY_SIZE(si5338_pll_src_names); n++) {
		strlcpy(name_buf[n], drvdata->name_prefix, MAX_NAME_PREFIX);
		strncat(name_buf[n], si5338_pll_src_names[n], STRNCAT_LENGTH);
	}
	strlcpy(register_name, drvdata->name_prefix, MAX_NAME_PREFIX);
	strncat(register_name, si5338_msynth_src_names[0], STRNCAT_LENGTH);
	clk = si5338_register_clock(&client->dev, &drvdata->pll.hw,
				 register_name, parent_names, 5,
				 &si5338_pll_ops, 0);
	if (IS_ERR(clk))
		return PTR_ERR(clk);

	/* If pll_vco is specified, always use it to set pll clock */
	if (require_pll && pdata->pll_vco) {
		if (pdata->pll_vco > FVCOMIN && pdata->pll_vco < FVCOMAX) {
			dev_dbg(&client->dev, "Setting pll vco rate to %u\n",
				pdata->pll_vco);
			ret = clk_set_rate(clk, pdata->pll_vco);
			if (ret) {
				dev_err(&client->dev, "Cannot set pll vco rate : %d\n",
					ret);
				return ret;
			}
		} else {
			pdata->pll_vco = 0;
		}
	}

	/* register clk multisync and clk out divider */
	drvdata->msynth = devm_kzalloc(&client->dev, 4 *
				       sizeof(*drvdata->msynth), GFP_KERNEL);
	if (!drvdata->msynth)
		return -ENOMEM;

	drvdata->clkout = devm_kzalloc(&client->dev, 4 *
				       sizeof(*drvdata->clkout), GFP_KERNEL);
	if (!drvdata->clkout)
		return -ENOMEM;

	drvdata->onecell.clk_num = 4;
	drvdata->onecell.clks = devm_kzalloc(&client->dev,
		4 * sizeof(*drvdata->onecell.clks), GFP_KERNEL);

	if (!drvdata->onecell.clks)
		return -ENOMEM;

	for (n = 0; n < 4; n++) {
		drvdata->msynth[n].num = n;
		drvdata->msynth[n].drvdata = drvdata;
		strlcpy(name_buf[0], drvdata->name_prefix, MAX_NAME_PREFIX);
		strncat(name_buf[0], si5338_msynth_src_names[0],
			STRNCAT_LENGTH);
		strlcpy(register_name, drvdata->name_prefix, MAX_NAME_PREFIX);
		strncat(register_name, si5338_msynth_names[n], STRNCAT_LENGTH);
		flags = (!pdata->pll_vco && n == pdata->pll_master) ?
			CLK_SET_RATE_PARENT : 0;

		clk = si5338_register_clock(&client->dev,
					    &drvdata->msynth[n].hw,
					    register_name,
					    parent_names,
					    1,
					    &si5338_msynth_ops,
					    flags);
		if (IS_ERR(clk))
			return PTR_ERR(clk);

	}

	/*
	 * ms0 is available for all clkout
	 * ms0/ms1/ms2/ms3 is available for each clkout respectivelly
	 */
	for (n = 0; n < 8; n++) {
		strlcpy(name_buf[n], drvdata->name_prefix, MAX_NAME_PREFIX);
		strncat(name_buf[n], si5338_clkout_src_names[n],
			STRNCAT_LENGTH);
	}

	for (n = 0; n < 4; n++) {
		drvdata->clkout[n].num = n;
		drvdata->clkout[n].drvdata = drvdata;
		/*
		 * Update source
		 * ms0 for clkout0
		 * ms1 for clkout1
		 * ms2 for clkout2
		 * ms3 for clkout3
		 */
		strlcpy(name_buf[6], drvdata->name_prefix, MAX_NAME_PREFIX);
		strncat(name_buf[6], si5338_msynth_names[n], STRNCAT_LENGTH);
		/*
		 * Use clkout_name from device tree or platform data ignoring
		 * name_prefix. The clkout_name must be unique for each clock.
		 */
		if (pdata->clkout[n].name) {
			if (strlen(pdata->clkout[n].name) >= MAX_NAME_LENGTH) {
				dev_warn(&client->dev,
					 "clkout[%d] name %s too long\n",
					 n, pdata->clkout[n].name);
			}
			strlcpy(register_name, pdata->clkout[n].name,
				MAX_NAME_LENGTH);
		} else {
			strlcpy(register_name, drvdata->name_prefix,
				MAX_NAME_PREFIX);
			strncat(register_name, si5338_clkout_names[n],
				STRNCAT_LENGTH);
		}

		clk = si5338_register_clock(&client->dev,
					    &drvdata->clkout[n].hw,
					    register_name,
					    parent_names,
					    8,
					    &si5338_clkout_ops,
					    CLK_SET_RATE_PARENT);
		if (IS_ERR(clk))
			return PTR_ERR(clk);

		if (register_debugfs_status(&drvdata->clkout[n])) {
			dev_warn(&client->dev,
				 "Failed to register clkout status in debugfs\n");
		}

		drvdata->onecell.clks[n] = clk;

		/* set initial clkout rate */
		if (pdata->clkout[n].rate) {
			dev_dbg(&client->dev, "Setting clkout%d rate to %lu\n",
				n, pdata->clkout[n].rate);
			ret = clk_set_rate(clk, pdata->clkout[n].rate);
			if (ret) {
				dev_err(&client->dev,
					"Cannot set rate for clkout%d: %d\n",
					n, ret);
				return ret;
			}
			/* clocks need to be prepared before post init */
			ret = clk_prepare(clk);
			if (ret) {
				dev_err(&client->dev,
					"Cannot prepare clk%d\n", n);
				return ret;
			}
		}
	}

	/*
	 * Important: Go through the procedure to check PLL locking
	 * and other steps required by si5338 reference manual.
	 */
	ret = post_init(drvdata);
	if (ret)
		return ret;

	for (n = 0; n < 4; n++) {
		if (pdata->clkout[n].rate) {
			if (pdata->clkout[n].enabled) {
				ret = clk_enable(drvdata->onecell.clks[n]);
				if (ret)
					return ret;
			} else {
				clk_unprepare(drvdata->onecell.clks[n]);
			}
		}
	}

	dev_dbg(&client->dev, "%s clocks are registered\n", id->name);

#ifdef CONFIG_OF
	ret = of_clk_add_provider(client->dev.of_node,
				  of_clk_src_onecell_get,
				  &drvdata->onecell);
	if (ret) {
		dev_err(&client->dev, "unable to add clk provider\n");
		return ret;
	}
#endif
	for (n = 0; n < 4; n++) {
		clk = drvdata->clkout[n].hw.clk;
		drvdata->lookup[n] = clkdev_alloc(clk,
						  __clk_get_name(clk),
						  NULL);
		if (!drvdata->lookup[n]) {
			dev_warn(&client->dev,
				"Unable to add clkout%d to clkdev\n", n);
			continue;
		}
		if (strlen(drvdata->lookup[n]->con_id) !=
					strlen(__clk_get_name(clk))) {
			dev_warn(&client->dev,
				 "Warning: clkdev doesn't support name longer than %zu\n",
				 strlen(drvdata->lookup[n]->con_id));
		}
		clkdev_add(drvdata->lookup[n]);
	}

	return 0;
}

static int si5338_i2c_remove(struct i2c_client *client)
{
	struct si5338_driver_data *drvdata = i2c_get_clientdata(client);
	int n;

#ifdef CONFIG_OF
	of_clk_del_provider(client->dev.of_node);
#endif

	for (n = 0; n < 4; n++) {
		if (drvdata->lookup[n])
			clkdev_drop(drvdata->lookup[n]);
	}

	dev_dbg(&client->dev, "Removed\n");

	return 0;
}


static const struct i2c_device_id si5338_i2c_ids[] = {
	{ "si5338", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, si5338_i2c_ids);

static struct i2c_driver si5338_driver = {
	.driver = {
		.name = "si5338",
		.of_match_table = of_match_ptr(si5338_dt_ids),
	},
	.probe = si5338_i2c_probe,
	.remove = si5338_i2c_remove,
	.id_table = si5338_i2c_ids,
};
module_i2c_driver(si5338_driver);

MODULE_AUTHOR("York Sun <yorksun@freescale.com");
MODULE_DESCRIPTION("Silicon Labs Si5338 clock generator driver");
MODULE_LICENSE("GPL v2");
