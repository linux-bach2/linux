/* SPDX-License-Identifier: GPL-2.0-only */
/* Exact cold sequence from stock bfgx_and_wifi_cfg; SHA256 d2b0f95d82154597529d0cec8d8c4caba3b00494106441352ad82de78d02b8c1.
 * SHUTDOWN_BFGX expands as in vendor exec_shutdown_type_cmd(). */
static const char *const hi1102_fw_names[] = {
	"hi1102/cr4_asic.bin",
	"hi1102/CPU_RAM_SCHED.bin",
	"hi1102/CPU_RAM_WBS.bin",
};

static const struct hi1102_boot_op hi1102_boot_ops[] = {
	{ HI_CMD, "SETPM 1 ", 0 },
	{ HI_CMD, "WRITEM 2 0x50000140 6 ", 0 },
	{ HI_CMD, "WRITEM 2 0x5000034C 0 ", 0 },
	{ HI_CMD, "WRITEM 2 0x50007030 0 ", 0 },
	{ HI_CMD, "WRITEM 2 0x50007004 2 ", 0 },
	{ HI_CMD, "WRITEM 2 0x50007000 2 ", 0 },
	{ HI_FILE, "hi1102/cr4_asic.bin", 0x70000 },
	{ HI_FILE, "hi1102/CPU_RAM_SCHED.bin", 0x300E4000 },
	{ HI_FILE, "hi1102/CPU_RAM_WBS.bin", 0x30168000 },
	{ HI_CALI, NULL, 0x30170000 },
	{ HI_CMD, "WRITEM 2 0x60001044 0x1 ", 0, true },
	{ HI_CMD, "WRITEM 2 0x50002280 0x5414 ", 0 },
	{ HI_CMD, "WRITEM 2 0x50002284 0xAC00 ", 0 },
	{ HI_CMD, "WRITEM 2 0x50002210 0x3518 ", 0 },
	{ HI_CMD, "WRITEM 2 0x50002214 0x3018 ", 0 },
	{ HI_CMD, "WRITEM 2 0x50000074 0x1 ", 0 },
	{ HI_CMD, "WRITEM 2 0x50000c04 0 " },
	{ HI_CMD, "WRITEM 2 0x50000094 1 " },
	{ HI_QUIT },
};
