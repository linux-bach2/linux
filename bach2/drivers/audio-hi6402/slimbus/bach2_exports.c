// SPDX-License-Identifier: GPL-2.0
/*
 * bach2: the vendor slimbus code was built into the 4.4 kernel, so the HI6402 codec reached it by
 * plain linkage. Out of tree, the codec and the version probe are separate modules: export the
 * entry points they use. No logic here.
 */
#include <linux/export.h>
#include "slimbus.h"

unsigned int slimbus_read_1byte(unsigned int reg);
unsigned int slimbus_read_4byte(unsigned int reg);
void slimbus_write_1byte(unsigned int reg, unsigned int val);
void slimbus_write_4byte(unsigned int reg, unsigned int val);
int slimbus_bus_configure(slimbus_bus_config_type_t type);

EXPORT_SYMBOL_GPL(slimbus_read_1byte);
EXPORT_SYMBOL_GPL(slimbus_read_4byte);
EXPORT_SYMBOL_GPL(slimbus_write_1byte);
EXPORT_SYMBOL_GPL(slimbus_write_4byte);
EXPORT_SYMBOL_GPL(slimbus_track_activate);
EXPORT_SYMBOL_GPL(slimbus_track_deactivate);
EXPORT_SYMBOL_GPL(slimbus_switch_framer);
EXPORT_SYMBOL_GPL(slimbus_pause_clock);
EXPORT_SYMBOL_GPL(slimbus_track_recover);
EXPORT_SYMBOL_GPL(slimbus_trackstate_get);
EXPORT_SYMBOL_GPL(slimbus_bus_configure);

#include <linux/module.h>
MODULE_DESCRIPTION("bach2: vendor HiSilicon/Cadence SLIMbus manager (HI6402)");
