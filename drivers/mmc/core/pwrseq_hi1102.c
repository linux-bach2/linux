// SPDX-License-Identifier: GPL-2.0-only
/* Hi1102 power sequencing on Bach2, derived from the vendor board.c sequence. */
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/mmc/host.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/pinctrl/consumer.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include "pwrseq.h"

struct hi1102_pwrseq {
	struct mmc_pwrseq pwrseq;
	struct clk *clk;
	struct gpio_desc *reset, *xldo;
	struct pinctrl *pins;
	struct pinctrl_state *normal, *prepare;
	bool clk_enabled, prepared;
};

static struct hi1102_pwrseq *to_hi1102(struct mmc_host *host)
{
	return container_of(host->pwrseq, struct hi1102_pwrseq, pwrseq);
}

static void hi1102_pre_power_on(struct mmc_host *host)
{
	struct hi1102_pwrseq *p = to_hi1102(host);
	struct device *dev = p->pwrseq.dev;
	int ret;

	p->prepared = false;
	if (!p->clk_enabled) {
		ret = clk_prepare_enable(p->clk);
		if (ret) {
			dev_err(dev, "32k clock enable failed: %d\n", ret);
			return;
		}
		p->clk_enabled = true;
	}
	ret = pinctrl_select_state(p->pins, p->prepare);
	if (ret) {
		dev_err(dev, "prepare pin state failed: %d\n", ret);
		return;
	}
	/* UART RX must be high while the shared Wi-Fi/BT reset is released. */
	p->xldo = gpiod_get(dev, "xldo", GPIOD_OUT_HIGH);
	if (IS_ERR(p->xldo)) {
		dev_err(dev, "xldo GPIO request failed: %ld\n", PTR_ERR(p->xldo));
		p->xldo = NULL;
		return;
	}
	gpiod_set_value_cansleep(p->reset, 1);
	msleep(10);
	p->prepared = true;
	dev_info(dev, "power prepare: 32k enabled, xldo high, reset asserted\n");
}

static void hi1102_post_power_on(struct mmc_host *host)
{
	struct hi1102_pwrseq *p = to_hi1102(host);
	int ret;

	if (!p->prepared)
		return;
	gpiod_set_value_cansleep(p->reset, 0);
	msleep(30);
	gpiod_put(p->xldo);
	p->xldo = NULL;
	ret = pinctrl_select_state(p->pins, p->normal);
	if (ret)
		dev_err(p->pwrseq.dev, "normal pin state failed: %d\n", ret);
	else
		dev_info(p->pwrseq.dev, "power on: reset released, UART pins restored\n");
}

static void hi1102_power_off(struct mmc_host *host)
{
	struct hi1102_pwrseq *p = to_hi1102(host);

	gpiod_set_value_cansleep(p->reset, 1);
	if (p->xldo) {
		gpiod_put(p->xldo);
		p->xldo = NULL;
	}
	if (p->clk_enabled) {
		clk_disable_unprepare(p->clk);
		p->clk_enabled = false;
	}
	p->prepared = false;
}

static const struct mmc_pwrseq_ops hi1102_ops = {
	.pre_power_on = hi1102_pre_power_on,
	.post_power_on = hi1102_post_power_on,
	.power_off = hi1102_power_off,
};

static int hi1102_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct hi1102_pwrseq *p;

	p = devm_kzalloc(dev, sizeof(*p), GFP_KERNEL);
	if (!p)
		return -ENOMEM;
	p->clk = devm_clk_get(dev, "ext_clock");
	if (IS_ERR(p->clk))
		return dev_err_probe(dev, PTR_ERR(p->clk), "32k clock unavailable\n");
	p->reset = devm_gpiod_get(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(p->reset))
		return dev_err_probe(dev, PTR_ERR(p->reset), "reset GPIO unavailable\n");
	p->pins = devm_pinctrl_get(dev);
	if (IS_ERR(p->pins))
		return dev_err_probe(dev, PTR_ERR(p->pins), "pin controller unavailable\n");
	p->normal = pinctrl_lookup_state(p->pins, "default");
	if (IS_ERR(p->normal))
		return PTR_ERR(p->normal);
	p->prepare = pinctrl_lookup_state(p->pins, "power-prepare");
	if (IS_ERR(p->prepare))
		return PTR_ERR(p->prepare);
	p->pwrseq.dev = dev;
	p->pwrseq.ops = &hi1102_ops;
	p->pwrseq.owner = THIS_MODULE;
	platform_set_drvdata(pdev, p);
	return mmc_pwrseq_register(&p->pwrseq);
}

static void hi1102_remove(struct platform_device *pdev)
{
	struct hi1102_pwrseq *p = platform_get_drvdata(pdev);

	mmc_pwrseq_unregister(&p->pwrseq);
	if (p->xldo)
		gpiod_put(p->xldo);
	if (p->clk_enabled)
		clk_disable_unprepare(p->clk);
}

static const struct of_device_id hi1102_match[] = {
	{ .compatible = "hisilicon,hi1102-pwrseq" },
	{ }
};
MODULE_DEVICE_TABLE(of, hi1102_match);

static struct platform_driver hi1102_driver = {
	.probe = hi1102_probe,
	.remove = hi1102_remove,
	.driver = { .name = "hi1102-pwrseq", .of_match_table = hi1102_match },
};
module_platform_driver(hi1102_driver);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Hi1102 SDIO power sequence");
