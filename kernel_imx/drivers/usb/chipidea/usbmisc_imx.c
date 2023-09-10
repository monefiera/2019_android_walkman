/*
 * Copyright 2012-2016 Freescale Semiconductor, Inc.
 * Copyright 2017 NXP
 * Copyright 2019 Sony Video & Sound Products Inc.
 * Copyright 2019 Sony Home Entertainment & Sound Products Inc.
 *
 * The code contained herein is licensed under the GNU General Public
 * License. You may obtain a copy of the GNU General Public License
 * Version 2 or later at the following locations:
 *
 * http://www.opensource.org/licenses/gpl-license.html
 * http://www.gnu.org/copyleft/gpl.html
 */

#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/module.h>
#include <linux/device.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/err.h>
#include <linux/io.h>
#include <linux/delay.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>
#include <linux/atomic.h>
#include <linux/icx_dmp_board_id.h>

#include "ci_hdrc_imx.h"

#define MX25_USB_PHY_CTRL_OFFSET	0x08
#define MX25_BM_EXTERNAL_VBUS_DIVIDER	BIT(23)

#define MX25_EHCI_INTERFACE_SINGLE_UNI	(2 << 0)
#define MX25_EHCI_INTERFACE_DIFF_UNI	(0 << 0)
#define MX25_EHCI_INTERFACE_MASK	(0xf)

#define MX25_OTG_SIC_SHIFT		29
#define MX25_OTG_SIC_MASK		(0x3 << MX25_OTG_SIC_SHIFT)
#define MX25_OTG_PM_BIT			BIT(24)
#define MX25_OTG_PP_BIT			BIT(11)
#define MX25_OTG_OCPOL_BIT		BIT(3)

#define MX25_H1_SIC_SHIFT		21
#define MX25_H1_SIC_MASK		(0x3 << MX25_H1_SIC_SHIFT)
#define MX25_H1_PP_BIT			BIT(18)
#define MX25_H1_PM_BIT			BIT(16)
#define MX25_H1_IPPUE_UP_BIT		BIT(7)
#define MX25_H1_IPPUE_DOWN_BIT		BIT(6)
#define MX25_H1_TLL_BIT			BIT(5)
#define MX25_H1_USBTE_BIT		BIT(4)
#define MX25_H1_OCPOL_BIT		BIT(2)

#define MX27_H1_PM_BIT			BIT(8)
#define MX27_H2_PM_BIT			BIT(16)
#define MX27_OTG_PM_BIT			BIT(24)

#define MX53_USB_OTG_PHY_CTRL_0_OFFSET	0x08
#define MX53_USB_OTG_PHY_CTRL_1_OFFSET	0x0c
#define MX53_USB_CTRL_1_OFFSET	        0x10
#define MX53_USB_CTRL_1_H2_XCVR_CLK_SEL_MASK (0x11 << 2)
#define MX53_USB_CTRL_1_H2_XCVR_CLK_SEL_ULPI BIT(2)
#define MX53_USB_CTRL_1_H3_XCVR_CLK_SEL_MASK (0x11 << 6)
#define MX53_USB_CTRL_1_H3_XCVR_CLK_SEL_ULPI BIT(6)
#define MX53_USB_UH2_CTRL_OFFSET	0x14
#define MX53_USB_UH3_CTRL_OFFSET	0x18
#define MX53_USB_CLKONOFF_CTRL_OFFSET	0x24
#define MX53_USB_CLKONOFF_CTRL_H2_INT60CKOFF BIT(21)
#define MX53_USB_CLKONOFF_CTRL_H3_INT60CKOFF BIT(22)
#define MX53_BM_OVER_CUR_DIS_H1		BIT(5)
#define MX53_BM_OVER_CUR_DIS_OTG	BIT(8)
#define MX53_BM_OVER_CUR_DIS_UHx	BIT(30)
#define MX53_USB_CTRL_1_UH2_ULPI_EN	BIT(26)
#define MX53_USB_CTRL_1_UH3_ULPI_EN	BIT(27)
#define MX53_USB_UHx_CTRL_WAKE_UP_EN	BIT(7)
#define MX53_USB_UHx_CTRL_ULPI_INT_EN	BIT(8)
#define MX53_USB_PHYCTRL1_PLLDIV_MASK	0x3
#define MX53_USB_PLL_DIV_24_MHZ		0x01

#define MX6_BM_NON_BURST_SETTING	BIT(1)
#define MX6_BM_OVER_CUR_DIS		BIT(7)
#define MX6_BM_OVER_CUR_POLARITY	BIT(8)
#define MX6_BM_PRW_POLARITY		BIT(9)
#define MX6_BM_WAKEUP_ENABLE		BIT(10)
#define MX6_BM_UTMI_ON_CLOCK		BIT(13)
#define MX6_BM_ID_WAKEUP		BIT(16)
#define MX6_BM_VBUS_WAKEUP		BIT(17)
#define MX6SX_BM_DPDM_WAKEUP_EN		BIT(29)
#define MX6_BM_WAKEUP_INTR		BIT(31)

#define MX6_USB_HSIC_CTRL_OFFSET	0x10
/* Send resume signal without 480Mhz PHY clock */
#define MX6SX_BM_HSIC_AUTO_RESUME	BIT(23)
/* set before portsc.suspendM = 1 */
#define MX6_BM_HSIC_DEV_CONN		BIT(21)
/* HSIC enable */
#define MX6_BM_HSIC_EN			BIT(12)
/* Force HSIC module 480M clock on, even when in Host is in suspend mode */
#define MX6_BM_HSIC_CLK_ON		BIT(11)

#define MX6_USB_OTG1_PHY_CTRL		0x18
/* For imx6dql, it is host-only controller, for later imx6, it is otg's */
#define MX6_USB_OTG2_PHY_CTRL		0x1c
#define MX6SX_USB_VBUS_WAKEUP_SOURCE(v)	(v << 8)
#define MX6SX_USB_VBUS_WAKEUP_SOURCE_VBUS	MX6SX_USB_VBUS_WAKEUP_SOURCE(0)
#define MX6SX_USB_VBUS_WAKEUP_SOURCE_AVALID	MX6SX_USB_VBUS_WAKEUP_SOURCE(1)
#define MX6SX_USB_VBUS_WAKEUP_SOURCE_BVALID	MX6SX_USB_VBUS_WAKEUP_SOURCE(2)
#define MX6SX_USB_VBUS_WAKEUP_SOURCE_SESS_END	MX6SX_USB_VBUS_WAKEUP_SOURCE(3)

#define VF610_OVER_CUR_DIS		BIT(7)

#define MX7D_USBNC_USB_CTRL2		0x4
/* The default DM/DP value is pull-down */
#define MX7D_USBNC_USB_CTRL2_DM_OVERRIDE_EN		BIT(15)
#define MX7D_USBNC_USB_CTRL2_DM_OVERRIDE_VAL		BIT(14)
#define MX7D_USBNC_USB_CTRL2_DP_OVERRIDE_EN		BIT(13)
#define MX7D_USBNC_USB_CTRL2_DP_OVERRIDE_VAL		BIT(12)
#define MX7D_USBNC_USB_CTRL2_DP_DM_MASK			(BIT(12) | BIT(13) | \
							BIT(14) | BIT(15))
#define MX7D_USBNC_USB_CTRL2_XCVRSEL_OVERRIDE_EN	BIT(11)
#define MX7D_USBNC_USB_CTRL2_XCVRSEL_OVERRIDE_MASK	(BIT(10) | BIT(9))
#define MX7D_USBNC_USB_CTRL2_XCVRSEL_OVERRIDE_XX	(BIT(10) | BIT(9))
#define MX7D_USBNC_USB_CTRL2_XCVRSEL_OVERRIDE_FS		  (BIT(9))
#define MX7D_USBNC_USB_CTRL2_XCVRSEL_OVERRIDE_LS	(BIT(10))
#define MX7D_USBNC_USB_CTRL2_XCVRSEL_OVERRIDE_HS	(0)

#define MX7D_USBNC_USB_CTRL2_OPMODE_OVERRIDE_EN		BIT(8)
#define MX7D_USBNC_USB_CTRL2_OPMODE_OVERRIDE_MASK	(BIT(7) | BIT(6))
#define MX7D_USBNC_USB_CTRL2_OPMODE(v)			(v << 6)
#define MX7D_USBNC_USB_CTRL2_OPMODE_NORMAL		(0)
#define MX7D_USBNC_USB_CTRL2_OPMODE_NON_DRIVING	MX7D_USBNC_USB_CTRL2_OPMODE(1)
#define MX7D_USBNC_USB_CTRL2_OPMODE_DISABLE_BIT_STUFF	(BIT(7))
#define MX7D_USBNC_USB_CTRL2_OPMODE_WO_AUTO_GEN		((BIT(7) | BIT(6))

#define MX7D_USBNC_USB_CTRL2_TERMSEL_OVERRIDE_EN	BIT(5)
#define MX7D_USBNC_USB_CTRL2_TERMSEL_OVERRIDE_VAL	BIT(4)
#define MX7D_USBNC_AUTO_RESUME				BIT(2)

#define	MX7D_USBNC_USB_CTRL2_NO_PULL_DOWN (0)

#define	MX7D_USBNC_USB_CTRL2_NO_PULL_DOWN_OVERRIDE (\
	MX7D_USBNC_USB_CTRL2_DM_OVERRIDE_EN | \
	MX7D_USBNC_USB_CTRL2_DP_OVERRIDE_EN | \
	MX7D_USBNC_USB_CTRL2_NO_PULL_DOWN \
	)

#define	MX7D_USBNC_USB_CTRL2_PULL_DOWN_MASK (\
	MX7D_USBNC_USB_CTRL2_DM_OVERRIDE_EN  | \
	MX7D_USBNC_USB_CTRL2_DM_OVERRIDE_VAL | \
	MX7D_USBNC_USB_CTRL2_DP_OVERRIDE_EN  | \
	MX7D_USBNC_USB_CTRL2_DP_OVERRIDE_VAL )

#define MX7D_USBNC_USB_CTRL2_PERIPERAL_LS (\
	MX7D_USBNC_USB_CTRL2_XCVRSEL_OVERRIDE_LS | \
	MX7D_USBNC_USB_CTRL2_OPMODE_NORMAL       | \
	MX7D_USBNC_USB_CTRL2_TERMSEL_OVERRIDE_VAL)

#define MX7D_USBNC_USB_CTRL2_PERIPERAL_LS_OVERRIDE (\
	MX7D_USBNC_USB_CTRL2_XCVRSEL_OVERRIDE_EN | \
	MX7D_USBNC_USB_CTRL2_OPMODE_OVERRIDE_EN  | \
	MX7D_USBNC_USB_CTRL2_TERMSEL_OVERRIDE_EN | \
	MX7D_USBNC_USB_CTRL2_PERIPERAL_LS)

#define	MX7D_USBNC_USB_CTRL2_PERIPERAL_LS_MASK (\
	MX7D_USBNC_USB_CTRL2_XCVRSEL_OVERRIDE_EN   | \
	MX7D_USBNC_USB_CTRL2_XCVRSEL_OVERRIDE_MASK | \
	MX7D_USBNC_USB_CTRL2_OPMODE_OVERRIDE_EN    | \
	MX7D_USBNC_USB_CTRL2_OPMODE_OVERRIDE_MASK  | \
	MX7D_USBNC_USB_CTRL2_TERMSEL_OVERRIDE_EN   | \
	MX7D_USBNC_USB_CTRL2_TERMSEL_OVERRIDE_VAL  )

#define MX7D_USBNC_USB_CTRL2_PERIPERAL_FS (\
	MX7D_USBNC_USB_CTRL2_XCVRSEL_OVERRIDE_FS | \
	MX7D_USBNC_USB_CTRL2_OPMODE_NORMAL       | \
	MX7D_USBNC_USB_CTRL2_TERMSEL_OVERRIDE_VAL)

#define MX7D_USBNC_USB_CTRL2_PERIPERAL_FS_OVERRIDE (\
	MX7D_USBNC_USB_CTRL2_XCVRSEL_OVERRIDE_EN | \
	MX7D_USBNC_USB_CTRL2_OPMODE_OVERRIDE_EN  | \
	MX7D_USBNC_USB_CTRL2_TERMSEL_OVERRIDE_EN | \
	MX7D_USBNC_USB_CTRL2_PERIPERAL_FS)

#define	MX7D_USBNC_USB_CTRL2_PERIPERAL_FS_MASK (\
	MX7D_USBNC_USB_CTRL2_XCVRSEL_OVERRIDE_EN   | \
	MX7D_USBNC_USB_CTRL2_XCVRSEL_OVERRIDE_MASK | \
	MX7D_USBNC_USB_CTRL2_OPMODE_OVERRIDE_EN    | \
	MX7D_USBNC_USB_CTRL2_OPMODE_OVERRIDE_MASK  | \
	MX7D_USBNC_USB_CTRL2_TERMSEL_OVERRIDE_EN   | \
	MX7D_USBNC_USB_CTRL2_TERMSEL_OVERRIDE_VAL  )

#define MX7D_USBNC_USB_CTRL2_PERIPERAL_FS_NOPU   (\
	MX7D_USBNC_USB_CTRL2_XCVRSEL_OVERRIDE_FS | \
	MX7D_USBNC_USB_CTRL2_OPMODE_NORMAL       )

#define MX7D_USBNC_USB_CTRL2_PERIPERAL_FS_NOPU_OVERRIDE (\
	MX7D_USBNC_USB_CTRL2_XCVRSEL_OVERRIDE_EN | \
	MX7D_USBNC_USB_CTRL2_OPMODE_OVERRIDE_EN  | \
	MX7D_USBNC_USB_CTRL2_TERMSEL_OVERRIDE_EN | \
	MX7D_USBNC_USB_CTRL2_PERIPERAL_FS_NOPU)

#define	MX7D_USBNC_USB_CTRL2_PERIPERAL_FS_NOPU_MASK (\
	MX7D_USBNC_USB_CTRL2_XCVRSEL_OVERRIDE_EN   | \
	MX7D_USBNC_USB_CTRL2_XCVRSEL_OVERRIDE_MASK | \
	MX7D_USBNC_USB_CTRL2_OPMODE_OVERRIDE_EN    | \
	MX7D_USBNC_USB_CTRL2_OPMODE_OVERRIDE_MASK  | \
	MX7D_USBNC_USB_CTRL2_TERMSEL_OVERRIDE_EN   | \
	MX7D_USBNC_USB_CTRL2_TERMSEL_OVERRIDE_VAL  )



#define MX7D_USB_VBUS_WAKEUP_SOURCE_MASK	0x3
#define MX7D_USB_VBUS_WAKEUP_SOURCE(v)		(v << 0)
#define MX7D_USB_VBUS_WAKEUP_SOURCE_VBUS	MX7D_USB_VBUS_WAKEUP_SOURCE(0)
#define MX7D_USB_VBUS_WAKEUP_SOURCE_AVALID	MX7D_USB_VBUS_WAKEUP_SOURCE(1)
#define MX7D_USB_VBUS_WAKEUP_SOURCE_BVALID	MX7D_USB_VBUS_WAKEUP_SOURCE(2)
#define MX7D_USB_VBUS_WAKEUP_SOURCE_SESS_END	MX7D_USB_VBUS_WAKEUP_SOURCE(3)
#define MX7D_USB_TERMSEL_OVERRIDE	BIT(4)
#define MX7D_USB_TERMSEL_OVERRIDE_EN	BIT(5)

#define MX7D_USB_OTG_PHY_CFG1		0x30
#define TXPREEMPAMPTUNE0_BIT		28
#define TXPREEMPAMPTUNE0_MASK		(3 << 28)
#define TXVREFTUNE0_BIT			20
#define TXVREFTUNE0_MASK		(0xf << 20)

#define MX7D_USB_OTG_PHY_CFG2_CHRG_DCDENB	BIT(3)
#define MX7D_USB_OTG_PHY_CFG2_CHRG_VDATSRCENB0	BIT(2)
#define MX7D_USB_OTG_PHY_CFG2_CHRG_VDATDETENB0	BIT(1)
#define MX7D_USB_OTG_PHY_CFG2_CHRG_CHRGSEL	BIT(0)
#define MX7D_USB_OTG_PHY_CFG2		0x34

#define MX7D_USB_OTG_PHY_STATUS		0x3c
#define MX7D_USB_OTG_PHY_STATUS_CHRGDET		BIT(29)
#define MX7D_USB_OTG_PHY_STATUS_VBUS_VLD	BIT(3)
#define MX7D_USB_OTG_PHY_STATUS_LINE_STATE1	BIT(1)
#define MX7D_USB_OTG_PHY_STATUS_LINE_STATE0	BIT(0)

#define MX7D_USB_OTG_PHY_STATUS_LINE_STATE_MASK		(BIT(1) | BIT(0))
#define MX7D_USB_OTG_PHY_STATUS_LINE_STATE_A0R5A	(BIT(1) | BIT(0))
#define MX7D_USB_OTG_PHY_STATUS_LINE_STATE_A1R0A	(BIT(1))
#define MX7D_USB_OTG_PHY_STATUS_LINE_STATE_A2R1A		 (BIT(0))
#define MX7D_USB_OTG_PHY_STATUS_LINE_STATE_SDP		(0)

#define ANADIG_ANA_MISC0		0x150
#define ANADIG_ANA_MISC0_SET		0x154
#define ANADIG_ANA_MISC0_CLK_DELAY(x)	((x >> 26) & 0x7)

#define ANADIG_USB1_CHRG_DETECT_SET	0x1b4
#define ANADIG_USB1_CHRG_DETECT_CLR	0x1b8
#define ANADIG_USB1_CHRG_DETECT_EN_B		BIT(20)
#define ANADIG_USB1_CHRG_DETECT_CHK_CHRG_B	BIT(19)
#define ANADIG_USB1_CHRG_DETECT_CHK_CONTACT	BIT(18)

#define ANADIG_USB1_VBUS_DET_STAT	0x1c0
#define ANADIG_USB1_VBUS_DET_STAT_VBUS_VALID	BIT(3)

#define ANADIG_USB1_CHRG_DET_STAT	0x1d0
#define ANADIG_USB1_CHRG_DET_STAT_DM_STATE	BIT(2)
#define ANADIG_USB1_CHRG_DET_STAT_CHRG_DETECTED	BIT(1)
#define ANADIG_USB1_CHRG_DET_STAT_PLUG_CONTACT	BIT(0)

struct usbmisc_ops {
	/* It's called once when probe a usb device */
	int (*init)(struct imx_usbmisc_data *data);
	/* It's called once after adding a usb device */
	int (*post)(struct imx_usbmisc_data *data);
	/* It's called when we need to enable/disable usb wakeup */
	int (*set_wakeup)(struct imx_usbmisc_data *data, bool enabled);
	/* usb charger detection */
	int (*charger_detection)(struct imx_usbmisc_data *data);
	/* It's called when system resume from usb power lost */
	int (*power_lost_check)(struct imx_usbmisc_data *data);
	/* It's called before setting portsc.suspendM */
	int (*hsic_set_connect)(struct imx_usbmisc_data *data);
	/* It's called during suspend/resume */
	int (*hsic_set_clk)(struct imx_usbmisc_data *data, bool enabled);
	int (*hsic_set_suspend)(struct imx_usbmisc_data *data);
	int (*hsic_set_resume)(struct imx_usbmisc_data *data);
	/* override UTMI termination select */
	int (*term_select_override)(struct imx_usbmisc_data *data,
						bool enable, int val);
};

struct imx_usbmisc {
	struct device	*dev;
	void __iomem *base;
	spinlock_t lock;
	const struct usbmisc_ops *ops;
	struct mutex mutex;
	int		vdm_src_poll_ms;
};

static struct regulator *vbus_wakeup_reg;

static inline bool is_imx53_usbmisc(struct imx_usbmisc_data *data);

static int usbmisc_imx25_init(struct imx_usbmisc_data *data)
{
	struct imx_usbmisc *usbmisc = dev_get_drvdata(data->dev);
	unsigned long flags;
	u32 val = 0;

	if (data->index > 1)
		return -EINVAL;

	spin_lock_irqsave(&usbmisc->lock, flags);
	switch (data->index) {
	case 0:
		val = readl(usbmisc->base);
		val &= ~(MX25_OTG_SIC_MASK | MX25_OTG_PP_BIT);
		val |= (MX25_EHCI_INTERFACE_DIFF_UNI & MX25_EHCI_INTERFACE_MASK) << MX25_OTG_SIC_SHIFT;
		val |= (MX25_OTG_PM_BIT | MX25_OTG_OCPOL_BIT);
		writel(val, usbmisc->base);
		break;
	case 1:
		val = readl(usbmisc->base);
		val &= ~(MX25_H1_SIC_MASK | MX25_H1_PP_BIT |  MX25_H1_IPPUE_UP_BIT);
		val |= (MX25_EHCI_INTERFACE_SINGLE_UNI & MX25_EHCI_INTERFACE_MASK) << MX25_H1_SIC_SHIFT;
		val |= (MX25_H1_PM_BIT | MX25_H1_OCPOL_BIT | MX25_H1_TLL_BIT |
			MX25_H1_USBTE_BIT | MX25_H1_IPPUE_DOWN_BIT);

		writel(val, usbmisc->base);

		break;
	}
	spin_unlock_irqrestore(&usbmisc->lock, flags);

	return 0;
}

static int usbmisc_imx25_post(struct imx_usbmisc_data *data)
{
	struct imx_usbmisc *usbmisc = dev_get_drvdata(data->dev);
	void __iomem *reg;
	unsigned long flags;
	u32 val;

	if (data->index > 2)
		return -EINVAL;

	if (data->evdo) {
		spin_lock_irqsave(&usbmisc->lock, flags);
		reg = usbmisc->base + MX25_USB_PHY_CTRL_OFFSET;
		val = readl(reg);
		writel(val | MX25_BM_EXTERNAL_VBUS_DIVIDER, reg);
		spin_unlock_irqrestore(&usbmisc->lock, flags);
		usleep_range(5000, 10000); /* needed to stabilize voltage */
	}

	return 0;
}

static int usbmisc_imx27_init(struct imx_usbmisc_data *data)
{
	struct imx_usbmisc *usbmisc = dev_get_drvdata(data->dev);
	unsigned long flags;
	u32 val;

	switch (data->index) {
	case 0:
		val = MX27_OTG_PM_BIT;
		break;
	case 1:
		val = MX27_H1_PM_BIT;
		break;
	case 2:
		val = MX27_H2_PM_BIT;
		break;
	default:
		return -EINVAL;
	}

	spin_lock_irqsave(&usbmisc->lock, flags);
	if (data->disable_oc)
		val = readl(usbmisc->base) | val;
	else
		val = readl(usbmisc->base) & ~val;
	writel(val, usbmisc->base);
	spin_unlock_irqrestore(&usbmisc->lock, flags);

	return 0;
}

static int usbmisc_imx53_init(struct imx_usbmisc_data *data)
{
	struct imx_usbmisc *usbmisc = dev_get_drvdata(data->dev);
	void __iomem *reg = NULL;
	unsigned long flags;
	u32 val = 0;

	if (data->index > 3)
		return -EINVAL;

	/* Select a 24 MHz reference clock for the PHY  */
	val = readl(usbmisc->base + MX53_USB_OTG_PHY_CTRL_1_OFFSET);
	val &= ~MX53_USB_PHYCTRL1_PLLDIV_MASK;
	val |= MX53_USB_PLL_DIV_24_MHZ;
	writel(val, usbmisc->base + MX53_USB_OTG_PHY_CTRL_1_OFFSET);

	spin_lock_irqsave(&usbmisc->lock, flags);

	switch (data->index) {
	case 0:
		if (data->disable_oc) {
			reg = usbmisc->base + MX53_USB_OTG_PHY_CTRL_0_OFFSET;
			val = readl(reg) | MX53_BM_OVER_CUR_DIS_OTG;
			writel(val, reg);
		}
		break;
	case 1:
		if (data->disable_oc) {
			reg = usbmisc->base + MX53_USB_OTG_PHY_CTRL_0_OFFSET;
			val = readl(reg) | MX53_BM_OVER_CUR_DIS_H1;
			writel(val, reg);
		}
		break;
	case 2:
		if (data->ulpi) {
			/* set USBH2 into ULPI-mode. */
			reg = usbmisc->base + MX53_USB_CTRL_1_OFFSET;
			val = readl(reg) | MX53_USB_CTRL_1_UH2_ULPI_EN;
			/* select ULPI clock */
			val &= ~MX53_USB_CTRL_1_H2_XCVR_CLK_SEL_MASK;
			val |= MX53_USB_CTRL_1_H2_XCVR_CLK_SEL_ULPI;
			writel(val, reg);
			/* Set interrupt wake up enable */
			reg = usbmisc->base + MX53_USB_UH2_CTRL_OFFSET;
			val = readl(reg) | MX53_USB_UHx_CTRL_WAKE_UP_EN
				| MX53_USB_UHx_CTRL_ULPI_INT_EN;
			writel(val, reg);
			if (is_imx53_usbmisc(data)) {
				/* Disable internal 60Mhz clock */
				reg = usbmisc->base +
					MX53_USB_CLKONOFF_CTRL_OFFSET;
				val = readl(reg) |
					MX53_USB_CLKONOFF_CTRL_H2_INT60CKOFF;
				writel(val, reg);
			}

		}
		if (data->disable_oc) {
			reg = usbmisc->base + MX53_USB_UH2_CTRL_OFFSET;
			val = readl(reg) | MX53_BM_OVER_CUR_DIS_UHx;
			writel(val, reg);
		}
		break;
	case 3:
		if (data->ulpi) {
			/* set USBH3 into ULPI-mode. */
			reg = usbmisc->base + MX53_USB_CTRL_1_OFFSET;
			val = readl(reg) | MX53_USB_CTRL_1_UH3_ULPI_EN;
			/* select ULPI clock */
			val &= ~MX53_USB_CTRL_1_H3_XCVR_CLK_SEL_MASK;
			val |= MX53_USB_CTRL_1_H3_XCVR_CLK_SEL_ULPI;
			writel(val, reg);
			/* Set interrupt wake up enable */
			reg = usbmisc->base + MX53_USB_UH3_CTRL_OFFSET;
			val = readl(reg) | MX53_USB_UHx_CTRL_WAKE_UP_EN
				| MX53_USB_UHx_CTRL_ULPI_INT_EN;
			writel(val, reg);

			if (is_imx53_usbmisc(data)) {
				/* Disable internal 60Mhz clock */
				reg = usbmisc->base +
					MX53_USB_CLKONOFF_CTRL_OFFSET;
				val = readl(reg) |
					MX53_USB_CLKONOFF_CTRL_H3_INT60CKOFF;
				writel(val, reg);
			}
		}
		if (data->disable_oc) {
			reg = usbmisc->base + MX53_USB_UH3_CTRL_OFFSET;
			val = readl(reg) | MX53_BM_OVER_CUR_DIS_UHx;
			writel(val, reg);
		}
		break;
	}

	spin_unlock_irqrestore(&usbmisc->lock, flags);

	return 0;
}

static int usbmisc_imx6_hsic_set_connect(struct imx_usbmisc_data *data)
{
	unsigned long flags;
	u32 val, offset;
	struct imx_usbmisc *usbmisc = dev_get_drvdata(data->dev);
	int ret = 0;

	spin_lock_irqsave(&usbmisc->lock, flags);
	if (data->index == 2 || data->index == 3) {
		offset = (data->index - 2) * 4;
	} else if (data->index == 0) {
		/*
		 * For controllers later than imx7d (imx7d is included),
		 * each controller has its own non core register region.
		 * And the controllers before than imx7d, the 1st controller
		 * is not HSIC controller.
		 */
		offset = 0;
	} else {
		dev_err(data->dev, "index is error for usbmisc\n");
		offset = 0;
		ret = -EINVAL;
	}

	val = readl(usbmisc->base + MX6_USB_HSIC_CTRL_OFFSET + offset);
	if (!(val & MX6_BM_HSIC_DEV_CONN))
		writel(val | MX6_BM_HSIC_DEV_CONN,
			usbmisc->base + MX6_USB_HSIC_CTRL_OFFSET + offset);
	spin_unlock_irqrestore(&usbmisc->lock, flags);

	return ret;
}

static int usbmisc_imx6_hsic_set_clk(struct imx_usbmisc_data *data, bool on)
{
	unsigned long flags;
	u32 val, offset;
	struct imx_usbmisc *usbmisc = dev_get_drvdata(data->dev);
	int ret = 0;

	spin_lock_irqsave(&usbmisc->lock, flags);
	if (data->index == 2 || data->index == 3) {
		offset = (data->index - 2) * 4;
	} else if (data->index == 0) {
		offset = 0;
	} else {
		dev_err(data->dev, "index is error for usbmisc\n");
		offset = 0;
		ret = -EINVAL;
	}

	val = readl(usbmisc->base + MX6_USB_HSIC_CTRL_OFFSET + offset);
	val |= MX6_BM_HSIC_EN | MX6_BM_HSIC_CLK_ON;
	if (on)
		val |= MX6_BM_HSIC_CLK_ON;
	else
		val &= ~MX6_BM_HSIC_CLK_ON;
	writel(val, usbmisc->base + MX6_USB_HSIC_CTRL_OFFSET + offset);
	spin_unlock_irqrestore(&usbmisc->lock, flags);

	return 0;
}

static int usbmisc_imx7_hsic_set_suspend(struct imx_usbmisc_data *data)
{
	unsigned long flags;
	struct imx_usbmisc *usbmisc = dev_get_drvdata(data->dev);
	u32 val;

	spin_lock_irqsave(&usbmisc->lock, flags);

        /* Disable VBUS valid comparator when in suspend mode. clear USB_OTGx_PHY_CTL2 bit 16
         * When OTG is disabled and DRVVBUS0 is asserted case
         * the Bandgap circuitry and VBUS Valid comparator are still powered, even in Suspend or Sleep mode.
         * So we need clear DRVVBUS0 under suspend mode here !
         */

        /* OTG None Core PHY CTRL 2 */
	val = readl(usbmisc->base + 0x34);
	val &= ~BIT(16);
	writel(val, usbmisc->base + 0x34);

	spin_unlock_irqrestore(&usbmisc->lock, flags);

	return 0;
}

static int usbmisc_imx7_hsic_set_resume(struct imx_usbmisc_data *data)
{
	unsigned long flags;
	struct imx_usbmisc *usbmisc = dev_get_drvdata(data->dev);
	u32 val;

	spin_lock_irqsave(&usbmisc->lock, flags);

        /* OTG None Core PHY CTRL 2 */
	val = readl(usbmisc->base + 0x34);
	val |= BIT(16);
	writel(val, usbmisc->base + 0x34);


	spin_unlock_irqrestore(&usbmisc->lock, flags);

	return 0;
}

static u32 imx6q_finalize_wakeup_setting(struct imx_usbmisc_data *data)
{
	if (data->available_role == USB_DR_MODE_PERIPHERAL)
		return MX6_BM_VBUS_WAKEUP;
	else if (data->available_role == USB_DR_MODE_OTG)
		return MX6_BM_VBUS_WAKEUP | MX6_BM_ID_WAKEUP;

	return 0;
}

static int usbmisc_imx6q_set_wakeup
	(struct imx_usbmisc_data *data, bool enabled)
{
	struct imx_usbmisc *usbmisc = dev_get_drvdata(data->dev);
	unsigned long flags;
	int ret = 0;
	u32 val, wakeup_setting = MX6_BM_WAKEUP_ENABLE;

	if (data->index > 3)
		return -EINVAL;

	spin_lock_irqsave(&usbmisc->lock, flags);
	val = readl(usbmisc->base + data->index * 4);
	if (enabled) {
		wakeup_setting |= imx6q_finalize_wakeup_setting(data);
		writel(val | wakeup_setting, usbmisc->base + data->index * 4);
		spin_unlock_irqrestore(&usbmisc->lock, flags);
		if (vbus_wakeup_reg)
			ret = regulator_enable(vbus_wakeup_reg);
	} else {
		if (val & MX6_BM_WAKEUP_INTR)
			pr_debug("wakeup int at ci_hdrc.%d\n", data->index);
		wakeup_setting |= MX6_BM_VBUS_WAKEUP | MX6_BM_ID_WAKEUP;
		writel(val & ~wakeup_setting, usbmisc->base + data->index * 4);
		spin_unlock_irqrestore(&usbmisc->lock, flags);
		if (vbus_wakeup_reg && regulator_is_enabled(vbus_wakeup_reg))
			regulator_disable(vbus_wakeup_reg);
	}

	return ret;
}

static int usbmisc_imx6q_init(struct imx_usbmisc_data *data)
{
	struct imx_usbmisc *usbmisc = dev_get_drvdata(data->dev);
	unsigned long flags;
	u32 reg, val;

	if (data->index > 3)
		return -EINVAL;

	spin_lock_irqsave(&usbmisc->lock, flags);

	reg = readl(usbmisc->base + data->index * 4);
	if (data->disable_oc) {
		reg |= MX6_BM_OVER_CUR_DIS;
	} else if (data->oc_polarity == 1) {
		/* High active */
		reg &= ~(MX6_BM_OVER_CUR_DIS | MX6_BM_OVER_CUR_POLARITY);
	}
	writel(reg, usbmisc->base + data->index * 4);

	/* SoC non-burst setting */
	reg = readl(usbmisc->base + data->index * 4);
	writel(reg | MX6_BM_NON_BURST_SETTING,
			usbmisc->base + data->index * 4);

	/* For HSIC controller */
	if (data->index == 2 || data->index == 3) {
		val = readl(usbmisc->base + data->index * 4);
		writel(val | MX6_BM_UTMI_ON_CLOCK,
			usbmisc->base + data->index * 4);
		val = readl(usbmisc->base + MX6_USB_HSIC_CTRL_OFFSET
			+ (data->index - 2) * 4);
		val |= MX6_BM_HSIC_EN | MX6_BM_HSIC_CLK_ON;
		writel(val, usbmisc->base + MX6_USB_HSIC_CTRL_OFFSET
			+ (data->index - 2) * 4);

		/*
		 * Need to add delay to wait 24M OSC to be stable,
		 * It is board specific.
		 */
		regmap_read(data->anatop, ANADIG_ANA_MISC0, &val);
		/* 0 <= data->osc_clkgate_delay <= 7 */
		if (data->osc_clkgate_delay > ANADIG_ANA_MISC0_CLK_DELAY(val))
			regmap_write(data->anatop, ANADIG_ANA_MISC0_SET,
				(data->osc_clkgate_delay) << 26);
	}
	spin_unlock_irqrestore(&usbmisc->lock, flags);

	usbmisc_imx6q_set_wakeup(data, false);

	return 0;
}

static int usbmisc_imx6sx_init(struct imx_usbmisc_data *data)
{
	void __iomem *reg = NULL;
	unsigned long flags;
	struct imx_usbmisc *usbmisc = dev_get_drvdata(data->dev);
	u32 val;

	usbmisc_imx6q_init(data);

	spin_lock_irqsave(&usbmisc->lock, flags);
	if (data->index == 0 || data->index == 1) {
		reg = usbmisc->base + MX6_USB_OTG1_PHY_CTRL + data->index * 4;
		/* Set vbus wakeup source as bvalid */
		val = readl(reg);
		writel(val | MX6SX_USB_VBUS_WAKEUP_SOURCE_BVALID, reg);
		/*
		 * Disable dp/dm wakeup in device mode when vbus is
		 * not there.
		 */
		val = readl(usbmisc->base + data->index * 4);
		writel(val & ~MX6SX_BM_DPDM_WAKEUP_EN,
			usbmisc->base + data->index * 4);
	}

	/* For HSIC controller */
	if (data->index == 2) {
		val = readl(usbmisc->base + MX6_USB_HSIC_CTRL_OFFSET
						+ (data->index - 2) * 4);
		val |= MX6SX_BM_HSIC_AUTO_RESUME;
		writel(val, usbmisc->base + MX6_USB_HSIC_CTRL_OFFSET
						+ (data->index - 2) * 4);
	}
	spin_unlock_irqrestore(&usbmisc->lock, flags);

	return 0;
}

static int usbmisc_vf610_init(struct imx_usbmisc_data *data)
{
	struct imx_usbmisc *usbmisc = dev_get_drvdata(data->dev);
	u32 reg;

	/*
	 * Vybrid only has one misc register set, but in two different
	 * areas. These is reflected in two instances of this driver.
	 */
	if (data->index >= 1)
		return -EINVAL;

	if (data->disable_oc) {
		reg = readl(usbmisc->base);
		writel(reg | VF610_OVER_CUR_DIS, usbmisc->base);
	}

	return 0;
}

static int usbmisc_imx7d_set_wakeup
	(struct imx_usbmisc_data *data, bool enabled)
{
	struct imx_usbmisc *usbmisc = dev_get_drvdata(data->dev);
	unsigned long flags;
	u32 val;
	u32 wakeup_setting = MX6_BM_WAKEUP_ENABLE;

	spin_lock_irqsave(&usbmisc->lock, flags);
	val = readl(usbmisc->base);
	if (enabled) {
		//wakeup_setting |= imx6q_finalize_wakeup_setting(data);
		writel(val | wakeup_setting, usbmisc->base);
	} else {
		if (val & MX6_BM_WAKEUP_INTR)
			dev_dbg(data->dev, "wakeup int\n");
		wakeup_setting |= MX6_BM_VBUS_WAKEUP | MX6_BM_ID_WAKEUP;
		writel(val & ~wakeup_setting, usbmisc->base);
	}
	spin_unlock_irqrestore(&usbmisc->lock, flags);

	return 0;
}

static int usbmisc_imx7d_init(struct imx_usbmisc_data *data)
{
	struct imx_usbmisc *usbmisc = dev_get_drvdata(data->dev);
	unsigned long flags;
	u32 reg;

	if (data->index >= 1)
		return -EINVAL;

	spin_lock_irqsave(&usbmisc->lock, flags);
	reg = readl(usbmisc->base);
	if (data->disable_oc) {
		reg |= MX6_BM_OVER_CUR_DIS;
	} else if (data->oc_polarity == 1) {
		/* High active */
		reg &= ~(MX6_BM_OVER_CUR_DIS | MX6_BM_OVER_CUR_POLARITY);
	}

	if (data->pwr_polarity)
		reg |= MX6_BM_PRW_POLARITY;

	writel(reg, usbmisc->base);

	/* SoC non-burst setting */
	reg = readl(usbmisc->base);
	writel(reg | MX6_BM_NON_BURST_SETTING, usbmisc->base);

	if (!data->hsic) {
		reg = readl(usbmisc->base + MX7D_USBNC_USB_CTRL2);
		reg &= ~MX7D_USB_VBUS_WAKEUP_SOURCE_MASK;
		writel(reg | MX7D_USB_VBUS_WAKEUP_SOURCE_BVALID
			| MX7D_USBNC_AUTO_RESUME,
			usbmisc->base + MX7D_USBNC_USB_CTRL2);
		/* PHY tuning for signal quality */
		reg = readl(usbmisc->base + MX7D_USB_OTG_PHY_CFG1);
		if (data->emp_curr_control && data->emp_curr_control <=
			(TXPREEMPAMPTUNE0_MASK >> TXPREEMPAMPTUNE0_BIT)) {
			reg &= ~TXPREEMPAMPTUNE0_MASK;
			reg |= (data->emp_curr_control << TXPREEMPAMPTUNE0_BIT);
		}

		if (data->dc_vol_level_adjust && data->dc_vol_level_adjust <=
			(TXVREFTUNE0_MASK >> TXVREFTUNE0_BIT)) {
			reg &= ~TXVREFTUNE0_MASK;
			reg |= (data->dc_vol_level_adjust << TXVREFTUNE0_BIT);
		}

		writel(reg, usbmisc->base + MX7D_USB_OTG_PHY_CFG1);
	}

	spin_unlock_irqrestore(&usbmisc->lock, flags);

	usbmisc_imx7d_set_wakeup(data, false);

	return 0;
}

static int usbmisc_imx7d_power_lost_check(struct imx_usbmisc_data *data)
{
	struct imx_usbmisc *usbmisc = dev_get_drvdata(data->dev);
	unsigned long flags;
	u32 val;

	spin_lock_irqsave(&usbmisc->lock, flags);
	val = readl(usbmisc->base);
	spin_unlock_irqrestore(&usbmisc->lock, flags);
	/*
	 * Here use a power on reset value to judge
	 * if the controller experienced a power lost
	 */
	if (val == 0x30001000)
		return 1;
	else
		return 0;
}


static int usbmisc_imx6sx_power_lost_check(struct imx_usbmisc_data *data)
{
	struct imx_usbmisc *usbmisc = dev_get_drvdata(data->dev);
	unsigned long flags;
	u32 val;

	spin_lock_irqsave(&usbmisc->lock, flags);
	val = readl(usbmisc->base + data->index * 4);
	spin_unlock_irqrestore(&usbmisc->lock, flags);
	/*
	 * Here use a power on reset value to judge
	 * if the controller experienced a power lost
	 */
	if (val == 0x30001000)
		return 1;
	else
		return 0;
}

#define CHARGER_DCP_CDP_DET_2ND_WAIT_MS	(40)

static int imx7d_charger_secondary_detection(struct imx_usbmisc_data *data)
{
	struct imx_usbmisc *usbmisc = dev_get_drvdata(data->dev);
	struct usb_phy *usb_phy = data->usb_phy;
	unsigned long flags;
	int val, bak_val;

	/* Pull up DP */
	spin_lock_irqsave(&usbmisc->lock, flags);
	val = readl(usbmisc->base + MX7D_USBNC_USB_CTRL2);
	bak_val = val;
	val &= ~MX7D_USBNC_USB_CTRL2_DP_DM_MASK;
	val |= MX7D_USBNC_USB_CTRL2_DM_OVERRIDE_EN |
		MX7D_USBNC_USB_CTRL2_DP_OVERRIDE_EN;
	val |= MX7D_USBNC_USB_CTRL2_TERMSEL_OVERRIDE_EN |
		MX7D_USBNC_USB_CTRL2_TERMSEL_OVERRIDE_VAL;
	writel(val, usbmisc->base + MX7D_USBNC_USB_CTRL2);
	spin_unlock_irqrestore(&usbmisc->lock, flags);

	msleep(CHARGER_DCP_CDP_DET_2ND_WAIT_MS);

	spin_lock_irqsave(&usbmisc->lock, flags);
	val = readl(usbmisc->base + MX7D_USB_OTG_PHY_STATUS);
	if (val & MX7D_USB_OTG_PHY_STATUS_LINE_STATE1) {
		dev_dbg(data->dev, "It is a dedicate charging port\n");
		usb_phy->chg_type = DCP_TYPE;
	} else {
		dev_dbg(data->dev, "It is a charging downstream port\n");
		usb_phy->chg_type = CDP_TYPE;
	}
	writel(bak_val, usbmisc->base + MX7D_USBNC_USB_CTRL2);
	spin_unlock_irqrestore(&usbmisc->lock, flags);

	return 0;
}

static void imx7_disable_charger_detector(struct imx_usbmisc_data *data)
{
	struct imx_usbmisc *usbmisc = dev_get_drvdata(data->dev);
	unsigned long flags;
	u32 val;

	spin_lock_irqsave(&usbmisc->lock, flags);
	val = readl(usbmisc->base + MX7D_USB_OTG_PHY_CFG2);
	val &= ~(MX7D_USB_OTG_PHY_CFG2_CHRG_DCDENB |
			MX7D_USB_OTG_PHY_CFG2_CHRG_VDATSRCENB0 |
			MX7D_USB_OTG_PHY_CFG2_CHRG_VDATDETENB0 |
			MX7D_USB_OTG_PHY_CFG2_CHRG_CHRGSEL);
	writel(val, usbmisc->base + MX7D_USB_OTG_PHY_CFG2);

	/* Set OPMODE to be 2'b00 and disable its override */
	val = readl(usbmisc->base + MX7D_USBNC_USB_CTRL2);
	val &= ~MX7D_USBNC_USB_CTRL2_OPMODE_OVERRIDE_MASK;
	writel(val, usbmisc->base + MX7D_USBNC_USB_CTRL2);

	val = readl(usbmisc->base + MX7D_USBNC_USB_CTRL2);
	writel(val & ~MX7D_USBNC_USB_CTRL2_OPMODE_OVERRIDE_EN,
			usbmisc->base + MX7D_USBNC_USB_CTRL2);
	spin_unlock_irqrestore(&usbmisc->lock, flags);
}

#define	ICX_PLATFORM_WAIT_LS_STABLE_MS	(2)
#define	ICX_PLATFORM_WAIT_LS_POLL_INTERVAL_US	(1000)
#define	ICX_PLATFORM_WAIT_LS_POLL_LONG_DET	(5)
#define	ICX_PLATFORM_WAIT_LS_POLL_LONG		(40)
#define	ICX_PLATFORM_WAIT_LS_POLL_SHORT_DET	(2)
#define	ICX_PLATFORM_WAIT_LS_POLL_SHORT		(8)

static int imx7d_detect_some_kind_of_a(struct imx_usbmisc_data *data,
	int poll_max, int poll_detect)
{	struct imx_usbmisc *usbmisc = dev_get_drvdata(data->dev);
	struct usb_phy *usb_phy = data->usb_phy;
	u32	ctrl2_save;
	u32	ctrl2;
	u32	val;
	u32	line_state = 0;
	u32	line_state_prev = ~(u32)0;
	int	poll_total = 0;
	int	poll_stables = 0;
	int	result = -ENXIO;
	unsigned long	flags;

	spin_lock_irqsave(&usbmisc->lock, flags);
	ctrl2_save = readl(usbmisc->base + MX7D_USBNC_USB_CTRL2);
	ctrl2 = ctrl2_save;
	ctrl2 &= ~MX7D_USBNC_USB_CTRL2_PULL_DOWN_MASK;
	ctrl2 |=  MX7D_USBNC_USB_CTRL2_NO_PULL_DOWN_OVERRIDE;
	ctrl2 &= ~MX7D_USBNC_USB_CTRL2_PERIPERAL_LS_MASK;
	ctrl2 |=  MX7D_USBNC_USB_CTRL2_PERIPERAL_LS_OVERRIDE;
	writel(ctrl2, usbmisc->base + MX7D_USBNC_USB_CTRL2);
	spin_unlock_irqrestore(&usbmisc->lock, flags);

	msleep(ICX_PLATFORM_WAIT_LS_STABLE_MS);
	while (poll_total < poll_max) {
		val = readl(usbmisc->base + MX7D_USB_OTG_PHY_STATUS);
		line_state = val & MX7D_USB_OTG_PHY_STATUS_LINE_STATE_MASK;
		if (line_state != line_state_prev) {
			/* "contact Bouncing" or "1st contact" */
			poll_stables = 0;
		} else {
			/* "contact stable" */
			poll_stables++;
			if (poll_stables >= poll_detect) {
				/* "contact stable enough" */
				break;
			}
		}
		line_state_prev = line_state;
		usleep_range(
			ICX_PLATFORM_WAIT_LS_POLL_INTERVAL_US,
			ICX_PLATFORM_WAIT_LS_POLL_INTERVAL_US + 1000
			);
		poll_total++;
	}
	if (poll_stables >= poll_detect) {
		/* "Contact stable enough" */
		switch (line_state) {
		case MX7D_USB_OTG_PHY_STATUS_LINE_STATE_A1R0A:
		case MX7D_USB_OTG_PHY_STATUS_LINE_STATE_A2R1A:
		case MX7D_USB_OTG_PHY_STATUS_LINE_STATE_A0R5A:
			/* We may fail detect A-0.5A USB AC charger. */
			dev_info(data->dev, "Detected A-type charger. line_state=0x%.x\n",
				line_state
			);
			usb_phy->chg_type = ACA_TYPE;
			result = 0;
			break;
		default:
			/* SDP */
			break;
		}
	}
	spin_lock_irqsave(&usbmisc->lock, flags);
	writel(ctrl2_save, usbmisc->base + MX7D_USBNC_USB_CTRL2);
	spin_unlock_irqrestore(&usbmisc->lock, flags);
	msleep(ICX_PLATFORM_WAIT_LS_STABLE_MS);
	return result;
}


static int imx7d_charger_data_contact_detect(struct imx_usbmisc_data *data)
{
	struct imx_usbmisc *usbmisc = dev_get_drvdata(data->dev);
	unsigned long flags;
	u32 val;
	int i, data_pin_contact_count = 0;

	spin_lock_irqsave(&usbmisc->lock, flags);

	/* check if vbus is valid */
	val = readl(usbmisc->base + MX7D_USB_OTG_PHY_STATUS);
	if (!(val & MX7D_USB_OTG_PHY_STATUS_VBUS_VLD)) {
		if (icx_dmp_board_id.setid == ICX_DMP_SETID_UNKNOWN) {
			spin_unlock_irqrestore(&usbmisc->lock, flags);
			dev_warn(data->dev, "On EVK: vbus is error\n");
			return -EINVAL;
		}
		/* "DMP" */
		/* USB1_VBUS can not sense VBUS line if it satisfies one of
		 * following conditions.
		 * 1. On "BB", always.
		 * 2. On booting for all boards.
		 */
		dev_notice(data->dev, "on DMP: Skip VBUS check.\n");
		/* On "DMP", FUSB303D (Type-C controller) checks VBUS
		 * level, so USB MISC doesn't need check VBUS level.
		 */
	}

	/*
	 * - Do not check whether a charger is connected to the USB port
	 * - Check whether the USB plug has been in contact with each other
	 */
	val = readl(usbmisc->base + MX7D_USB_OTG_PHY_CFG2);
	writel(val | MX7D_USB_OTG_PHY_CFG2_CHRG_DCDENB,
			usbmisc->base + MX7D_USB_OTG_PHY_CFG2);

	spin_unlock_irqrestore(&usbmisc->lock, flags);
	/* Check if plug is connected */
	for (i = 0; i < 100; i = i + 1) {
		val = readl(usbmisc->base + MX7D_USB_OTG_PHY_STATUS);
		if (!(val & MX7D_USB_OTG_PHY_STATUS_LINE_STATE0)) {
			if (data_pin_contact_count++ > 5)
				/* Data pin makes contact */
				break;
			else
				usleep_range(5000, 10000);
		} else {
			data_pin_contact_count = 0;
			usleep_range(5000, 6000);
		}
	}

	if (i == 100) {
		dev_notice(data->dev,
			"VBUS is coming from a dedicated power supply.\n");
		imx7_disable_charger_detector(data);
		return -ENXIO;
	}

	return 0;
}

#define	PRIMARY_DETECTION_MS			(120)
#define	PRIMARY_DETECTION_ENOUGH_MS		(50)
#define	PRIMARY_DETECTION_INTERVAL_MIN_US	(1000)
#define	PRIMARY_DETECTION_INTERVAL_MAX_US	(2000)

static int imx7d_charger_primary_detection(struct imx_usbmisc_data *data)
{
	struct imx_usbmisc *usbmisc = dev_get_drvdata(data->dev);
	struct usb_phy *usb_phy = data->usb_phy;
	unsigned long flags;
	u32 val;
	int ret;
	int	stable;
	int	i;

	ret = imx7d_charger_data_contact_detect(data);
	if (ret)
		return ret;

	spin_lock_irqsave(&usbmisc->lock, flags);
	/* Set OPMODE to be non-driving mode */
	val = readl(usbmisc->base + MX7D_USBNC_USB_CTRL2);
	val &= ~MX7D_USBNC_USB_CTRL2_OPMODE_OVERRIDE_MASK;
	val |= MX7D_USBNC_USB_CTRL2_OPMODE_NON_DRIVING;
	writel(val, usbmisc->base + MX7D_USBNC_USB_CTRL2);

	val = readl(usbmisc->base + MX7D_USBNC_USB_CTRL2);
	writel(val | MX7D_USBNC_USB_CTRL2_OPMODE_OVERRIDE_EN,
			usbmisc->base + MX7D_USBNC_USB_CTRL2);

	/*
	 * - Do check whether a charger is connected to the USB port
	 * - Do not Check whether the USB plug has been in contact with
	 * each other
	 */
	/* VDATSRCENB0=1 and CHRGSEL=0
	 *  connect VDP_SRC to DP
	 *  connect IDM_SINK to DM
	 * VDATDETENB0=1
	 *  connect IDP_SRC (It may have another effect).
	 */
	val = readl(usbmisc->base + MX7D_USB_OTG_PHY_CFG2);
	writel(val | MX7D_USB_OTG_PHY_CFG2_CHRG_VDATSRCENB0 |
			MX7D_USB_OTG_PHY_CFG2_CHRG_VDATDETENB0,
				usbmisc->base + MX7D_USB_OTG_PHY_CFG2);
	spin_unlock_irqrestore(&usbmisc->lock, flags);

	stable = 0;
	i = 0;
	while (i < usbmisc->vdm_src_poll_ms) {
		val = readl(usbmisc->base + MX7D_USB_OTG_PHY_STATUS);
		if ((val & MX7D_USB_OTG_PHY_STATUS_CHRGDET) != 0) {
			stable++;
			if (stable >= PRIMARY_DETECTION_ENOUGH_MS) {
				/* "Detected CDP" or "Detected DCP" */
				break;
			}
		} else {
			/* "It may not connected" or "Connected SDP" */
			stable = 0;
		}
		usleep_range(
			PRIMARY_DETECTION_INTERVAL_MIN_US,
			PRIMARY_DETECTION_INTERVAL_MAX_US
		);
		i++;
	}
	if (stable < PRIMARY_DETECTION_ENOUGH_MS) {
		dev_dbg(data->dev, "It may be a standard downstream port\n");
		usb_phy->chg_type = SDP_TYPE;
	}

	imx7_disable_charger_detector(data);

	if (usb_phy->chg_type == SDP_TYPE) {
		/* Detected as SDP, try identify A-Type charger. */
		/* Ignore error. */
		(void) imx7d_detect_some_kind_of_a(data,
			ICX_PLATFORM_WAIT_LS_POLL_LONG,
			ICX_PLATFORM_WAIT_LS_POLL_LONG_DET
		);
	}

	return 0;
}

static int imx7d_charger_detection(struct imx_usbmisc_data *data)
{
	struct usb_phy *usb_phy = data->usb_phy;
	int ret;

	ret = imx7d_charger_primary_detection(data);
	if (ret)
		return ret;

	switch (usb_phy->chg_type) {
	case ACA_TYPE:
	case SDP_TYPE:
		return ret;
	default:
		/* Continue charger detection. */
		break;
	}

	/* Still remain UNKNOWN_TYPE */
	ret = imx7d_charger_secondary_detection(data);
	switch (usb_phy->chg_type) {
	case CDP_TYPE:
		return ret;
	default:
		/* Continue charger detection. */
		break;
	}
	/* DCP_TYPE or unknown, ignore error.
	 * Detect some USB AC charger shorted DP with DM,
	 * and terminated by Thevenin termination.
	 * Here we spend enough time, DP and DM
	 * may be contacted.
	 */
	(void) imx7d_detect_some_kind_of_a(data,
		ICX_PLATFORM_WAIT_LS_POLL_SHORT,
		ICX_PLATFORM_WAIT_LS_POLL_SHORT_DET
	);

	return ret;
}

static int usbmisc_term_select_override(struct imx_usbmisc_data *data,
						bool enable, int val)
{
	struct imx_usbmisc *usbmisc = dev_get_drvdata(data->dev);
	unsigned long flags;
	u32 reg;

	spin_lock_irqsave(&usbmisc->lock, flags);

	reg = readl(usbmisc->base + MX7D_USBNC_USB_CTRL2);
	if (enable) {
		if (val)
			writel(reg | MX7D_USB_TERMSEL_OVERRIDE,
				usbmisc->base + MX7D_USBNC_USB_CTRL2);
		else
			writel(reg & ~MX7D_USB_TERMSEL_OVERRIDE,
				usbmisc->base + MX7D_USBNC_USB_CTRL2);

		reg = readl(usbmisc->base + MX7D_USBNC_USB_CTRL2);
		writel(reg | MX7D_USB_TERMSEL_OVERRIDE_EN,
			usbmisc->base + MX7D_USBNC_USB_CTRL2);
	} else {
		writel(reg & ~MX7D_USB_TERMSEL_OVERRIDE_EN,
			usbmisc->base + MX7D_USBNC_USB_CTRL2);
	}

	spin_unlock_irqrestore(&usbmisc->lock, flags);

	return 0;
}

static int usbmisc_imx7ulp_init(struct imx_usbmisc_data *data)
{
	struct imx_usbmisc *usbmisc = dev_get_drvdata(data->dev);
	unsigned long flags;
	u32 reg;

	if (data->index >= 1)
		return -EINVAL;

	spin_lock_irqsave(&usbmisc->lock, flags);
	reg = readl(usbmisc->base);
	if (data->disable_oc) {
		reg |= MX6_BM_OVER_CUR_DIS;
	} else if (data->oc_polarity == 1) {
		/* High active */
		reg &= ~(MX6_BM_OVER_CUR_DIS | MX6_BM_OVER_CUR_POLARITY);
	}

	if (data->pwr_polarity)
		reg |= MX6_BM_PRW_POLARITY;

	writel(reg, usbmisc->base);

	/* SoC non-burst setting */
	reg = readl(usbmisc->base);
	writel(reg | MX6_BM_NON_BURST_SETTING, usbmisc->base);

	if (data->hsic) {
		reg = readl(usbmisc->base);
		writel(reg | MX6_BM_UTMI_ON_CLOCK, usbmisc->base);

		reg = readl(usbmisc->base + MX6_USB_HSIC_CTRL_OFFSET);
		reg |= MX6_BM_HSIC_EN | MX6_BM_HSIC_CLK_ON;
		writel(reg, usbmisc->base + MX6_USB_HSIC_CTRL_OFFSET);

		/*
		 * For non-HSIC controller, the autoresume is enabled
		 * at MXS PHY driver (usbphy_ctrl bit18).
		 */
		reg = readl(usbmisc->base + MX7D_USBNC_USB_CTRL2);
		writel(reg | MX7D_USBNC_AUTO_RESUME,
			usbmisc->base + MX7D_USBNC_USB_CTRL2);
	} else {
		reg = readl(usbmisc->base + MX7D_USBNC_USB_CTRL2);
		reg &= ~MX7D_USB_VBUS_WAKEUP_SOURCE_MASK;
		writel(reg | MX7D_USB_VBUS_WAKEUP_SOURCE_BVALID,
			 usbmisc->base + MX7D_USBNC_USB_CTRL2);
	}

	spin_unlock_irqrestore(&usbmisc->lock, flags);

	usbmisc_imx7d_set_wakeup(data, false);

	return 0;
}

static const struct usbmisc_ops imx25_usbmisc_ops = {
	.init = usbmisc_imx25_init,
	.post = usbmisc_imx25_post,
};

static const struct usbmisc_ops imx27_usbmisc_ops = {
	.init = usbmisc_imx27_init,
};

static const struct usbmisc_ops imx51_usbmisc_ops = {
	.init = usbmisc_imx53_init,
};

static const struct usbmisc_ops imx53_usbmisc_ops = {
	.init = usbmisc_imx53_init,
};

static const struct usbmisc_ops imx6q_usbmisc_ops = {
	.set_wakeup = usbmisc_imx6q_set_wakeup,
	.init = usbmisc_imx6q_init,
	.hsic_set_connect = usbmisc_imx6_hsic_set_connect,
	.hsic_set_clk   = usbmisc_imx6_hsic_set_clk,
};

static const struct usbmisc_ops vf610_usbmisc_ops = {
	.init = usbmisc_vf610_init,
};

static const struct usbmisc_ops imx6sx_usbmisc_ops = {
	.set_wakeup = usbmisc_imx6q_set_wakeup,
	.init = usbmisc_imx6sx_init,
	.power_lost_check = usbmisc_imx6sx_power_lost_check,
	.hsic_set_connect = usbmisc_imx6_hsic_set_connect,
	.hsic_set_clk = usbmisc_imx6_hsic_set_clk,
};

static const struct usbmisc_ops imx7d_usbmisc_ops = {
	.init = usbmisc_imx7d_init,
	.set_wakeup = usbmisc_imx7d_set_wakeup,
	.power_lost_check = usbmisc_imx7d_power_lost_check,
	.charger_detection = imx7d_charger_detection,
	.term_select_override = usbmisc_term_select_override,
        .hsic_set_suspend = usbmisc_imx7_hsic_set_suspend,
        .hsic_set_resume = usbmisc_imx7_hsic_set_resume,
};

static const struct usbmisc_ops imx7ulp_usbmisc_ops = {
	.init = usbmisc_imx7ulp_init,
	.set_wakeup = usbmisc_imx7d_set_wakeup,
	.power_lost_check = usbmisc_imx7d_power_lost_check,
	.hsic_set_connect = usbmisc_imx6_hsic_set_connect,
	.hsic_set_clk   = usbmisc_imx6_hsic_set_clk,
};

static inline bool is_imx53_usbmisc(struct imx_usbmisc_data *data)
{
	struct imx_usbmisc *usbmisc = dev_get_drvdata(data->dev);

	return usbmisc->ops == &imx53_usbmisc_ops;
}

int imx_usbmisc_init(struct imx_usbmisc_data *data)
{
	struct imx_usbmisc *usbmisc;

	if (!data)
		return 0;

	usbmisc = dev_get_drvdata(data->dev);
	if (!usbmisc->ops->init)
		return 0;
	return usbmisc->ops->init(data);
}
EXPORT_SYMBOL_GPL(imx_usbmisc_init);

int imx_usbmisc_init_post(struct imx_usbmisc_data *data)
{
	struct imx_usbmisc *usbmisc;

	if (!data)
		return 0;

	usbmisc = dev_get_drvdata(data->dev);
	if (!usbmisc->ops->post)
		return 0;
	return usbmisc->ops->post(data);
}
EXPORT_SYMBOL_GPL(imx_usbmisc_init_post);

int imx_usbmisc_set_wakeup(struct imx_usbmisc_data *data, bool enabled)
{
	struct imx_usbmisc *usbmisc;

	if (!data)
		return 0;

	usbmisc = dev_get_drvdata(data->dev);
	if (!usbmisc->ops->set_wakeup)
		return 0;
	return usbmisc->ops->set_wakeup(data, enabled);
}
EXPORT_SYMBOL_GPL(imx_usbmisc_set_wakeup);

int imx_usbmisc_charger_detection(struct imx_usbmisc_data *data, bool connect)
{
	struct imx_usbmisc *usbmisc;
	struct usb_phy *usb_phy;
	int ret = 0;

	if (!data)
		return -EINVAL;

	usbmisc = dev_get_drvdata(data->dev);
	usb_phy = data->usb_phy;
	if (!usbmisc->ops->charger_detection)
		return -ENOTSUPP;

	mutex_lock(&usbmisc->mutex);
	if (connect) {
		ret = usbmisc->ops->charger_detection(data);
		if (ret) {
			dev_err(data->dev,
					"Error occurs during detection: %d\n",
					ret);
			usb_phy->chg_state = USB_CHARGER_ABSENT;
		} else {
			usb_phy->chg_state = USB_CHARGER_PRESENT;
		}
	} else {
		usb_phy->chg_state = USB_CHARGER_ABSENT;
		usb_phy->chg_type = UNKNOWN_TYPE;
	}
	mutex_unlock(&usbmisc->mutex);
	return ret;
}
EXPORT_SYMBOL_GPL(imx_usbmisc_charger_detection);

int imx_usbmisc_power_lost_check(struct imx_usbmisc_data *data)
{
	struct imx_usbmisc *usbmisc;

	if (!data)
		return 0;

	usbmisc = dev_get_drvdata(data->dev);
	if (!usbmisc->ops->power_lost_check)
		return 0;
	return usbmisc->ops->power_lost_check(data);
}
EXPORT_SYMBOL_GPL(imx_usbmisc_power_lost_check);

int imx_usbmisc_hsic_set_connect(struct imx_usbmisc_data *data)
{
	struct imx_usbmisc *usbmisc;

	if (!data)
		return 0;

	usbmisc = dev_get_drvdata(data->dev);
	if (!usbmisc->ops->hsic_set_connect || !data->hsic)
		return 0;
	return usbmisc->ops->hsic_set_connect(data);
}
EXPORT_SYMBOL_GPL(imx_usbmisc_hsic_set_connect);

int imx_usbmisc_hsic_set_clk(struct imx_usbmisc_data *data, bool on)
{
	struct imx_usbmisc *usbmisc;

	if (!data)
		return 0;

	usbmisc = dev_get_drvdata(data->dev);
	if (!usbmisc->ops->hsic_set_clk || !data->hsic)
		return 0;
	return usbmisc->ops->hsic_set_clk(data, on);
}
EXPORT_SYMBOL_GPL(imx_usbmisc_hsic_set_clk);

int imx_usbmisc_suspend(struct imx_usbmisc_data *data)
{
	struct imx_usbmisc *usbmisc;

	if (!data)
		return 0;

	usbmisc = dev_get_drvdata(data->dev);

	if (!usbmisc->ops->hsic_set_suspend)
		return 0;
	return usbmisc->ops->hsic_set_suspend(data);
}
EXPORT_SYMBOL_GPL(imx_usbmisc_suspend);

int imx_usbmisc_resume(struct imx_usbmisc_data *data)
{
	struct imx_usbmisc *usbmisc;

	if (!data)
		return 0;

	usbmisc = dev_get_drvdata(data->dev);

	if (!usbmisc->ops->hsic_set_resume)
		return 0;
	return usbmisc->ops->hsic_set_resume(data);
}
EXPORT_SYMBOL_GPL(imx_usbmisc_resume);



int imx_usbmisc_term_select_override(struct imx_usbmisc_data *data,
						bool enable, int val)
{
	struct imx_usbmisc *usbmisc;

	if (!data)
		return 0;

	usbmisc = dev_get_drvdata(data->dev);
	if (!usbmisc->ops->term_select_override)
		return 0;
	return usbmisc->ops->term_select_override(data, enable, val);
}
EXPORT_SYMBOL_GPL(imx_usbmisc_term_select_override);

static const struct of_device_id usbmisc_imx_dt_ids[] = {
	{
		.compatible = "fsl,imx25-usbmisc",
		.data = &imx25_usbmisc_ops,
	},
	{
		.compatible = "fsl,imx35-usbmisc",
		.data = &imx25_usbmisc_ops,
	},
	{
		.compatible = "fsl,imx27-usbmisc",
		.data = &imx27_usbmisc_ops,
	},
	{
		.compatible = "fsl,imx51-usbmisc",
		.data = &imx51_usbmisc_ops,
	},
	{
		.compatible = "fsl,imx53-usbmisc",
		.data = &imx53_usbmisc_ops,
	},
	{
		.compatible = "fsl,imx6q-usbmisc",
		.data = &imx6q_usbmisc_ops,
	},
	{
		.compatible = "fsl,vf610-usbmisc",
		.data = &vf610_usbmisc_ops,
	},
	{
		.compatible = "fsl,imx6sx-usbmisc",
		.data = &imx6sx_usbmisc_ops,
	},
	{
		.compatible = "fsl,imx6ul-usbmisc",
		.data = &imx6sx_usbmisc_ops,
	},
	{
		.compatible = "fsl,imx7d-usbmisc",
		.data = &imx7d_usbmisc_ops,
	},
	{
		.compatible = "fsl,imx7ulp-usbmisc",
		.data = &imx7ulp_usbmisc_ops,
	},
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, usbmisc_imx_dt_ids);

/* Show vdm_src_poll_ms
 */
static ssize_t usbmisc_imx_vdm_src_poll_ms_show(struct device *dev,
	struct device_attribute *attr,
	char *buf)
{
	struct imx_usbmisc	*usbmisc;
	ssize_t			result;

	usbmisc = dev_get_drvdata(dev);

	result = snprintf(buf, PAGE_SIZE, "%d\n",
		usbmisc->vdm_src_poll_ms
	);
	return result;
}

/* Store vdm_src_poll_ms
 */
static ssize_t usbmisc_imx_vdm_src_poll_ms_store(struct device *dev,
	struct device_attribute *attr,
	const char *buf,
	size_t size)
{
	struct imx_usbmisc	*usbmisc;
	int	ret;
	ssize_t result = -EINVAL;
	long	attr_val;

	usbmisc = dev_get_drvdata(dev);

	if (size >= PAGE_SIZE) {
		/* Too long write. */
		result = -EINVAL;
		dev_err(usbmisc->dev, "Too long write. size=%lu, result=%ld\n",
			(unsigned long)size, (long)result
		);
		goto out;
	}

	attr_val = PRIMARY_DETECTION_MS;
	ret = kstrtol(buf, 0, &attr_val);
	if (ret != 0) {
		result = (ssize_t)ret;
		dev_err(usbmisc->dev, "Invalid value. result=%ld\n",
			(long)result
		);
		goto out;
	}
	if ((attr_val < PRIMARY_DETECTION_ENOUGH_MS) ||
	    (attr_val > INT_MAX)) {
		result = -EINVAL;
		dev_err(usbmisc->dev, "Invalid value. result=%ld, val=%ld\n",
			(long)result, (long)attr_val
		);
	}
	usbmisc->vdm_src_poll_ms = attr_val;
	wmb();	/* Sync with driver context. */
	result = (__force ssize_t)size;

out:
	return result;
}

/*! vdm_src_poll_ms - VDM_SRC polling time in msec
 *  Mode:
 *   0644
 *  Description:
 *   Connector mechanical design trim interface.
 *  Usage:
 *  Write String: Function
 *   "5..200":  Polling VDM_SRC voltage on DM line.
 *              The range 5..200 are typical value range.
 */
static DEVICE_ATTR(vdm_src_poll_ms, 0644,
	usbmisc_imx_vdm_src_poll_ms_show,
	usbmisc_imx_vdm_src_poll_ms_store
	);

static struct attribute *usbmisc_imx_attributes[] = {
	&dev_attr_vdm_src_poll_ms.attr,
	NULL,
};

static const struct attribute_group usbmisc_imx_attr_group = {
	.attrs = usbmisc_imx_attributes,
};

/* prop: vdm-src-poll-ms
 * format: <u32>
 * Set polling VDM_SRC voltage on DM line duration time in milli seconds.
 * It may spend "vdm-src-poll-ms" milli seconds or more
 * at primary detection procedure.
 * This parameter depends on USB Type-C connector mechanical design.
 * Insertion time, and contact orders, debounce time, and etc...
 */
static const char *prop_vdm_src_poll_ms =
	"svs,vdm-src-poll-ms";

static int usbmisc_imx_of_probe(struct imx_usbmisc *usbmisc)
{
	u32		of_val;
	const char	*prop;
	int		ret = 0;
	int		result = 0;

	usbmisc->vdm_src_poll_ms = PRIMARY_DETECTION_MS;
	of_val = PRIMARY_DETECTION_MS;
	prop = prop_vdm_src_poll_ms;
	ret = device_property_read_u32(usbmisc->dev, prop, &of_val);
	if (ret == 0) {
		/* Get property. */
		if (of_val > INT_MAX) {
			/* too large value. */
			of_val = PRIMARY_DETECTION_MS;
			dev_warn(usbmisc->dev, "Too large property value. prop=%s, val(fixed)=%u\n",
				prop, (unsigned int)of_val
			);
		}
		if (of_val < PRIMARY_DETECTION_ENOUGH_MS) {
			/* too small value. */
			of_val = PRIMARY_DETECTION_ENOUGH_MS;
			dev_warn(usbmisc->dev, "Too small property value. prop=%s, val(fixed)=%u\n",
				prop, (unsigned int)of_val
			);
		}
		usbmisc->vdm_src_poll_ms = (__force int)of_val;
	} else {
		/* There is no property. */
		result = result ? result : ret;
	}
	return result;
}

static int usbmisc_imx_probe(struct platform_device *pdev)
{
#define DEFER_COUNT_MAX	(20)
	static int defer_count; /* conter starts zero. */
	struct device	*dev;
	struct resource	*res;
	struct imx_usbmisc *data;
	const struct of_device_id *of_id;
	long	board_id_init;
	int	ret = 0;
	int	result = 0;

	dev = &(pdev->dev);
	of_id = of_match_device(usbmisc_imx_dt_ids, dev);
	if (!of_id)
		return -ENODEV;

	board_id_init = atomic_read(&(icx_dmp_board_id.init));
	if (board_id_init == ICX_DMP_INIT_NOTYET) {
		if (board_id_init >= 0) {
			if (defer_count < DEFER_COUNT_MAX) {
				defer_count++;
				dev_notice(dev, "Defer probe. count=%d\n",
					defer_count
				);
				return -EPROBE_DEFER;
			}
		}
	}

	data = devm_kzalloc(dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	spin_lock_init(&data->lock);
	mutex_init(&data->mutex);
	data->dev = dev;
	/* Ignore error */
	(void) usbmisc_imx_of_probe(data);

	/* create sysfs */
	ret = sysfs_create_group(&(dev->kobj), &usbmisc_imx_attr_group);
	if (ret != 0) {
		dev_err(dev, "Can not create device attribute node. ret=%d\n",
			ret
		);
		result = ret;
		goto out_err;
	}

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	data->base = devm_ioremap_resource(dev, res);
	if (IS_ERR(data->base)) {
		result = PTR_ERR(data->base);
		dev_err(dev, "Can not map device register memory. result=%d\n",
			result
		);
		goto out_remove_sysfs;
	}

	data->ops = (const struct usbmisc_ops *)of_id->data;
	platform_set_drvdata(pdev, data);

	vbus_wakeup_reg = devm_regulator_get(dev, "vbus-wakeup");
	if (PTR_ERR(vbus_wakeup_reg) == -EPROBE_DEFER) {
		result = -EPROBE_DEFER;
		goto out_remove_sysfs;
	} else if (PTR_ERR(vbus_wakeup_reg) == -ENODEV) {
		/* no vbus regualator is needed */
		vbus_wakeup_reg = NULL;
	} else if (IS_ERR(vbus_wakeup_reg)) {
		result = PTR_ERR(vbus_wakeup_reg);
		dev_err(dev, "Getting regulator error: %d\n",
			result);
		goto out_remove_sysfs;
	}

	return 0;

out_remove_sysfs:
	sysfs_remove_group(&(dev->kobj), &usbmisc_imx_attr_group);
out_err:
	return result;
}

static int usbmisc_imx_remove(struct platform_device *pdev)
{
	struct device	*dev;

	dev = &(pdev->dev);
	sysfs_remove_group(&(dev->kobj), &usbmisc_imx_attr_group);
	return 0;
}

static struct platform_driver usbmisc_imx_driver = {
	.probe = usbmisc_imx_probe,
	.remove = usbmisc_imx_remove,
	.driver = {
		.name = "usbmisc_imx",
		.of_match_table = usbmisc_imx_dt_ids,
	 },
};

module_platform_driver(usbmisc_imx_driver);

MODULE_ALIAS("platform:usbmisc-imx");
MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("driver for imx usb non-core registers");
MODULE_AUTHOR("Richard Zhao <richard.zhao@freescale.com>");
