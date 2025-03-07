// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2015 Broadcom
 */

/**
 * DOC: VC4 KMS
 *
 * This is the general code for implementing KMS mode setting that
 * doesn't clearly associate with any of the other objects (plane,
 * crtc, HDMI encoder).
 */

#include <linux/clk.h>

#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_crtc.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_plane_helper.h>
#include <drm/drm_probe_helper.h>
#include <drm/drm_vblank.h>

#include "vc4_drv.h"
#include "vc4_regs.h"

#define HVS_NUM_CHANNELS 3

struct vc4_ctm_state {
	struct drm_private_state base;
	struct drm_color_ctm *ctm;
	int fifo;
};

static struct vc4_ctm_state *to_vc4_ctm_state(struct drm_private_state *priv)
{
	return container_of(priv, struct vc4_ctm_state, base);
}

struct vc4_hvs_state {
	struct drm_private_state base;
	unsigned int unassigned_channels;
	unsigned int num_outputs;
	unsigned long fifo_load;
	unsigned long core_clock_rate;
};

static struct vc4_hvs_state *
to_vc4_hvs_state(struct drm_private_state *priv)
{
	return container_of(priv, struct vc4_hvs_state, base);
}

struct vc4_load_tracker_state {
	struct drm_private_state base;
	u64 hvs_load;
	u64 membus_load;
};

static struct vc4_load_tracker_state *
to_vc4_load_tracker_state(struct drm_private_state *priv)
{
	return container_of(priv, struct vc4_load_tracker_state, base);
}

static struct vc4_ctm_state *vc4_get_ctm_state(struct drm_atomic_state *state,
					       struct drm_private_obj *manager)
{
	struct drm_device *dev = state->dev;
	struct vc4_dev *vc4 = to_vc4_dev(dev);
	struct drm_private_state *priv_state;
	int ret;

	ret = drm_modeset_lock(&vc4->ctm_state_lock, state->acquire_ctx);
	if (ret)
		return ERR_PTR(ret);

	priv_state = drm_atomic_get_private_obj_state(state, manager);
	if (IS_ERR(priv_state))
		return ERR_CAST(priv_state);

	return to_vc4_ctm_state(priv_state);
}

static struct drm_private_state *
vc4_ctm_duplicate_state(struct drm_private_obj *obj)
{
	struct vc4_ctm_state *state;

	state = kmemdup(obj->state, sizeof(*state), GFP_KERNEL);
	if (!state)
		return NULL;

	__drm_atomic_helper_private_obj_duplicate_state(obj, &state->base);

	return &state->base;
}

static void vc4_ctm_destroy_state(struct drm_private_obj *obj,
				  struct drm_private_state *state)
{
	struct vc4_ctm_state *ctm_state = to_vc4_ctm_state(state);

	kfree(ctm_state);
}

static const struct drm_private_state_funcs vc4_ctm_state_funcs = {
	.atomic_duplicate_state = vc4_ctm_duplicate_state,
	.atomic_destroy_state = vc4_ctm_destroy_state,
};

static void vc4_ctm_obj_fini(struct drm_device *dev, void *unused)
{
	struct vc4_dev *vc4 = to_vc4_dev(dev);

	drm_atomic_private_obj_fini(&vc4->ctm_manager);
}

static int vc4_ctm_obj_init(struct vc4_dev *vc4)
{
	struct vc4_ctm_state *ctm_state;

	drm_modeset_lock_init(&vc4->ctm_state_lock);

	ctm_state = kzalloc(sizeof(*ctm_state), GFP_KERNEL);
	if (!ctm_state)
		return -ENOMEM;

	drm_atomic_private_obj_init(&vc4->base, &vc4->ctm_manager, &ctm_state->base,
				    &vc4_ctm_state_funcs);

	return drmm_add_action_or_reset(&vc4->base, vc4_ctm_obj_fini, NULL);
}

/* Converts a DRM S31.32 value to the HW S0.9 format. */
static u16 vc4_ctm_s31_32_to_s0_9(u64 in)
{
	u16 r;

	/* Sign bit. */
	r = in & BIT_ULL(63) ? BIT(9) : 0;

	if ((in & GENMASK_ULL(62, 32)) > 0) {
		/* We have zero integer bits so we can only saturate here. */
		r |= GENMASK(8, 0);
	} else {
		/* Otherwise take the 9 most important fractional bits. */
		r |= (in >> 23) & GENMASK(8, 0);
	}

	return r;
}

static void
vc4_ctm_commit(struct vc4_dev *vc4, struct drm_atomic_state *state)
{
	struct vc4_ctm_state *ctm_state = to_vc4_ctm_state(vc4->ctm_manager.state);
	struct drm_color_ctm *ctm = ctm_state->ctm;

	if (vc4->firmware_kms)
		return;

	if (ctm_state->fifo) {
		HVS_WRITE(SCALER_OLEDCOEF2,
			  VC4_SET_FIELD(vc4_ctm_s31_32_to_s0_9(ctm->matrix[0]),
					SCALER_OLEDCOEF2_R_TO_R) |
			  VC4_SET_FIELD(vc4_ctm_s31_32_to_s0_9(ctm->matrix[3]),
					SCALER_OLEDCOEF2_R_TO_G) |
			  VC4_SET_FIELD(vc4_ctm_s31_32_to_s0_9(ctm->matrix[6]),
					SCALER_OLEDCOEF2_R_TO_B));
		HVS_WRITE(SCALER_OLEDCOEF1,
			  VC4_SET_FIELD(vc4_ctm_s31_32_to_s0_9(ctm->matrix[1]),
					SCALER_OLEDCOEF1_G_TO_R) |
			  VC4_SET_FIELD(vc4_ctm_s31_32_to_s0_9(ctm->matrix[4]),
					SCALER_OLEDCOEF1_G_TO_G) |
			  VC4_SET_FIELD(vc4_ctm_s31_32_to_s0_9(ctm->matrix[7]),
					SCALER_OLEDCOEF1_G_TO_B));
		HVS_WRITE(SCALER_OLEDCOEF0,
			  VC4_SET_FIELD(vc4_ctm_s31_32_to_s0_9(ctm->matrix[2]),
					SCALER_OLEDCOEF0_B_TO_R) |
			  VC4_SET_FIELD(vc4_ctm_s31_32_to_s0_9(ctm->matrix[5]),
					SCALER_OLEDCOEF0_B_TO_G) |
			  VC4_SET_FIELD(vc4_ctm_s31_32_to_s0_9(ctm->matrix[8]),
					SCALER_OLEDCOEF0_B_TO_B));
	}

	HVS_WRITE(SCALER_OLEDOFFS,
		  VC4_SET_FIELD(ctm_state->fifo, SCALER_OLEDOFFS_DISPFIFO));
}

static struct vc4_hvs_state *
vc4_hvs_get_new_global_state(struct drm_atomic_state *state)
{
	struct vc4_dev *vc4 = to_vc4_dev(state->dev);
	struct drm_private_state *priv_state;

	priv_state = drm_atomic_get_new_private_obj_state(state, &vc4->hvs_channels);
	if (IS_ERR(priv_state))
		return ERR_CAST(priv_state);

	return to_vc4_hvs_state(priv_state);
}

static struct vc4_hvs_state *
vc4_hvs_get_global_state(struct drm_atomic_state *state)
{
	struct vc4_dev *vc4 = to_vc4_dev(state->dev);
	struct drm_private_state *priv_state;

	priv_state = drm_atomic_get_private_obj_state(state, &vc4->hvs_channels);
	if (IS_ERR(priv_state))
		return ERR_CAST(priv_state);

	return to_vc4_hvs_state(priv_state);
}

static void vc4_hvs_pv_muxing_commit(struct vc4_dev *vc4,
				     struct drm_atomic_state *state)
{
	struct drm_crtc_state *crtc_state;
	struct drm_crtc *crtc;
	unsigned int i;

	for_each_new_crtc_in_state(state, crtc, crtc_state, i) {
		struct vc4_crtc_state *vc4_state = to_vc4_crtc_state(crtc_state);
		u32 dispctrl;
		u32 dsp3_mux;

		if (!crtc_state->active)
			continue;

		if (vc4_state->assigned_channel != 2)
			continue;

		/*
		 * SCALER_DISPCTRL_DSP3 = X, where X < 2 means 'connect DSP3 to
		 * FIFO X'.
		 * SCALER_DISPCTRL_DSP3 = 3 means 'disable DSP 3'.
		 *
		 * DSP3 is connected to FIFO2 unless the transposer is
		 * enabled. In this case, FIFO 2 is directly accessed by the
		 * TXP IP, and we need to disable the FIFO2 -> pixelvalve1
		 * route.
		 */
		if (vc4_state->feed_txp)
			dsp3_mux = VC4_SET_FIELD(3, SCALER_DISPCTRL_DSP3_MUX);
		else
			dsp3_mux = VC4_SET_FIELD(2, SCALER_DISPCTRL_DSP3_MUX);

		dispctrl = HVS_READ(SCALER_DISPCTRL) &
			   ~SCALER_DISPCTRL_DSP3_MUX_MASK;
		HVS_WRITE(SCALER_DISPCTRL, dispctrl | dsp3_mux);
	}
}

static void vc5_hvs_pv_muxing_commit(struct vc4_dev *vc4,
				     struct drm_atomic_state *state)
{
	struct drm_crtc_state *crtc_state;
	struct drm_crtc *crtc;
	unsigned char mux;
	unsigned int i;
	u32 reg;

	for_each_new_crtc_in_state(state, crtc, crtc_state, i) {
		struct vc4_crtc_state *vc4_state = to_vc4_crtc_state(crtc_state);
		struct vc4_crtc *vc4_crtc = to_vc4_crtc(crtc);

		if (!vc4_state->update_muxing)
			continue;

		switch (vc4_crtc->data->hvs_output) {
		case 2:
			mux = (vc4_state->assigned_channel == 2) ? 0 : 1;
			reg = HVS_READ(SCALER_DISPECTRL);
			HVS_WRITE(SCALER_DISPECTRL,
				  (reg & ~SCALER_DISPECTRL_DSP2_MUX_MASK) |
				  VC4_SET_FIELD(mux, SCALER_DISPECTRL_DSP2_MUX));
			break;

		case 3:
			if (vc4_state->assigned_channel == VC4_HVS_CHANNEL_DISABLED)
				mux = 3;
			else
				mux = vc4_state->assigned_channel;

			reg = HVS_READ(SCALER_DISPCTRL);
			HVS_WRITE(SCALER_DISPCTRL,
				  (reg & ~SCALER_DISPCTRL_DSP3_MUX_MASK) |
				  VC4_SET_FIELD(mux, SCALER_DISPCTRL_DSP3_MUX));
			break;

		case 4:
			if (vc4_state->assigned_channel == VC4_HVS_CHANNEL_DISABLED)
				mux = 3;
			else
				mux = vc4_state->assigned_channel;

			reg = HVS_READ(SCALER_DISPEOLN);
			HVS_WRITE(SCALER_DISPEOLN,
				  (reg & ~SCALER_DISPEOLN_DSP4_MUX_MASK) |
				  VC4_SET_FIELD(mux, SCALER_DISPEOLN_DSP4_MUX));

			break;

		case 5:
			if (vc4_state->assigned_channel == VC4_HVS_CHANNEL_DISABLED)
				mux = 3;
			else
				mux = vc4_state->assigned_channel;

			reg = HVS_READ(SCALER_DISPDITHER);
			HVS_WRITE(SCALER_DISPDITHER,
				  (reg & ~SCALER_DISPDITHER_DSP5_MUX_MASK) |
				  VC4_SET_FIELD(mux, SCALER_DISPDITHER_DSP5_MUX));
			break;

		default:
			break;
		}
	}
}

static void
vc4_atomic_complete_commit(struct drm_atomic_state *state)
{
	struct drm_device *dev = state->dev;
	struct vc4_dev *vc4 = to_vc4_dev(dev);
	struct vc4_hvs *hvs = vc4->hvs;
	struct drm_crtc_state *new_crtc_state;
	struct vc4_hvs_state *hvs_state;
	struct drm_crtc *crtc;
	struct clk_request *core_req;
	int i;

	hvs_state = vc4_hvs_get_new_global_state(state);
	if (WARN_ON(!hvs_state))
		return;

	for_each_new_crtc_in_state(state, crtc, new_crtc_state, i) {
		struct vc4_crtc_state *vc4_crtc_state;

		if (!new_crtc_state->commit || vc4->firmware_kms)
			continue;

		vc4_crtc_state = to_vc4_crtc_state(new_crtc_state);
		vc4_hvs_mask_underrun(dev, vc4_crtc_state->assigned_channel);
	}

	if (vc4->hvs && vc4->hvs->hvs5) {
		unsigned long core_rate = max_t(unsigned long,
						500000000,
						hvs_state->core_clock_rate);

		drm_dbg(dev, "Raising the core clock at %lu Hz\n", core_rate);

		/*
		 * Do a temporary request on the core clock during the
		 * modeset.
		 */
		core_req = clk_request_start(hvs->core_clk, core_rate);

		/*
		 * And remove the previous one based on the HVS
		 * requirements if any.
		 */
		clk_request_done(hvs->core_req);
	}

	drm_atomic_helper_wait_for_fences(dev, state, false);

	drm_atomic_helper_wait_for_dependencies(state);

	drm_atomic_helper_commit_modeset_disables(dev, state);

	vc4_ctm_commit(vc4, state);

	if (!vc4->firmware_kms) {
		if (vc4->hvs->hvs5)
			vc5_hvs_pv_muxing_commit(vc4, state);
		else
			vc4_hvs_pv_muxing_commit(vc4, state);
	}

	drm_atomic_helper_commit_planes(dev, state, 0);

	drm_atomic_helper_commit_modeset_enables(dev, state);

	drm_atomic_helper_fake_vblank(state);

	drm_atomic_helper_commit_hw_done(state);

	drm_atomic_helper_wait_for_flip_done(dev, state);

	drm_atomic_helper_cleanup_planes(dev, state);

	drm_atomic_helper_commit_cleanup_done(state);

	if (vc4->hvs && vc4->hvs->hvs5) {
		drm_dbg(dev, "Running the core clock at %lu Hz\n",
			hvs_state->core_clock_rate);

		/*
		 * Request a clock rate based on the current HVS
		 * requirements.
		 */
		hvs->core_req = clk_request_start(hvs->core_clk,
						  hvs_state->core_clock_rate);

		/* And drop the temporary request */
		clk_request_done(core_req);
	}

	drm_atomic_state_put(state);

	up(&vc4->async_modeset);
}

static void commit_work(struct work_struct *work)
{
	struct drm_atomic_state *state = container_of(work,
						      struct drm_atomic_state,
						      commit_work);
	vc4_atomic_complete_commit(state);
}

/**
 * vc4_atomic_commit - commit validated state object
 * @dev: DRM device
 * @state: the driver state object
 * @nonblock: nonblocking commit
 *
 * This function commits a with drm_atomic_helper_check() pre-validated state
 * object. This can still fail when e.g. the framebuffer reservation fails. For
 * now this doesn't implement asynchronous commits.
 *
 * RETURNS
 * Zero for success or -errno.
 */
static int vc4_atomic_commit(struct drm_device *dev,
			     struct drm_atomic_state *state,
			     bool nonblock)
{
	struct vc4_dev *vc4 = to_vc4_dev(dev);
	int ret;

	if (state->async_update) {
		ret = down_interruptible(&vc4->async_modeset);
		if (ret)
			return ret;

		ret = drm_atomic_helper_prepare_planes(dev, state);
		if (ret) {
			up(&vc4->async_modeset);
			return ret;
		}

		drm_atomic_helper_async_commit(dev, state);

		drm_atomic_helper_cleanup_planes(dev, state);

		up(&vc4->async_modeset);

		return 0;
	}

	/* We know for sure we don't want an async update here. Set
	 * state->legacy_cursor_update to false to prevent
	 * drm_atomic_helper_setup_commit() from auto-completing
	 * commit->flip_done.
	 */
	if (!vc4->firmware_kms)
		state->legacy_cursor_update = false;
	ret = drm_atomic_helper_setup_commit(state, nonblock);
	if (ret)
		return ret;

	INIT_WORK(&state->commit_work, commit_work);

	ret = down_interruptible(&vc4->async_modeset);
	if (ret)
		return ret;

	ret = drm_atomic_helper_prepare_planes(dev, state);
	if (ret) {
		up(&vc4->async_modeset);
		return ret;
	}

	if (!nonblock) {
		ret = drm_atomic_helper_wait_for_fences(dev, state, true);
		if (ret) {
			drm_atomic_helper_cleanup_planes(dev, state);
			up(&vc4->async_modeset);
			return ret;
		}
	}

	/*
	 * This is the point of no return - everything below never fails except
	 * when the hw goes bonghits. Which means we can commit the new state on
	 * the software side now.
	 */

	BUG_ON(drm_atomic_helper_swap_state(state, false) < 0);

	/*
	 * Everything below can be run asynchronously without the need to grab
	 * any modeset locks at all under one condition: It must be guaranteed
	 * that the asynchronous work has either been cancelled (if the driver
	 * supports it, which at least requires that the framebuffers get
	 * cleaned up with drm_atomic_helper_cleanup_planes()) or completed
	 * before the new state gets committed on the software side with
	 * drm_atomic_helper_swap_state().
	 *
	 * This scheme allows new atomic state updates to be prepared and
	 * checked in parallel to the asynchronous completion of the previous
	 * update. Which is important since compositors need to figure out the
	 * composition of the next frame right after having submitted the
	 * current layout.
	 */

	drm_atomic_state_get(state);
	if (nonblock)
		queue_work(system_unbound_wq, &state->commit_work);
	else
		vc4_atomic_complete_commit(state);

	return 0;
}

static struct drm_framebuffer *vc4_fb_create(struct drm_device *dev,
					     struct drm_file *file_priv,
					     const struct drm_mode_fb_cmd2 *mode_cmd)
{
	struct drm_mode_fb_cmd2 mode_cmd_local;

	/* If the user didn't specify a modifier, use the
	 * vc4_set_tiling_ioctl() state for the BO.
	 */
	if (!(mode_cmd->flags & DRM_MODE_FB_MODIFIERS)) {
		struct drm_gem_object *gem_obj;
		struct vc4_bo *bo;

		gem_obj = drm_gem_object_lookup(file_priv,
						mode_cmd->handles[0]);
		if (!gem_obj) {
			DRM_DEBUG("Failed to look up GEM BO %d\n",
				  mode_cmd->handles[0]);
			return ERR_PTR(-ENOENT);
		}
		bo = to_vc4_bo(gem_obj);

		mode_cmd_local = *mode_cmd;

		if (bo->t_format) {
			mode_cmd_local.modifier[0] =
				DRM_FORMAT_MOD_BROADCOM_VC4_T_TILED;
		} else {
			mode_cmd_local.modifier[0] = DRM_FORMAT_MOD_NONE;
		}

		drm_gem_object_put(gem_obj);

		mode_cmd = &mode_cmd_local;
	}

	return drm_gem_fb_create(dev, file_priv, mode_cmd);
}

/* Our CTM has some peculiar limitations: we can only enable it for one CRTC
 * at a time and the HW only supports S0.9 scalars. To account for the latter,
 * we don't allow userland to set a CTM that we have no hope of approximating.
 */
static int
vc4_ctm_atomic_check(struct drm_device *dev, struct drm_atomic_state *state)
{
	struct vc4_dev *vc4 = to_vc4_dev(dev);
	struct vc4_ctm_state *ctm_state = NULL;
	struct drm_crtc *crtc;
	struct drm_crtc_state *old_crtc_state, *new_crtc_state;
	struct drm_color_ctm *ctm;
	int i;

	for_each_oldnew_crtc_in_state(state, crtc, old_crtc_state, new_crtc_state, i) {
		/* CTM is being disabled. */
		if (!new_crtc_state->ctm && old_crtc_state->ctm) {
			ctm_state = vc4_get_ctm_state(state, &vc4->ctm_manager);
			if (IS_ERR(ctm_state))
				return PTR_ERR(ctm_state);
			ctm_state->fifo = 0;
		}
	}

	for_each_oldnew_crtc_in_state(state, crtc, old_crtc_state, new_crtc_state, i) {
		if (new_crtc_state->ctm == old_crtc_state->ctm)
			continue;

		if (!ctm_state) {
			ctm_state = vc4_get_ctm_state(state, &vc4->ctm_manager);
			if (IS_ERR(ctm_state))
				return PTR_ERR(ctm_state);
		}

		/* CTM is being enabled or the matrix changed. */
		if (new_crtc_state->ctm) {
			struct vc4_crtc_state *vc4_crtc_state =
				to_vc4_crtc_state(new_crtc_state);

			/* fifo is 1-based since 0 disables CTM. */
			int fifo = vc4_crtc_state->assigned_channel + 1;

			/* Check userland isn't trying to turn on CTM for more
			 * than one CRTC at a time.
			 */
			if (ctm_state->fifo && ctm_state->fifo != fifo) {
				DRM_DEBUG_DRIVER("Too many CTM configured\n");
				return -EINVAL;
			}

			/* Check we can approximate the specified CTM.
			 * We disallow scalars |c| > 1.0 since the HW has
			 * no integer bits.
			 */
			ctm = new_crtc_state->ctm->data;
			for (i = 0; i < ARRAY_SIZE(ctm->matrix); i++) {
				u64 val = ctm->matrix[i];

				val &= ~BIT_ULL(63);
				if (val > BIT_ULL(32))
					return -EINVAL;
			}

			ctm_state->fifo = fifo;
			ctm_state->ctm = ctm;
		}
	}

	return 0;
}

static int vc4_load_tracker_atomic_check(struct drm_atomic_state *state)
{
	struct drm_plane_state *old_plane_state, *new_plane_state;
	struct vc4_dev *vc4 = to_vc4_dev(state->dev);
	struct vc4_load_tracker_state *load_state;
	struct drm_private_state *priv_state;
	struct drm_plane *plane;
	int i;

	priv_state = drm_atomic_get_private_obj_state(state,
						      &vc4->load_tracker);
	if (IS_ERR(priv_state))
		return PTR_ERR(priv_state);

	load_state = to_vc4_load_tracker_state(priv_state);
	for_each_oldnew_plane_in_state(state, plane, old_plane_state,
				       new_plane_state, i) {
		struct vc4_plane_state *vc4_plane_state;

		if (old_plane_state->fb && old_plane_state->crtc) {
			vc4_plane_state = to_vc4_plane_state(old_plane_state);
			load_state->membus_load -= vc4_plane_state->membus_load;
			load_state->hvs_load -= vc4_plane_state->hvs_load;
		}

		if (new_plane_state->fb && new_plane_state->crtc) {
			vc4_plane_state = to_vc4_plane_state(new_plane_state);
			load_state->membus_load += vc4_plane_state->membus_load;
			load_state->hvs_load += vc4_plane_state->hvs_load;
		}
	}

	/* Don't check the load when the tracker is disabled. */
	if (!vc4->load_tracker_enabled)
		return 0;

	/* The absolute limit is 2Gbyte/sec, but let's take a margin to let
	 * the system work when other blocks are accessing the memory.
	 */
	if (load_state->membus_load > SZ_1G + SZ_512M)
		return -ENOSPC;

	/* HVS clock is supposed to run @ 250Mhz, let's take a margin and
	 * consider the maximum number of cycles is 240M.
	 */
	if (load_state->hvs_load > 240000000ULL)
		return -ENOSPC;

	return 0;
}

static struct drm_private_state *
vc4_load_tracker_duplicate_state(struct drm_private_obj *obj)
{
	struct vc4_load_tracker_state *state;

	state = kmemdup(obj->state, sizeof(*state), GFP_KERNEL);
	if (!state)
		return NULL;

	__drm_atomic_helper_private_obj_duplicate_state(obj, &state->base);

	return &state->base;
}

static void vc4_load_tracker_destroy_state(struct drm_private_obj *obj,
					   struct drm_private_state *state)
{
	struct vc4_load_tracker_state *load_state;

	load_state = to_vc4_load_tracker_state(state);
	kfree(load_state);
}

static const struct drm_private_state_funcs vc4_load_tracker_state_funcs = {
	.atomic_duplicate_state = vc4_load_tracker_duplicate_state,
	.atomic_destroy_state = vc4_load_tracker_destroy_state,
};

static void vc4_load_tracker_obj_fini(struct drm_device *dev, void *unused)
{
	struct vc4_dev *vc4 = to_vc4_dev(dev);

	drm_atomic_private_obj_fini(&vc4->load_tracker);
}

static int vc4_load_tracker_obj_init(struct vc4_dev *vc4)
{
	struct vc4_load_tracker_state *load_state;

	load_state = kzalloc(sizeof(*load_state), GFP_KERNEL);
	if (!load_state)
		return -ENOMEM;

	drm_atomic_private_obj_init(&vc4->base, &vc4->load_tracker,
				    &load_state->base,
				    &vc4_load_tracker_state_funcs);

	return drmm_add_action_or_reset(&vc4->base, vc4_load_tracker_obj_fini, NULL);
}

static struct drm_private_state *
vc4_hvs_channels_duplicate_state(struct drm_private_obj *obj)
{
	struct vc4_hvs_state *old_state = to_vc4_hvs_state(obj->state);
	struct vc4_hvs_state *state;

	state = kzalloc(sizeof(*state), GFP_KERNEL);
	if (!state)
		return NULL;

	__drm_atomic_helper_private_obj_duplicate_state(obj, &state->base);

	state->unassigned_channels = old_state->unassigned_channels;
	state->fifo_load = old_state->fifo_load;
	state->num_outputs = old_state->num_outputs;
	state->core_clock_rate = old_state->core_clock_rate;

	return &state->base;
}

static void vc4_hvs_channels_destroy_state(struct drm_private_obj *obj,
					   struct drm_private_state *state)
{
	struct vc4_hvs_state *hvs_state = to_vc4_hvs_state(state);

	kfree(hvs_state);
}

static const struct drm_private_state_funcs vc4_hvs_state_funcs = {
	.atomic_duplicate_state = vc4_hvs_channels_duplicate_state,
	.atomic_destroy_state = vc4_hvs_channels_destroy_state,
};

static void vc4_hvs_channels_obj_fini(struct drm_device *dev, void *unused)
{
	struct vc4_dev *vc4 = to_vc4_dev(dev);

	drm_atomic_private_obj_fini(&vc4->hvs_channels);
}

static int vc4_hvs_channels_obj_init(struct vc4_dev *vc4)
{
	struct vc4_hvs_state *state;

	state = kzalloc(sizeof(*state), GFP_KERNEL);
	if (!state)
		return -ENOMEM;

	state->unassigned_channels = GENMASK(HVS_NUM_CHANNELS - 1, 0);
	drm_atomic_private_obj_init(&vc4->base, &vc4->hvs_channels,
				    &state->base,
				    &vc4_hvs_state_funcs);

	return drmm_add_action_or_reset(&vc4->base, vc4_hvs_channels_obj_fini, NULL);
}

/*
 * The BCM2711 HVS has up to 7 outputs connected to the pixelvalves and
 * the TXP (and therefore all the CRTCs found on that platform).
 *
 * The naive (and our initial) implementation would just iterate over
 * all the active CRTCs, try to find a suitable FIFO, and then remove it
 * from the pool of available FIFOs. However, there are a few corner
 * cases that need to be considered:
 *
 * - When running in a dual-display setup (so with two CRTCs involved),
 *   we can update the state of a single CRTC (for example by changing
 *   its mode using xrandr under X11) without affecting the other. In
 *   this case, the other CRTC wouldn't be in the state at all, so we
 *   need to consider all the running CRTCs in the DRM device to assign
 *   a FIFO, not just the one in the state.
 *
 * - To fix the above, we can't use drm_atomic_get_crtc_state on all
 *   enabled CRTCs to pull their CRTC state into the global state, since
 *   a page flip would start considering their vblank to complete. Since
 *   we don't have a guarantee that they are actually active, that
 *   vblank might never happen, and shouldn't even be considered if we
 *   want to do a page flip on a single CRTC. That can be tested by
 *   doing a modetest -v first on HDMI1 and then on HDMI0.
 *
 * - Since we need the pixelvalve to be disabled and enabled back when
 *   the FIFO is changed, we should keep the FIFO assigned for as long
 *   as the CRTC is enabled, only considering it free again once that
 *   CRTC has been disabled. This can be tested by booting X11 on a
 *   single display, and changing the resolution down and then back up.
 */
static int vc4_pv_muxing_atomic_check(struct drm_device *dev,
				      struct drm_atomic_state *state)
{
	struct vc4_dev *vc4 = to_vc4_dev(state->dev);
	struct vc4_hvs_state *hvs_new_state;
	struct drm_crtc_state *old_crtc_state, *new_crtc_state;
	struct drm_crtc *crtc;
	unsigned int i;

	hvs_new_state = vc4_hvs_get_global_state(state);
	if (!hvs_new_state)
		return -EINVAL;

	for_each_oldnew_crtc_in_state(state, crtc, old_crtc_state, new_crtc_state, i) {
		struct vc4_crtc_state *old_vc4_crtc_state =
			to_vc4_crtc_state(old_crtc_state);
		struct vc4_crtc_state *new_vc4_crtc_state =
			to_vc4_crtc_state(new_crtc_state);
		struct vc4_crtc *vc4_crtc = to_vc4_crtc(crtc);
		unsigned int matching_channels;

		if (vc4->firmware_kms)
			continue;

		/* Nothing to do here, let's skip it */
		if (old_crtc_state->enable == new_crtc_state->enable)
			continue;

		/* Muxing will need to be modified, mark it as such */
		new_vc4_crtc_state->update_muxing = true;

		/* If we're disabling our CRTC, we put back our channel */
		if (!new_crtc_state->enable) {
			hvs_new_state->unassigned_channels |= BIT(old_vc4_crtc_state->assigned_channel);
			new_vc4_crtc_state->assigned_channel = VC4_HVS_CHANNEL_DISABLED;
			continue;
		}

		/*
		 * The problem we have to solve here is that we have
		 * up to 7 encoders, connected to up to 6 CRTCs.
		 *
		 * Those CRTCs, depending on the instance, can be
		 * routed to 1, 2 or 3 HVS FIFOs, and we need to set
		 * the change the muxing between FIFOs and outputs in
		 * the HVS accordingly.
		 *
		 * It would be pretty hard to come up with an
		 * algorithm that would generically solve
		 * this. However, the current routing trees we support
		 * allow us to simplify a bit the problem.
		 *
		 * Indeed, with the current supported layouts, if we
		 * try to assign in the ascending crtc index order the
		 * FIFOs, we can't fall into the situation where an
		 * earlier CRTC that had multiple routes is assigned
		 * one that was the only option for a later CRTC.
		 *
		 * If the layout changes and doesn't give us that in
		 * the future, we will need to have something smarter,
		 * but it works so far.
		 */
		matching_channels = hvs_new_state->unassigned_channels & vc4_crtc->data->hvs_available_channels;
		if (matching_channels) {
			unsigned int channel = ffs(matching_channels) - 1;

			new_vc4_crtc_state->assigned_channel = channel;
			hvs_new_state->unassigned_channels &= ~BIT(channel);
		} else {
			return -EINVAL;
		}
	}

	return 0;
}

static int
vc4_core_clock_atomic_check(struct drm_atomic_state *state)
{
	struct vc4_dev *vc4 = to_vc4_dev(state->dev);
	struct drm_private_state *priv_state;
	struct vc4_hvs_state *hvs_new_state;
	struct vc4_load_tracker_state *load_state;
	struct drm_crtc_state *old_crtc_state, *new_crtc_state;
	struct drm_crtc *crtc;
	unsigned long pixel_rate;
	unsigned long cob_rate;
	unsigned int i;

	priv_state = drm_atomic_get_private_obj_state(state,
						      &vc4->load_tracker);
	if (IS_ERR(priv_state))
		return PTR_ERR(priv_state);

	load_state = to_vc4_load_tracker_state(priv_state);

	hvs_new_state = vc4_hvs_get_global_state(state);
	if (!hvs_new_state)
		return -EINVAL;

	for_each_oldnew_crtc_in_state(state, crtc,
				      old_crtc_state,
				      new_crtc_state,
				      i) {
		if (old_crtc_state->active) {
			struct vc4_crtc_state *old_vc4_state =
				to_vc4_crtc_state(old_crtc_state);

			hvs_new_state->num_outputs -= 1;
			hvs_new_state->fifo_load -= old_vc4_state->hvs_load;
		}

		if (new_crtc_state->active) {
			struct vc4_crtc_state *new_vc4_state =
				to_vc4_crtc_state(new_crtc_state);

			hvs_new_state->num_outputs += 1;
			hvs_new_state->fifo_load += new_vc4_state->hvs_load;
		}
	}

	cob_rate = hvs_new_state->fifo_load;
	pixel_rate = load_state->hvs_load;
	if (hvs_new_state->num_outputs > 1) {
		pixel_rate = (pixel_rate * 40) / 100;
	} else {
		pixel_rate = (pixel_rate * 60) / 100;
	}

	hvs_new_state->core_clock_rate = max(cob_rate, pixel_rate);

	return 0;
}


static int
vc4_atomic_check(struct drm_device *dev, struct drm_atomic_state *state)
{
	int ret;

	ret = vc4_pv_muxing_atomic_check(dev, state);
	if (ret)
		return ret;

	ret = vc4_ctm_atomic_check(dev, state);
	if (ret < 0)
		return ret;

	ret = drm_atomic_helper_check(dev, state);
	if (ret)
		return ret;

	ret = vc4_load_tracker_atomic_check(state);
	if (ret)
		return ret;

	return vc4_core_clock_atomic_check(state);
}

static const struct drm_mode_config_funcs vc4_mode_funcs = {
	.atomic_check = vc4_atomic_check,
	.atomic_commit = vc4_atomic_commit,
	.fb_create = vc4_fb_create,
};

int vc4_kms_load(struct drm_device *dev)
{
	struct vc4_dev *vc4 = to_vc4_dev(dev);
	bool is_vc5 = of_device_is_compatible(dev->dev->of_node,
					      "brcm,bcm2711-vc5");
	int ret;

	/*
	 * The limits enforced by the load tracker aren't relevant for
	 * the BCM2711, but the load tracker computations are used for
	 * the core clock rate calculation.
	 */
	if (!is_vc5) {
		/* Start with the load tracker enabled. Can be
		 * disabled through the debugfs load_tracker file.
		 */
		vc4->load_tracker_enabled = true;
	}

	sema_init(&vc4->async_modeset, 1);

	/* Set support for vblank irq fast disable, before drm_vblank_init() */
	dev->vblank_disable_immediate = true;

	dev->irq_enabled = true;
	ret = drm_vblank_init(dev, dev->mode_config.num_crtc);
	if (ret < 0) {
		dev_err(dev->dev, "failed to initialize vblank\n");
		return ret;
	}

	if (is_vc5) {
		dev->mode_config.max_width = 7680;
		dev->mode_config.max_height = 7680;
	} else {
		dev->mode_config.max_width = 2048;
		dev->mode_config.max_height = 2048;
	}

	dev->mode_config.funcs = &vc4_mode_funcs;
	dev->mode_config.preferred_depth = 24;
	dev->mode_config.async_page_flip = true;
	dev->mode_config.allow_fb_modifiers = true;
	if (vc4->firmware_kms)
		dev->mode_config.normalize_zpos = true;

	ret = vc4_ctm_obj_init(vc4);
	if (ret)
		return ret;

	ret = vc4_load_tracker_obj_init(vc4);
	if (ret)
		return ret;

	ret = vc4_hvs_channels_obj_init(vc4);
	if (ret)
		return ret;

	drm_mode_config_reset(dev);

	drm_kms_helper_poll_init(dev);

	return 0;
}
