// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 Tildeguy (tildeguy@mainlining.org)
 */

#include <linux/delay.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/notifier.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <linux/reboot.h>
#include <linux/string.h>
#include <linux/of.h>
#include <linux/regmap.h>
#include <linux/gpio/consumer.h>
#include <linux/mfd/syscon.h>

struct hi6250_reboot {
	u32 default_reason;
	struct device *dev;
	struct regmap *pmuctrl;
	struct regmap *sysctrl;
	struct gpio_desc *powerhold_gpio;
};

/* Bootup marker for Hi6250 BL */
/* TODO: Consider moving into a new driver under hisilicon,hi6250-pmuctrl */

#define HI6250_BOOTUP_PMUCTRL_OFFSET 0x630
#define HI6250_BOOTUP_PMUCTRL_MASK   0xff
#define HI6250_BOOTUP_PMUCTRL_VALUE  250

static void hi6250_reboot_init(
  struct hi6250_reboot *hi6250_reboot)
{
	regmap_update_bits(hi6250_reboot->pmuctrl,
		HI6250_BOOTUP_PMUCTRL_OFFSET,
		HI6250_BOOTUP_PMUCTRL_MASK,
		HI6250_BOOTUP_PMUCTRL_VALUE);
}

#define HI6250_REBOOT_REASON_PMUCTRL_OFFSET		0x62c
#define HI6250_REBOOT_REASON_PMUCTRL_MASK		0xff
#define HI6250_REBOOT_REASON_AP_S_COLDBOOT	0x00
#define HI6250_REBOOT_REASON_CHARGEREBOOT	0x06
#define HI6250_REBOOT_REASON_COLDBOOT		0x10

#define HI6250_RESET_SYSCTRL_OFFSET 0x510
#define HI6250_RESET_SYSCTRL_MASK   (1<<2)
#define HI6250_RESET_SYSCTRL_VALUE   (1<<2)

static void hi6250_reset_with_reason(
  struct hi6250_reboot *hi6250_reboot,
  u8 reason
) {
	/* do some goop with gpio25 */
	gpiod_set_value_cansleep(hi6250_reboot->powerhold_gpio, 1);
	mdelay(1000);

	/* setup reboot reason in pmuctrl */
	regmap_update_bits(hi6250_reboot->pmuctrl,
	  HI6250_REBOOT_REASON_PMUCTRL_OFFSET,
	  HI6250_REBOOT_REASON_PMUCTRL_MASK,
	  reason);

	/* request reset from sysctrl */
	regmap_update_bits(hi6250_reboot->sysctrl,
	  HI6250_RESET_SYSCTRL_OFFSET,
	  HI6250_RESET_SYSCTRL_MASK,
	  HI6250_RESET_SYSCTRL_VALUE);
}

static int hi6250_restart_handler(
  struct sys_off_data *data
) {
	struct hi6250_reboot *hi6250_reboot = data->cb_data;

	dev_info(hi6250_reboot->dev, "[restart]running out of goops");

	{
		u8 reason = hi6250_reboot->default_reason;
		const char *cmd = data->cmd;

		/* vendor enum (mntn_public_interface.h): bootloader 0x01, recovery 0x02, erecovery 0x08 */
		if (cmd && !strcmp(cmd, "bootloader"))
			reason = 0x01;
		else if (cmd && !strcmp(cmd, "recovery"))
			reason = 0x02;
		else if (cmd && !strcmp(cmd, "erecovery"))
			reason = 0x08;
		else if (cmd && !strcmp(cmd, "coldboot"))
			reason = HI6250_REBOOT_REASON_COLDBOOT;
		dev_info(hi6250_reboot->dev, "restart cmd=%s reason=0x%02x\n", cmd ? cmd : "(none)", reason);
		hi6250_reset_with_reason(hi6250_reboot, reason);
	}

	while (1)
		cpu_do_idle();

	return NOTIFY_DONE;
}

static int hi6250_poweroff_handler(
  struct sys_off_data *data
) {
	struct hi6250_reboot *hi6250_reboot = data->cb_data;
	
	dev_info(hi6250_reboot->dev, "[poweroff]running out of goops");

	hi6250_reset_with_reason(
		hi6250_reboot,
	  HI6250_REBOOT_REASON_COLDBOOT);
	
	while (1)
		cpu_do_idle();

	return NOTIFY_DONE;
}

static struct hi6250_reboot *hi6250_reboot_pending;	/* bach2: powerhold GPIO still to take */

static int hi6250_reboot_probe(struct platform_device *pdev)
{
	struct device_node *np = pdev->dev.of_node;
	struct hi6250_reboot *hi6250_reboot;
	int priority = SYS_OFF_PRIO_FIRMWARE + 1;
	int ret;

	hi6250_reboot = devm_kzalloc(&pdev->dev, sizeof(*hi6250_reboot),
				GFP_KERNEL);
	if (!hi6250_reboot)
		return -ENOMEM;

	hi6250_reboot->default_reason = HI6250_REBOOT_REASON_COLDBOOT;
	of_property_read_u32(np, "hisilicon,default-reboot-reason", &hi6250_reboot->default_reason);
	hi6250_reboot->pmuctrl = syscon_regmap_lookup_by_phandle(np,
						  "hisilicon,pmuctrl-syscon");
	if (IS_ERR(hi6250_reboot->pmuctrl))
		return dev_err_probe(&pdev->dev, PTR_ERR(hi6250_reboot->pmuctrl),
				     "failed to get pmuctrl regmap\n");

	hi6250_reboot->sysctrl = syscon_regmap_lookup_by_phandle(np,
						  "hisilicon,sysctrl-syscon");
	if (IS_ERR(hi6250_reboot->sysctrl))
		return dev_err_probe(&pdev->dev, PTR_ERR(hi6250_reboot->sysctrl),
				     "failed to get sysctrl regmap\n");

	/*
	 * bach2: this driver now registers at subsys_initcall, before the gpio25 controller probes. Restart and
	 * power-off work without the powerhold GPIO (gpiod_set_value_cansleep(NULL) does nothing), so a missing
	 * provider is not fatal here: hi6250_reboot_powerhold_late() takes the GPIO once it exists.
	 */
	hi6250_reboot->powerhold_gpio = devm_gpiod_get_optional(&pdev->dev, "hisilicon,powerhold",
							GPIOD_OUT_LOW);
	if (IS_ERR(hi6250_reboot->powerhold_gpio)) {
		if (PTR_ERR(hi6250_reboot->powerhold_gpio) != -EPROBE_DEFER)
			return dev_err_probe(&pdev->dev, PTR_ERR(hi6250_reboot->powerhold_gpio),
					     "failed to get powerhold gpio\n");
		hi6250_reboot->powerhold_gpio = NULL;
		hi6250_reboot_pending = hi6250_reboot;
	}

	hi6250_reboot->dev = &pdev->dev;
	hi6250_reboot_init(hi6250_reboot);
	
	ret = devm_register_sys_off_handler(&pdev->dev,
						  SYS_OFF_MODE_POWER_OFF,
					    priority,
					    hi6250_poweroff_handler,
					    hi6250_reboot);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "cannot register poweroff handler\n");

	dev_info(hi6250_reboot->dev, "registered poweroff driver");
	
	ret = devm_register_sys_off_handler(&pdev->dev,
					    SYS_OFF_MODE_RESTART,
					    priority,
					    hi6250_restart_handler,
					    hi6250_reboot);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "cannot register restart handler\n");

	dev_info(hi6250_reboot->dev, "registered restart driver");

	return ret;
}

static const struct of_device_id hi6250_reboot_of_match[] = {
	{ .compatible = "hisilicon,hi6250-reboot" },
	{}
};
MODULE_DEVICE_TABLE(of, hi6250_reboot_of_match);

static struct platform_driver hi6250_reboot_driver = {
	.probe = hi6250_reboot_probe,
	.driver = {
		.name = "hi6250-reboot",
		.of_match_table = hi6250_reboot_of_match,
	},
};
/*
 * bach2: register at subsys_initcall so the restart handler exists before ordinary (device_initcall) drivers
 * probe. Self-cycle c36 (2026-11-26): a driver's SError panic at 0.48 s could not reboot (this driver had not
 * probed yet and PSCI SYSTEM_RESET does nothing on this firmware), so the tablet hung until a 10 s power-key press.
 */
static int __init hi6250_reboot_driver_init(void)
{
	return platform_driver_register(&hi6250_reboot_driver);
}
subsys_initcall(hi6250_reboot_driver_init);

static int __init hi6250_reboot_powerhold_late(void)
{
	struct hi6250_reboot *r = hi6250_reboot_pending;
	struct gpio_desc *g;

	if (!r)
		return 0;
	g = devm_gpiod_get_optional(r->dev, "hisilicon,powerhold", GPIOD_OUT_LOW);
	if (IS_ERR(g)) {
		dev_err(r->dev, "powerhold gpio still unavailable: %ld\n", PTR_ERR(g));
		return 0;
	}
	r->powerhold_gpio = g;
	dev_info(r->dev, "powerhold gpio taken late\n");
	return 0;
}
late_initcall_sync(hi6250_reboot_powerhold_late);
