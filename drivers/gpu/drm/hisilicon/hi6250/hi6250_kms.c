// SPDX-License-Identifier: GPL-2.0-only
/*
 * HiSilicon Hi6250 (Kirin 659) display subsystem: an "adopt" KMS driver.
 *
 * The bootloader powers the DSS, sets up DSI, the panel and the whole online pipeline
 * (RCH D0 -> OVL0 -> DPP -> LDI0 -> DSI0) and leaves it scanning out its logo buffer. This
 * driver takes that pipeline over as it is. It never touches power, clocks, resets, the
 * SMMU, DSI or the panel: it only
 *  - points RCH D0 at the framebuffer to show: MCTL0 mutex on, D0 DMA_DATA_ADDR0 (and
 *    DMA_STRIDE0 if the pitch changes), MCTL RCH6 (D0) flush, mutex off. The hardware latches
 *    the shadow registers at the next frame start (MCTL0 TOP = 2, auto mode);
 *  - unmasks the ITF0 VSYNC interrupt (GLB ITF0/PDP mask registers, SPI 245) while vblank
 *    interrupts are needed, and clears the interrupt status it reads;
 *  - optionally enables the LDI0 output CRC (DRM CRC source "auto", debugfs only).
 *
 * The mode is read back from LDI0; the pixel clock is not visible in the DSS, so it comes
 * from the measured VSYNC rate (probe-time interrupt self-test), else the stock 160 MHz.
 * If the VSYNC interrupt does not behave (no interrupt, or a storm because the status does
 * not clear), vblank falls back to the DRM vblank timer, so page flips still complete.
 *
 * The probe refuses (-ENODEV) unless the pipeline is exactly in the state the bootloader
 * leaves it in; simplefb then stays in charge. Only after every check has passed are
 * conflicting framebuffers (simplefb on the logo buffer) removed. Disabling the plane or the
 * CRTC parks D0 on the bootloader's buffer ("boot" memory region, reserved and never freed),
 * so the panel never scans out freed memory.
 *
 * Buffers come from a dedicated CMA pool (memory-region "pool"); D0 reads physical addresses
 * (SMMU bypassed for this channel by the bootloader), 16-byte aligned, below 4 GiB.
 *
 * Register offsets: vendor soc_dss_interface.h (hi6250); the interrupt and flush sequences
 * follow the vendor hisifb driver (dss_pdp_isr, hisi_dss_mctl_mutex_lock/unlock) and the
 * Kirin 960 DRM driver (hisi_fb_pan_display).
 */

#include <linux/bitfield.h>
#include <linux/bits.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/interrupt.h>
#include <linux/jiffies.h>
#include <linux/ktime.h>
#include <linux/math64.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_reserved_mem.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/seq_file.h>
#include <linux/sizes.h>
#include <linux/spinlock.h>
#include <linux/workqueue.h>

#include <drm/clients/drm_client_setup.h>
#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_crtc.h>
#include <drm/drm_debugfs.h>
#include <drm/drm_debugfs_crc.h>
#include <drm/drm_drv.h>
#include <drm/drm_dumb_buffers.h>
#include <drm/drm_encoder.h>
#include <drm/drm_fb_dma_helper.h>
#include <drm/drm_fbdev_dma.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_gem_dma_helper.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_managed.h>
#include <drm/drm_modeset_helper_vtables.h>
#include <drm/drm_print.h>
#include <drm/drm_probe_helper.h>
#include <drm/drm_vblank.h>
#include <drm/drm_vblank_helper.h>

#include <linux/aperture.h>

#define DRIVER_NAME		"kirin"	/* mesa kmsro pairs "kirin" with a render-only GPU (panfrost) */
#define DRIVER_DESC		"HiSilicon Hi6250 DSS (adopted bootloader pipeline)"

/* DSS global block */
#define GLB_ITF0_INTS		0x00180	/* raw, write 1 to clear */
#define GLB_ITF0_INT_MSK	0x00184	/* 1 = masked */
#define GLB_PDP_INTS		0x00224
#define GLB_PDP_INT_MSK		0x00228
#define GLB_CRC_DBG_LDI0	0x00404
#define GLB_CRC_LDI0_EN		0x0040c
#define GLB_CRC_LDI0_FRM	0x00410
#define  PDP_ITF0_INTS		BIT(16)
#define  ITF0_VACTIVE0_END	BIT(8)
#define  ITF0_VACTIVE0_START	BIT(7)
#define  ITF0_VSYNC		BIT(4)
#define  ITF0_LDI_UNFLOW	BIT(2)

/* MCTL */
#define MCTL_SYS		0x10000
#define  MCTL_RCH6_FLUSH_EN	0x118	/* RCH6 = D0 */
#define  MCTL_OV0_FLUSH_EN	0x128
#define MCTL0			0x10800
#define  MCTL_CTL_EN		0x000
#define  MCTL_CTL_MUTEX		0x004
#define  MCTL_CTL_MUTEX_RCH6	0x048
#define  MCTL_CTL_TOP		0x050
#define  MCTL_CTL_FLUSH_STATUS	0x054
#define  MCTL_CTL_STATUS	0x068

/* RCH D0 (DMA + DFC) */
#define RCH_D0			0x50000
#define  DMA_OFT_X1		0x008
#define  DMA_OFT_Y1		0x00c
#define  DMA_CTRL		0x01c
#define   DMA_CTRL_TILE		BIT(1)
#define   DMA_CTRL_FMT		GENMASK(7, 3)
#define   DMA_CTRL_MMU		BIT(8)
#define   DMA_CTRL_FMT_BOOT	5	/* the bootloader's 32-bit BGRX layout (DRM XRGB8888) */
#define  DMA_DATA_ADDR0		0x060
#define  DMA_STRIDE0		0x064
#define   DMA_STRIDE0_STRIDE	GENMASK(15, 0)	/* in 16-byte units; b16-19 L2 interleave */
#define  CH_CTL			0x0d4
#define   CH_CTL_EN		BIT(0)
#define  DFC_DISP_SIZE		0x100	/* (w - 1) << 16 | (h - 1) */

/* OVL0 */
#define OVL0			0x60000
#define  OVL_SIZE		0x000	/* (h - 1) << 16 | (w - 1) */

/* LDI0 (video mode panel: HRZ_CTRL0 = hfp | (hbp + hsw - 1) << 16) */
#define LDI0			0x7d000
#define  LDI_HRZ_CTRL0		0x000
#define  LDI_HRZ_CTRL2		0x008	/* hactive - 1 */
#define  LDI_VRT_CTRL0		0x00c	/* vfp | vbp << 16 */
#define  LDI_VRT_CTRL1		0x010	/* vsw - 1 */
#define  LDI_VRT_CTRL2		0x014	/* vactive - 1 */
#define  LDI_CTRL		0x024
#define   LDI_CTRL_EN		BIT(0)
#define  LDI_VSTATE		0x054

/* CRG (syscon), read only: the DSS must be clocked, powered and out of isolation */
#define CRG_PERCLKEN3		0x038
#define  CRG_DSS_CLKS		(BIT(12) | BIT(13))	/* pclk_dss, aclk_dss */
#define CRG_ISOSTAT		0x14c
#define  CRG_DSS_ISO		BIT(6)
#define CRG_PERPWRACK		0x15c
#define  CRG_DSS_PWRACK		BIT(5)

#define HI6250_DSS_ALIGN	16
#define HI6250_PITCH_ALIGN	64	/* dumb buffers: panfrost renders linear rows at 64 bytes */
#define HI6250_STOCK_PIXCLK_KHZ	160000
#define HI6250_STOCK_HSW	20	/* stock panel: hsw 20, hbp 50; the LDI keeps only the sum */
#define HI6250_SELFTEST_MS	250
#define HI6250_IRQ_STORM	600	/* interrupts per second; expected: 59.5 */

static bool enable = true;
module_param(enable, bool, 0444);
MODULE_PARM_DESC(enable, "Adopt the bootloader display pipeline (0: leave simplefb in charge)");

static bool force_timer;
module_param(force_timer, bool, 0444);
MODULE_PARM_DESC(force_timer, "Use the DRM vblank timer instead of the VSYNC interrupt");

static bool flush_ov0;
module_param(flush_ov0, bool, 0644);
MODULE_PARM_DESC(flush_ov0, "Also flush OVL0 on each page flip (experiment, default off)");

struct hi6250_dss {
	struct drm_device drm;
	struct drm_plane primary;
	struct drm_crtc crtc;
	struct drm_encoder *encoder;
	struct drm_connector connector;
	struct drm_display_mode mode;

	void __iomem *base;
	struct regmap *crg;
	int irq;

	u32 boot_addr;		/* the bootloader's scanout buffer: the parking address */
	u32 boot_stride0;
	u32 boot_itf0_msk;
	u32 boot_pdp_msk;
	u32 boot_crc_en;
	u32 cur_addr;
	u32 cur_pitch;

	spinlock_t lock;	/* vbl_on, use_timer; serializes the flip sequence */
	bool vbl_on;
	bool use_timer;
	bool irq_dead;
	bool crc_on;
	struct work_struct irq_dead_work;

	/* statistics (debugfs) */
	unsigned long storm_win;
	unsigned int storm_n;
	unsigned int irqs;
	unsigned int vsyncs;
	unsigned int underflows;
	unsigned int flips;
	unsigned int parks;
	unsigned int selftest_vsyncs;
	u32 selftest_mhz;	/* measured VSYNC rate in mHz */
	ktime_t first_vsync;
	ktime_t last_vsync;
};

static inline struct hi6250_dss *to_dss(struct drm_device *drm)
{
	return container_of(drm, struct hi6250_dss, drm);
}

static inline u32 dss_rd(struct hi6250_dss *dss, u32 off)
{
	return readl(dss->base + off);
}

static inline void dss_wr(struct hi6250_dss *dss, u32 off, u32 val)
{
	writel(val, dss->base + off);
}

/* ------------------------------------------------------------------------------------------
 * Scanout: the four-write page flip on RCH D0, latched by the hardware at the next frame.
 */
static void hi6250_dss_scanout(struct hi6250_dss *dss, u32 addr, u32 pitch)
{
	unsigned long flags;

	spin_lock_irqsave(&dss->lock, flags);
	dss_wr(dss, MCTL0 + MCTL_CTL_MUTEX, 1);
	dss_wr(dss, RCH_D0 + DMA_DATA_ADDR0, addr);
	if (pitch != dss->cur_pitch) {
		u32 s = dss_rd(dss, RCH_D0 + DMA_STRIDE0) & ~DMA_STRIDE0_STRIDE;

		dss_wr(dss, RCH_D0 + DMA_STRIDE0, s | (pitch / HI6250_DSS_ALIGN));
		dss->cur_pitch = pitch;
	}
	dss_wr(dss, MCTL_SYS + MCTL_RCH6_FLUSH_EN, 1);
	if (READ_ONCE(flush_ov0))
		dss_wr(dss, MCTL_SYS + MCTL_OV0_FLUSH_EN, 1);
	dss_wr(dss, MCTL0 + MCTL_CTL_MUTEX, 0);
	dss->cur_addr = addr;
	spin_unlock_irqrestore(&dss->lock, flags);
}

static void hi6250_dss_park(struct hi6250_dss *dss)
{
	if (dss->cur_addr == dss->boot_addr && dss->cur_pitch == dss->mode.hdisplay * 4)
		return;
	hi6250_dss_scanout(dss, dss->boot_addr, dss->mode.hdisplay * 4);
	dss->parks++;
}

/* ------------------------------------------------------------------------------------------
 * Interrupts and vblank
 */
static void hi6250_dss_irq_unmask(struct hi6250_dss *dss)
{
	dss_wr(dss, GLB_ITF0_INTS, ITF0_VSYNC);
	dss_wr(dss, GLB_ITF0_INT_MSK, dss->boot_itf0_msk & ~ITF0_VSYNC);
	dss_wr(dss, GLB_PDP_INT_MSK, dss->boot_pdp_msk & ~PDP_ITF0_INTS);
}

static void hi6250_dss_irq_mask(struct hi6250_dss *dss)
{
	dss_wr(dss, GLB_PDP_INT_MSK, dss->boot_pdp_msk | PDP_ITF0_INTS);
	dss_wr(dss, GLB_ITF0_INT_MSK, dss->boot_itf0_msk | ITF0_VSYNC);
}

static bool hi6250_dss_vblank(struct hi6250_dss *dss)
{
	bool ret = drm_crtc_handle_vblank(&dss->crtc);

	if (READ_ONCE(dss->crc_on)) {
		u32 crc = dss_rd(dss, GLB_CRC_DBG_LDI0);

		drm_crtc_add_crc_entry(&dss->crtc, true, drm_crtc_vblank_count(&dss->crtc), &crc);
	}
	return ret;
}

static irqreturn_t hi6250_dss_irq(int irq, void *arg)
{
	struct hi6250_dss *dss = arg;
	u32 itf0, pdp, pending;

	/* vendor dss_pdp_isr order: read both levels, clear the second level, then the first */
	itf0 = dss_rd(dss, GLB_ITF0_INTS);
	pdp = dss_rd(dss, GLB_PDP_INTS);
	dss_wr(dss, GLB_ITF0_INTS, itf0);
	dss_wr(dss, GLB_PDP_INTS, pdp);
	pending = itf0 & ~dss_rd(dss, GLB_ITF0_INT_MSK);
	dss->irqs++;

	if (time_after(jiffies, dss->storm_win + HZ)) {
		dss->storm_win = jiffies;
		dss->storm_n = 0;
	}
	if (++dss->storm_n > HI6250_IRQ_STORM) {
		/* the status does not clear as the vendor driver says: give the line up */
		hi6250_dss_irq_mask(dss);
		disable_irq_nosync(irq);
		WRITE_ONCE(dss->irq_dead, true);
		schedule_work(&dss->irq_dead_work);
		return IRQ_HANDLED;
	}

	if (itf0 & ITF0_LDI_UNFLOW) {
		dss->underflows++;
		dev_warn_ratelimited(dss->drm.dev, "LDI0 underflow (itf0 %08x)\n", itf0);
	}
	if (!(pending & ITF0_VSYNC))
		return pending || pdp ? IRQ_HANDLED : IRQ_NONE;

	dss->last_vsync = ktime_get();
	if (!dss->vsyncs++)
		dss->first_vsync = dss->last_vsync;
	if (READ_ONCE(dss->vbl_on))
		hi6250_dss_vblank(dss);
	return IRQ_HANDLED;
}

static void hi6250_dss_irq_dead_work(struct work_struct *work)
{
	struct hi6250_dss *dss = container_of(work, struct hi6250_dss, irq_dead_work);
	unsigned long flags;
	bool start;

	drm_err(&dss->drm, "VSYNC interrupt storm (> %d/s): line disabled, vblank timer from now on\n",
		HI6250_IRQ_STORM);
	spin_lock_irqsave(&dss->lock, flags);
	start = dss->vbl_on && !dss->use_timer;
	dss->use_timer = true;
	spin_unlock_irqrestore(&dss->lock, flags);
	if (start)
		drm_crtc_vblank_start_timer(&dss->crtc);
}

static int hi6250_crtc_enable_vblank(struct drm_crtc *crtc)
{
	struct hi6250_dss *dss = to_dss(crtc->dev);
	unsigned long flags;
	bool timer;

	spin_lock_irqsave(&dss->lock, flags);
	WRITE_ONCE(dss->vbl_on, true);
	timer = dss->use_timer;
	if (!timer)
		hi6250_dss_irq_unmask(dss);
	spin_unlock_irqrestore(&dss->lock, flags);

	return timer ? drm_crtc_vblank_start_timer(crtc) : 0;
}

static void hi6250_crtc_disable_vblank(struct drm_crtc *crtc)
{
	struct hi6250_dss *dss = to_dss(crtc->dev);
	unsigned long flags;
	bool timer;

	spin_lock_irqsave(&dss->lock, flags);
	WRITE_ONCE(dss->vbl_on, false);
	timer = dss->use_timer;
	if (!timer)
		hi6250_dss_irq_mask(dss);
	spin_unlock_irqrestore(&dss->lock, flags);

	if (timer)
		drm_crtc_vblank_cancel_timer(crtc);
}

static bool hi6250_crtc_get_vblank_timestamp(struct drm_crtc *crtc, int *max_error,
					     ktime_t *vblank_time, bool in_vblank_irq)
{
	struct hi6250_dss *dss = to_dss(crtc->dev);

	if (!READ_ONCE(dss->use_timer))
		return false;	/* interrupt: DRM takes ktime_get() in the handler */
	return drm_crtc_vblank_helper_get_vblank_timestamp_from_timer(crtc, max_error,
								      vblank_time, in_vblank_irq);
}

static bool hi6250_crtc_handle_vblank_timeout(struct drm_crtc *crtc)
{
	return hi6250_dss_vblank(to_dss(crtc->dev));
}

/* ------------------------------------------------------------------------------------------
 * CRC (debugfs: dri/N/crtc-0/crc): the LDI0 output CRC, one value per frame
 */
static const char * const hi6250_crc_sources[] = { "auto" };

static int hi6250_crtc_verify_crc_source(struct drm_crtc *crtc, const char *source,
					 size_t *values_cnt)
{
	if (source && strcmp(source, "auto"))
		return -EINVAL;
	*values_cnt = 1;
	return 0;
}

static const char * const *hi6250_crtc_get_crc_sources(struct drm_crtc *crtc, size_t *count)
{
	*count = ARRAY_SIZE(hi6250_crc_sources);
	return hi6250_crc_sources;
}

static int hi6250_crtc_set_crc_source(struct drm_crtc *crtc, const char *source)
{
	struct hi6250_dss *dss = to_dss(crtc->dev);
	bool on = source && !strcmp(source, "auto");

	if (source && !on)
		return -EINVAL;
	dss_wr(dss, GLB_CRC_LDI0_EN, on ? 1 : dss->boot_crc_en);
	WRITE_ONCE(dss->crc_on, on);
	return 0;
}

/* ------------------------------------------------------------------------------------------
 * Primary plane (RCH D0)
 */
static const u32 hi6250_formats[] = {
	DRM_FORMAT_XRGB8888,
	DRM_FORMAT_ARGB8888,	/* OVL0 layer 0 is in SRC blend mode: alpha is ignored */
};

static dma_addr_t hi6250_plane_addr(struct drm_plane_state *st)
{
	struct drm_framebuffer *fb = st->fb;
	struct drm_gem_dma_object *obj = drm_fb_dma_get_gem_obj(fb, 0);

	return obj->dma_addr + fb->offsets[0] + (st->src.y1 >> 16) * fb->pitches[0] +
	       (st->src.x1 >> 16) * fb->format->cpp[0];
}

static int hi6250_plane_atomic_check(struct drm_plane *plane, struct drm_atomic_state *state)
{
	struct drm_plane_state *st = drm_atomic_get_new_plane_state(state, plane);
	struct drm_crtc_state *crtc_state = NULL;
	struct drm_framebuffer *fb = st->fb;
	dma_addr_t addr;
	int ret;

	if (st->crtc)
		crtc_state = drm_atomic_get_new_crtc_state(state, st->crtc);
	ret = drm_atomic_helper_check_plane_state(st, crtc_state, DRM_PLANE_NO_SCALING,
						  DRM_PLANE_NO_SCALING, false, false);
	if (ret || !st->visible)
		return ret;

	addr = hi6250_plane_addr(st);
	if (fb->pitches[0] % HI6250_DSS_ALIGN || fb->pitches[0] / HI6250_DSS_ALIGN > 0xffff ||
	    addr % HI6250_DSS_ALIGN) {
		drm_dbg_kms(plane->dev, "pitch %u / address %pad not 16-byte aligned\n",
			    fb->pitches[0], &addr);
		return -EINVAL;
	}
	if (addr + (u64)fb->pitches[0] * (drm_rect_height(&st->dst) - 1) +
	    drm_rect_width(&st->dst) * 4 > SZ_4G) {
		drm_dbg_kms(plane->dev, "buffer %pad above 4 GiB\n", &addr);
		return -EINVAL;
	}
	return 0;
}

static void hi6250_plane_atomic_update(struct drm_plane *plane, struct drm_atomic_state *state)
{
	struct hi6250_dss *dss = to_dss(plane->dev);
	struct drm_plane_state *st = drm_atomic_get_new_plane_state(state, plane);
	int idx;

	if (!st->visible || !drm_dev_enter(plane->dev, &idx))
		return;
	hi6250_dss_scanout(dss, hi6250_plane_addr(st), st->fb->pitches[0]);
	dss->flips++;
	drm_dev_exit(idx);
}

static void hi6250_plane_atomic_disable(struct drm_plane *plane, struct drm_atomic_state *state)
{
	struct hi6250_dss *dss = to_dss(plane->dev);
	int idx;

	if (!drm_dev_enter(plane->dev, &idx))
		return;
	hi6250_dss_park(dss);
	drm_dev_exit(idx);
}

static const struct drm_plane_helper_funcs hi6250_plane_helper_funcs = {
	.atomic_check = hi6250_plane_atomic_check,
	.atomic_update = hi6250_plane_atomic_update,
	.atomic_disable = hi6250_plane_atomic_disable,
};

static const struct drm_plane_funcs hi6250_plane_funcs = {
	.update_plane = drm_atomic_helper_update_plane,
	.disable_plane = drm_atomic_helper_disable_plane,
	.destroy = drm_plane_cleanup,
	.reset = drm_atomic_helper_plane_reset,
	.atomic_duplicate_state = drm_atomic_helper_plane_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_plane_destroy_state,
};

/* ------------------------------------------------------------------------------------------
 * CRTC: the fixed pipeline. Enable/disable only switch vblank handling; the LDI keeps running.
 */
static enum drm_mode_status hi6250_crtc_mode_valid(struct drm_crtc *crtc,
						   const struct drm_display_mode *mode)
{
	struct hi6250_dss *dss = to_dss(crtc->dev);

	return drm_mode_match(mode, &dss->mode, DRM_MODE_MATCH_TIMINGS | DRM_MODE_MATCH_CLOCK) ?
	       MODE_OK : MODE_BAD;
}

static int hi6250_crtc_atomic_check(struct drm_crtc *crtc, struct drm_atomic_state *state)
{
	struct drm_crtc_state *cs = drm_atomic_get_new_crtc_state(state, crtc);

	return drm_atomic_helper_check_crtc_primary_plane(cs);
}

static void hi6250_crtc_atomic_enable(struct drm_crtc *crtc, struct drm_atomic_state *state)
{
	drm_crtc_vblank_on(crtc);
}

static void hi6250_crtc_atomic_disable(struct drm_crtc *crtc, struct drm_atomic_state *state)
{
	struct hi6250_dss *dss = to_dss(crtc->dev);
	int idx;

	drm_crtc_vblank_off(crtc);
	if (drm_dev_enter(crtc->dev, &idx)) {
		hi6250_dss_park(dss);
		drm_dev_exit(idx);
	}
}

static void hi6250_crtc_atomic_flush(struct drm_crtc *crtc, struct drm_atomic_state *state)
{
	struct drm_crtc_state *cs = drm_atomic_get_new_crtc_state(state, crtc);
	struct drm_pending_vblank_event *event;

	spin_lock_irq(&crtc->dev->event_lock);
	event = cs->event;
	cs->event = NULL;
	/*
	 * The flip registers are written (atomic_update ran before this); the event goes out at
	 * the first VSYNC after now. The new address is latched at a frame start, and the old
	 * buffer is last read in the active area before that VSYNC, so it is free by then.
	 */
	if (event) {
		if (drm_crtc_vblank_get(crtc) == 0)
			drm_crtc_arm_vblank_event(crtc, event);
		else
			drm_crtc_send_vblank_event(crtc, event);
	}
	spin_unlock_irq(&crtc->dev->event_lock);
}

static const struct drm_crtc_helper_funcs hi6250_crtc_helper_funcs = {
	.mode_valid = hi6250_crtc_mode_valid,
	.atomic_check = hi6250_crtc_atomic_check,
	.atomic_flush = hi6250_crtc_atomic_flush,
	.atomic_enable = hi6250_crtc_atomic_enable,
	.atomic_disable = hi6250_crtc_atomic_disable,
	.handle_vblank_timeout = hi6250_crtc_handle_vblank_timeout,
};

static const struct drm_crtc_funcs hi6250_crtc_funcs = {
	.reset = drm_atomic_helper_crtc_reset,
	.destroy = drm_crtc_cleanup,
	.set_config = drm_atomic_helper_set_config,
	.page_flip = drm_atomic_helper_page_flip,
	.atomic_duplicate_state = drm_atomic_helper_crtc_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_crtc_destroy_state,
	.enable_vblank = hi6250_crtc_enable_vblank,
	.disable_vblank = hi6250_crtc_disable_vblank,
	.get_vblank_timestamp = hi6250_crtc_get_vblank_timestamp,
	.set_crc_source = hi6250_crtc_set_crc_source,
	.verify_crc_source = hi6250_crtc_verify_crc_source,
	.get_crc_sources = hi6250_crtc_get_crc_sources,
};

/* ------------------------------------------------------------------------------------------
 * Connector: the panel, always connected, one fixed mode
 */
static int hi6250_connector_get_modes(struct drm_connector *connector)
{
	return drm_connector_helper_get_modes_fixed(connector, &to_dss(connector->dev)->mode);
}

static const struct drm_connector_helper_funcs hi6250_connector_helper_funcs = {
	.get_modes = hi6250_connector_get_modes,
};

static const struct drm_connector_funcs hi6250_connector_funcs = {
	.reset = drm_atomic_helper_connector_reset,
	.fill_modes = drm_helper_probe_single_connector_modes,
	.atomic_duplicate_state = drm_atomic_helper_connector_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_connector_destroy_state,
};

static const struct drm_mode_config_funcs hi6250_mode_config_funcs = {
	.fb_create = drm_gem_fb_create,
	.atomic_check = drm_atomic_helper_check,
	.atomic_commit = drm_atomic_helper_commit,
};

/* ------------------------------------------------------------------------------------------
 * debugfs: dri/N/hi6250_dss (read only)
 */
static int hi6250_dss_state_show(struct seq_file *m, void *data)
{
	struct drm_debugfs_entry *entry = m->private;
	struct hi6250_dss *dss = to_dss(entry->dev);
	s64 span = ktime_us_delta(dss->last_vsync, dss->first_vsync);

	seq_printf(m, "vblank=%s irq_dead=%d vbl_on=%d crc_on=%d flush_ov0=%d\n",
		   READ_ONCE(dss->use_timer) ? "timer" : "irq", READ_ONCE(dss->irq_dead),
		   READ_ONCE(dss->vbl_on), READ_ONCE(dss->crc_on), READ_ONCE(flush_ov0));
	seq_printf(m, "irqs=%u vsyncs=%u underflows=%u flips=%u parks=%u selftest_vsyncs=%u selftest_mhz=%u\n",
		   dss->irqs, dss->vsyncs, dss->underflows, dss->flips, dss->parks,
		   dss->selftest_vsyncs, dss->selftest_mhz);
	/* VSYNCs are only counted while vblank is on: rate = delta vsyncs / delta last_vsync_us */
	seq_printf(m, "first_vsync_us=%lld last_vsync_us=%lld span_us=%lld\n",
		   ktime_to_us(dss->first_vsync), ktime_to_us(dss->last_vsync), span);
	seq_printf(m, "boot_addr=%08x cur_addr=%08x cur_pitch=%u\n", dss->boot_addr, dss->cur_addr,
		   dss->cur_pitch);
	seq_printf(m, "d0: ctrl=%08x addr0=%08x stride0=%08x ch_ctl=%08x\n",
		   dss_rd(dss, RCH_D0 + DMA_CTRL), dss_rd(dss, RCH_D0 + DMA_DATA_ADDR0),
		   dss_rd(dss, RCH_D0 + DMA_STRIDE0), dss_rd(dss, RCH_D0 + CH_CTL));
	seq_printf(m, "mctl0: en=%08x mutex=%08x mutex_rch6=%08x top=%08x flush_status=%08x status=%08x rch6_flush=%08x\n",
		   dss_rd(dss, MCTL0 + MCTL_CTL_EN), dss_rd(dss, MCTL0 + MCTL_CTL_MUTEX),
		   dss_rd(dss, MCTL0 + MCTL_CTL_MUTEX_RCH6), dss_rd(dss, MCTL0 + MCTL_CTL_TOP),
		   dss_rd(dss, MCTL0 + MCTL_CTL_FLUSH_STATUS), dss_rd(dss, MCTL0 + MCTL_CTL_STATUS),
		   dss_rd(dss, MCTL_SYS + MCTL_RCH6_FLUSH_EN));
	seq_printf(m, "glb: itf0_ints=%08x itf0_msk=%08x pdp_ints=%08x pdp_msk=%08x\n",
		   dss_rd(dss, GLB_ITF0_INTS), dss_rd(dss, GLB_ITF0_INT_MSK),
		   dss_rd(dss, GLB_PDP_INTS), dss_rd(dss, GLB_PDP_INT_MSK));
	seq_printf(m, "ldi0: ctrl=%08x vstate=%08x crc=%08x crc_en=%08x crc_frm=%08x\n",
		   dss_rd(dss, LDI0 + LDI_CTRL), dss_rd(dss, LDI0 + LDI_VSTATE),
		   dss_rd(dss, GLB_CRC_DBG_LDI0), dss_rd(dss, GLB_CRC_LDI0_EN),
		   dss_rd(dss, GLB_CRC_LDI0_FRM));
	return 0;
}

/* ------------------------------------------------------------------------------------------
 * Driver
 */
static int hi6250_dumb_create(struct drm_file *file, struct drm_device *drm,
			      struct drm_mode_create_dumb *args)
{
	int ret = drm_mode_size_dumb(drm, args, HI6250_PITCH_ALIGN, 0);

	if (ret)
		return ret;
	return drm_gem_dma_dumb_create_internal(file, drm, args);
}

DEFINE_DRM_GEM_DMA_FOPS(hi6250_fops);

static const struct drm_driver hi6250_driver = {
	.driver_features = DRIVER_GEM | DRIVER_MODESET | DRIVER_ATOMIC,
	.fops = &hi6250_fops,
	DRM_GEM_DMA_DRIVER_OPS_WITH_DUMB_CREATE(hi6250_dumb_create),
	DRM_FBDEV_DMA_DRIVER_OPS,
	.name = DRIVER_NAME,
	.desc = DRIVER_DESC,
	.major = 1,
	.minor = 0,
};

/* The pipeline must be exactly what the bootloader leaves; anything else: hands off. */
static int hi6250_dss_adopt(struct hi6250_dss *dss, struct device *dev)
{
	u32 clk, iso, ack, en, mutex6, top, d0ctrl, chctl, ldictrl, hrz0, hrz2, vrt0, vrt1, vrt2;
	u32 ovl, dfc, hact, vact, hfp, hsum, hsw, vfp, vbp, vsw;
	struct drm_display_mode *m = &dss->mode;
	int ret;

	ret = regmap_read(dss->crg, CRG_PERCLKEN3, &clk);
	ret = ret ?: regmap_read(dss->crg, CRG_ISOSTAT, &iso);
	ret = ret ?: regmap_read(dss->crg, CRG_PERPWRACK, &ack);
	if (ret)
		return dev_err_probe(dev, ret, "CRG read\n");
	if ((clk & CRG_DSS_CLKS) != CRG_DSS_CLKS || (iso & CRG_DSS_ISO) || !(ack & CRG_DSS_PWRACK)) {
		dev_info(dev, "DSS not running (perclken3 %08x isostat %08x pwrack %08x): not adopting\n",
			 clk, iso, ack);
		return -ENODEV;
	}

	en = dss_rd(dss, MCTL0 + MCTL_CTL_EN);
	mutex6 = dss_rd(dss, MCTL0 + MCTL_CTL_MUTEX_RCH6);
	top = dss_rd(dss, MCTL0 + MCTL_CTL_TOP);
	d0ctrl = dss_rd(dss, RCH_D0 + DMA_CTRL);
	chctl = dss_rd(dss, RCH_D0 + CH_CTL);
	ldictrl = dss_rd(dss, LDI0 + LDI_CTRL);
	hrz0 = dss_rd(dss, LDI0 + LDI_HRZ_CTRL0);
	hrz2 = dss_rd(dss, LDI0 + LDI_HRZ_CTRL2);
	vrt0 = dss_rd(dss, LDI0 + LDI_VRT_CTRL0);
	vrt1 = dss_rd(dss, LDI0 + LDI_VRT_CTRL1);
	vrt2 = dss_rd(dss, LDI0 + LDI_VRT_CTRL2);
	ovl = dss_rd(dss, OVL0 + OVL_SIZE);
	dfc = dss_rd(dss, RCH_D0 + DFC_DISP_SIZE);
	dss->boot_addr = dss_rd(dss, RCH_D0 + DMA_DATA_ADDR0);
	dss->boot_stride0 = dss_rd(dss, RCH_D0 + DMA_STRIDE0);

	hact = (hrz2 & 0xfff) + 1;
	vact = (vrt2 & 0xfff) + 1;
	hfp = hrz0 & 0xfff;
	hsum = ((hrz0 >> 16) & 0xfff) + 1;	/* hbp + hsw */
	vfp = vrt0 & 0xfff;
	vbp = (vrt0 >> 16) & 0xfff;
	vsw = (vrt1 & 0xfff) + 1;

	dev_info(dev, "boot pipe: mctl0 en %u mutex_rch6 %u top %u; d0 ctrl %08x ch_ctl %08x addr %08x stride0 %08x; ldi0 ctrl %08x %ux%u hfp %u hbp+hsw %u vfp %u vbp %u vsw %u\n",
		 en, mutex6, top, d0ctrl, chctl, dss->boot_addr, dss->boot_stride0, ldictrl,
		 hact, vact, hfp, hsum, vfp, vbp, vsw);

	if (en != 1 || mutex6 != 1 || top != 2 || !(chctl & CH_CTL_EN) || !(ldictrl & LDI_CTRL_EN) ||
	    (d0ctrl & (DMA_CTRL_MMU | DMA_CTRL_TILE)) ||
	    FIELD_GET(DMA_CTRL_FMT, d0ctrl) != DMA_CTRL_FMT_BOOT) {
		dev_info(dev, "D0 is not the bootloader's single linear layer on MCTL0: not adopting\n");
		return -ENODEV;
	}
	if (hact < 64 || vact < 64 || !hsum || !vsw ||
	    ovl != ((vact - 1) << 16 | (hact - 1)) || dfc != ((hact - 1) << 16 | (vact - 1)) ||
	    (dss->boot_stride0 & DMA_STRIDE0_STRIDE) != hact * 4 / HI6250_DSS_ALIGN) {
		dev_info(dev, "OVL0 %08x / DFC %08x / stride do not match LDI0 %ux%u: not adopting\n",
			 ovl, dfc, hact, vact);
		return -ENODEV;
	}

	hsw = hsum > HI6250_STOCK_HSW ? HI6250_STOCK_HSW : 1;
	m->hdisplay = hact;
	m->hsync_start = hact + hfp;
	m->hsync_end = m->hsync_start + hsw;
	m->htotal = m->hsync_end + hsum - hsw;
	m->vdisplay = vact;
	m->vsync_start = vact + vfp;
	m->vsync_end = m->vsync_start + vsw;
	m->vtotal = m->vsync_end + vbp;
	m->clock = HI6250_STOCK_PIXCLK_KHZ;
	m->flags = DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC;
	m->type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED;
	m->width_mm = 136;	/* 10.1" 16:10 panel, portrait */
	m->height_mm = 218;

	dss->cur_addr = dss->boot_addr;
	dss->cur_pitch = hact * 4;
	dss->boot_itf0_msk = dss_rd(dss, GLB_ITF0_INT_MSK);
	dss->boot_pdp_msk = dss_rd(dss, GLB_PDP_INT_MSK);
	dss->boot_crc_en = dss_rd(dss, GLB_CRC_LDI0_EN);
	return 0;
}

/* The boot buffer must be the reserved "boot" region (never freed: the parking buffer). */
static int hi6250_dss_check_boot_region(struct hi6250_dss *dss, struct device *dev)
{
	struct resource res;
	u64 fb_end = (u64)dss->boot_addr + dss->cur_pitch * dss->mode.vdisplay;
	int ret;

	ret = of_reserved_mem_region_to_resource_byname(dev->of_node, "boot", &res);
	if (ret)
		return dev_err_probe(dev, ret, "no \"boot\" memory region\n");
	if (dss->boot_addr < res.start || fb_end > (u64)res.end + 1) {
		dev_info(dev, "scanout %08x is outside the boot region %pR: not adopting\n",
			 dss->boot_addr, &res);
		return -ENODEV;
	}
	return 0;
}

/* Unmask VSYNC for a moment: is there an interrupt at the frame rate, and which rate? */
static void hi6250_dss_irq_selftest(struct hi6250_dss *dss, struct device *dev)
{
	unsigned int n, lo = HI6250_SELFTEST_MS * 30 / 1000, hi = HI6250_SELFTEST_MS * 90 / 1000;
	s64 span;

	if (force_timer) {
		dss->use_timer = true;
		dev_info(dev, "vblank: timer (force_timer)\n");
		return;
	}
	hi6250_dss_irq_unmask(dss);
	msleep(HI6250_SELFTEST_MS);
	hi6250_dss_irq_mask(dss);

	n = dss->vsyncs;
	dss->selftest_vsyncs = n;
	span = ktime_us_delta(dss->last_vsync, dss->first_vsync);
	if (n > 1 && span > 0)
		dss->selftest_mhz = div64_u64((u64)(n - 1) * 1000000000ULL, span);

	if (READ_ONCE(dss->irq_dead) || n < lo || n > hi) {
		dss->use_timer = true;
		dev_warn(dev, "VSYNC self-test: %u interrupts in %d ms (%u total, dead %d): vblank timer\n",
			 n, HI6250_SELFTEST_MS, dss->irqs, READ_ONCE(dss->irq_dead));
		return;
	}
	dev_info(dev, "VSYNC self-test: %u interrupts in %d ms, %u.%03u Hz: vblank interrupt\n",
		 n, HI6250_SELFTEST_MS, dss->selftest_mhz / 1000, dss->selftest_mhz % 1000);

	/* the LDI has no pixel clock register: derive it from the measured frame rate */
	if (n >= 8 && dss->selftest_mhz) {
		u64 khz = div64_u64((u64)dss->mode.htotal * dss->mode.vtotal * dss->selftest_mhz,
				    1000000);

		if (khz > HI6250_STOCK_PIXCLK_KHZ * 9 / 10 && khz < HI6250_STOCK_PIXCLK_KHZ * 11 / 10)
			dss->mode.clock = khz;
	}
}

static void hi6250_dss_release_pool(void *dev)
{
	of_reserved_mem_device_release(dev);
}

static int hi6250_dss_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct hi6250_dss *dss;
	struct drm_device *drm;
	struct drm_plane *primary;
	struct resource *res;
	u32 mhz;
	int ret;

	if (!enable)
		return -ENODEV;

	dss = devm_drm_dev_alloc(dev, &hi6250_driver, struct hi6250_dss, drm);
	if (IS_ERR(dss))
		return PTR_ERR(dss);
	drm = &dss->drm;
	platform_set_drvdata(pdev, dss);
	spin_lock_init(&dss->lock);
	INIT_WORK(&dss->irq_dead_work, hi6250_dss_irq_dead_work);
	dss->storm_win = jiffies;

	/* map without request_mem_region: nothing else claims the DSS, but keep it non-exclusive */
	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res)
		return -EINVAL;
	dss->base = devm_ioremap(dev, res->start, resource_size(res));
	if (!dss->base)
		return -ENOMEM;
	dss->crg = syscon_regmap_lookup_by_phandle(dev->of_node, "hisilicon,crg-syscon");
	if (IS_ERR(dss->crg))
		return dev_err_probe(dev, PTR_ERR(dss->crg), "CRG syscon\n");
	dss->irq = platform_get_irq(pdev, 0);
	if (dss->irq < 0)
		return dss->irq;

	ret = hi6250_dss_adopt(dss, dev);
	if (ret)
		return ret;
	ret = hi6250_dss_check_boot_region(dss, dev);
	if (ret)
		return ret;

	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(32));
	if (ret)
		return dev_err_probe(dev, ret, "DMA mask\n");
	ret = of_reserved_mem_device_init_by_name(dev, dev->of_node, "pool");
	if (ret)
		return dev_err_probe(dev, ret, "no \"pool\" memory region (CMA)\n");
	ret = devm_add_action_or_reset(dev, hi6250_dss_release_pool, dev);
	if (ret)
		return ret;

	/* interrupts: everything stays masked except during the self-test and while vblank is on */
	hi6250_dss_irq_mask(dss);
	dss_wr(dss, GLB_ITF0_INTS, ~0u);
	ret = devm_request_irq(dev, dss->irq, hi6250_dss_irq, 0, dev_name(dev), dss);
	if (ret)
		return dev_err_probe(dev, ret, "IRQ %d\n", dss->irq);
	hi6250_dss_irq_selftest(dss, dev);

	ret = drmm_mode_config_init(drm);
	if (ret)
		return ret;
	drm->mode_config.min_width = 1;
	drm->mode_config.min_height = 1;
	drm->mode_config.max_width = 4096;
	drm->mode_config.max_height = 4096;
	drm->mode_config.preferred_depth = 24;
	drm->mode_config.funcs = &hi6250_mode_config_funcs;

	primary = &dss->primary;
	ret = drm_universal_plane_init(drm, primary, 1, &hi6250_plane_funcs, hi6250_formats,
				       ARRAY_SIZE(hi6250_formats), NULL, DRM_PLANE_TYPE_PRIMARY,
				       "D0");
	if (ret)
		return ret;
	drm_plane_helper_add(primary, &hi6250_plane_helper_funcs);

	ret = drm_crtc_init_with_planes(drm, &dss->crtc, primary, NULL, &hi6250_crtc_funcs, "OVL0");
	if (ret)
		return ret;
	drm_crtc_helper_add(&dss->crtc, &hi6250_crtc_helper_funcs);

	dss->encoder = drmm_plain_encoder_alloc(drm, NULL, DRM_MODE_ENCODER_DSI, NULL);
	if (IS_ERR(dss->encoder))
		return PTR_ERR(dss->encoder);
	dss->encoder->possible_crtcs = drm_crtc_mask(&dss->crtc);

	ret = drmm_connector_init(drm, &dss->connector, &hi6250_connector_funcs,
				  DRM_MODE_CONNECTOR_DSI, NULL);
	if (ret)
		return ret;
	drm_connector_helper_add(&dss->connector, &hi6250_connector_helper_funcs);
	ret = drm_connector_attach_encoder(&dss->connector, dss->encoder);
	if (ret)
		return ret;

	ret = drm_vblank_init(drm, 1);
	if (ret)
		return ret;
	drm_mode_config_reset(drm);

	drm_debugfs_add_file(drm, "hi6250_dss", hi6250_dss_state_show, NULL);

	/*
	 * Point of no return for simplefb: everything above only read the pipeline. The panel
	 * keeps showing the boot buffer until the first commit.
	 */
	ret = aperture_remove_conflicting_devices(dss->boot_addr, dss->cur_pitch * dss->mode.vdisplay,
						  DRIVER_NAME);
	if (ret)
		return dev_err_probe(dev, ret, "remove the boot framebuffer\n");

	ret = drm_dev_register(drm, 0);
	if (ret)
		return ret;

	mhz = div_u64((u64)dss->mode.clock * 1000000, dss->mode.htotal * dss->mode.vtotal);
	drm_info(drm, "adopted %ux%u@%u.%03u Hz (%u kHz) on D0, boot buffer %08x, vblank %s\n",
		 dss->mode.hdisplay, dss->mode.vdisplay, mhz / 1000, mhz % 1000, dss->mode.clock,
		 dss->boot_addr, dss->use_timer ? "timer" : "irq");
	drm_client_setup(drm, NULL);
	return 0;
}

static void hi6250_dss_remove(struct platform_device *pdev)
{
	struct hi6250_dss *dss = platform_get_drvdata(pdev);

	drm_dev_unplug(&dss->drm);
	drm_atomic_helper_shutdown(&dss->drm);
	hi6250_dss_irq_mask(dss);
	hi6250_dss_park(dss);
	cancel_work_sync(&dss->irq_dead_work);
}

static void hi6250_dss_shutdown(struct platform_device *pdev)
{
	struct hi6250_dss *dss = platform_get_drvdata(pdev);

	/* no atomic commit here (it could wait for vblanks): mask, park on the boot buffer */
	if (!dss)
		return;
	hi6250_dss_irq_mask(dss);
	hi6250_dss_park(dss);
}

static const struct of_device_id hi6250_dss_of_match[] = {
	{ .compatible = "hisilicon,hi6250-dpe" },
	{ }
};
MODULE_DEVICE_TABLE(of, hi6250_dss_of_match);

static struct platform_driver hi6250_dss_driver = {
	.probe = hi6250_dss_probe,
	.remove = hi6250_dss_remove,
	.shutdown = hi6250_dss_shutdown,
	.driver = {
		.name = "hi6250-kms",
		.of_match_table = hi6250_dss_of_match,
	},
};
module_platform_driver(hi6250_dss_driver);

MODULE_DESCRIPTION(DRIVER_DESC);
MODULE_LICENSE("GPL");
