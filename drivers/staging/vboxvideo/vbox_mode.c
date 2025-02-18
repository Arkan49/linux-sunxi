/*
 * Copyright (C) 2013-2017 Oracle Corporation
 * This file is based on ast_mode.c
 * Copyright 2012 Red Hat Inc.
 * Parts based on xf86-video-ast
 * Copyright (c) 2005 ASPEED Technology Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sub license, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
 * THE COPYRIGHT HOLDERS, AUTHORS AND/OR ITS SUPPLIERS BE LIABLE FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
 * OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
 * USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 */
/*
 * Authors: Dave Airlie <airlied@redhat.com>
 *          Michael Thayer <michael.thayer@oracle.com,
 *          Hans de Goede <hdegoede@redhat.com>
 */
#include <linux/export.h>
#include <drm/drm_crtc_helper.h>
#include <drm/drm_plane_helper.h>
#include <drm/drm_atomic_helper.h>

#include "vbox_drv.h"
#include "vboxvideo.h"
#include "hgsmi_channels.h"

/**
 * Set a graphics mode.  Poke any required values into registers, do an HGSMI
 * mode set and tell the host we support advanced graphics functions.
 */
static void vbox_do_modeset(struct drm_crtc *crtc)
{
	struct vbox_crtc *vbox_crtc = to_vbox_crtc(crtc);
	struct vbox_private *vbox;
	int width, height, bpp, pitch;
	u16 flags;
	s32 x_offset, y_offset;

	vbox = crtc->dev->dev_private;
	width = vbox_crtc->width ? vbox_crtc->width : 640;
	height = vbox_crtc->height ? vbox_crtc->height : 480;
	bpp = crtc->enabled ? CRTC_FB(crtc)->format->cpp[0] * 8 : 32;
	pitch = crtc->enabled ? CRTC_FB(crtc)->pitches[0] : width * bpp / 8;
	x_offset = vbox->single_framebuffer ? vbox_crtc->x : vbox_crtc->x_hint;
	y_offset = vbox->single_framebuffer ? vbox_crtc->y : vbox_crtc->y_hint;

	/*
	 * This is the old way of setting graphics modes.  It assumed one screen
	 * and a frame-buffer at the start of video RAM.  On older versions of
	 * VirtualBox, certain parts of the code still assume that the first
	 * screen is programmed this way, so try to fake it.
	 */
	if (vbox_crtc->crtc_id == 0 && crtc->enabled &&
	    vbox_crtc->fb_offset / pitch < 0xffff - crtc->y &&
	    vbox_crtc->fb_offset % (bpp / 8) == 0) {
		vbox_write_ioport(VBE_DISPI_INDEX_XRES, width);
		vbox_write_ioport(VBE_DISPI_INDEX_YRES, height);
		vbox_write_ioport(VBE_DISPI_INDEX_VIRT_WIDTH, pitch * 8 / bpp);
		vbox_write_ioport(VBE_DISPI_INDEX_BPP,
				  CRTC_FB(crtc)->format->cpp[0] * 8);
		vbox_write_ioport(VBE_DISPI_INDEX_ENABLE, VBE_DISPI_ENABLED);
		vbox_write_ioport(
			VBE_DISPI_INDEX_X_OFFSET,
			vbox_crtc->fb_offset % pitch / bpp * 8 + vbox_crtc->x);
		vbox_write_ioport(VBE_DISPI_INDEX_Y_OFFSET,
				  vbox_crtc->fb_offset / pitch + vbox_crtc->y);
	}

	flags = VBVA_SCREEN_F_ACTIVE;
	flags |= (crtc->enabled && !vbox_crtc->blanked) ?
		 0 : VBVA_SCREEN_F_BLANK;
	flags |= vbox_crtc->disconnected ? VBVA_SCREEN_F_DISABLED : 0;
	hgsmi_process_display_info(vbox->guest_pool, vbox_crtc->crtc_id,
				   x_offset, y_offset,
				   vbox_crtc->x * bpp / 8 +
							vbox_crtc->y * pitch,
				   pitch, width, height,
				   vbox_crtc->blanked ? 0 : bpp, flags);
}

static int vbox_set_view(struct drm_crtc *crtc)
{
	struct vbox_crtc *vbox_crtc = to_vbox_crtc(crtc);
	struct vbox_private *vbox = crtc->dev->dev_private;
	struct vbva_infoview *p;

	/*
	 * Tell the host about the view.  This design originally targeted the
	 * Windows XP driver architecture and assumed that each screen would
	 * have a dedicated frame buffer with the command buffer following it,
	 * the whole being a "view".  The host works out which screen a command
	 * buffer belongs to by checking whether it is in the first view, then
	 * whether it is in the second and so on.  The first match wins.  We
	 * cheat around this by making the first view be the managed memory
	 * plus the first command buffer, the second the same plus the second
	 * buffer and so on.
	 */
	p = hgsmi_buffer_alloc(vbox->guest_pool, sizeof(*p),
			       HGSMI_CH_VBVA, VBVA_INFO_VIEW);
	if (!p)
		return -ENOMEM;

	p->view_index = vbox_crtc->crtc_id;
	p->view_offset = vbox_crtc->fb_offset;
	p->view_size = vbox->available_vram_size - vbox_crtc->fb_offset +
		       vbox_crtc->crtc_id * VBVA_MIN_BUFFER_SIZE;
	p->max_screen_size = vbox->available_vram_size - vbox_crtc->fb_offset;

	hgsmi_buffer_submit(vbox->guest_pool, p);
	hgsmi_buffer_free(vbox->guest_pool, p);

	return 0;
}

static void vbox_crtc_dpms(struct drm_crtc *crtc, int mode)
{
	struct vbox_crtc *vbox_crtc = to_vbox_crtc(crtc);
	struct vbox_private *vbox = crtc->dev->dev_private;

	switch (mode) {
	case DRM_MODE_DPMS_ON:
		vbox_crtc->blanked = false;
		break;
	case DRM_MODE_DPMS_STANDBY:
	case DRM_MODE_DPMS_SUSPEND:
	case DRM_MODE_DPMS_OFF:
		vbox_crtc->blanked = true;
		break;
	}

	mutex_lock(&vbox->hw_mutex);
	vbox_do_modeset(crtc);
	mutex_unlock(&vbox->hw_mutex);
}

static bool vbox_crtc_mode_fixup(struct drm_crtc *crtc,
				 const struct drm_display_mode *mode,
				 struct drm_display_mode *adjusted_mode)
{
	return true;
}

/*
 * Try to map the layout of virtual screens to the range of the input device.
 * Return true if we need to re-set the crtc modes due to screen offset
 * changes.
 */
static bool vbox_set_up_input_mapping(struct vbox_private *vbox)
{
	struct drm_crtc *crtci;
	struct drm_connector *connectori;
	struct drm_framebuffer *fb1 = NULL;
	bool single_framebuffer = true;
	bool old_single_framebuffer = vbox->single_framebuffer;
	u16 width = 0, height = 0;

	/*
	 * Are we using an X.Org-style single large frame-buffer for all crtcs?
	 * If so then screen layout can be deduced from the crtc offsets.
	 * Same fall-back if this is the fbdev frame-buffer.
	 */
	list_for_each_entry(crtci, &vbox->ddev.mode_config.crtc_list, head) {
		if (!fb1) {
			fb1 = CRTC_FB(crtci);
			if (to_vbox_framebuffer(fb1) == &vbox->fbdev->afb)
				break;
		} else if (CRTC_FB(crtci) && fb1 != CRTC_FB(crtci)) {
			single_framebuffer = false;
		}
	}
	if (single_framebuffer) {
		vbox->single_framebuffer = true;
		list_for_each_entry(crtci, &vbox->ddev.mode_config.crtc_list,
				    head) {
			if (!CRTC_FB(crtci))
				continue;

			vbox->input_mapping_width = CRTC_FB(crtci)->width;
			vbox->input_mapping_height = CRTC_FB(crtci)->height;
			break;
		}
		return old_single_framebuffer != vbox->single_framebuffer;
	}
	/* Otherwise calculate the total span of all screens. */
	list_for_each_entry(connectori, &vbox->ddev.mode_config.connector_list,
			    head) {
		struct vbox_connector *vbox_connector =
		    to_vbox_connector(connectori);
		struct vbox_crtc *vbox_crtc = vbox_connector->vbox_crtc;

		width = max_t(u16, width, vbox_crtc->x_hint +
					  vbox_connector->mode_hint.width);
		height = max_t(u16, height, vbox_crtc->y_hint +
					    vbox_connector->mode_hint.height);
	}

	vbox->single_framebuffer = false;
	vbox->input_mapping_width = width;
	vbox->input_mapping_height = height;

	return old_single_framebuffer != vbox->single_framebuffer;
}

static void vbox_crtc_set_base_and_mode(struct drm_crtc *crtc,
					struct drm_framebuffer *fb,
					struct drm_display_mode *mode,
					int x, int y)
{
	struct vbox_private *vbox = crtc->dev->dev_private;
	struct vbox_crtc *vbox_crtc = to_vbox_crtc(crtc);
	struct vbox_bo *bo;

	mutex_lock(&vbox->hw_mutex);

	if (fb) {
		bo = gem_to_vbox_bo(to_vbox_framebuffer(fb)->obj);
		vbox_crtc->fb_offset = vbox_bo_gpu_offset(bo);
	}

	vbox_crtc->width = mode->hdisplay;
	vbox_crtc->height = mode->vdisplay;
	vbox_crtc->x = x;
	vbox_crtc->y = y;

	/* vbox_do_modeset() checks vbox->single_framebuffer so update it now */
	if (mode && vbox_set_up_input_mapping(vbox)) {
		struct drm_crtc *crtci;

		list_for_each_entry(crtci, &vbox->ddev.mode_config.crtc_list,
				    head) {
			if (crtci == crtc)
				continue;
			vbox_do_modeset(crtci);
		}
	}

	vbox_set_view(crtc);
	vbox_do_modeset(crtc);

	if (mode)
		hgsmi_update_input_mapping(vbox->guest_pool, 0, 0,
					   vbox->input_mapping_width,
					   vbox->input_mapping_height);

	mutex_unlock(&vbox->hw_mutex);
}

static int vbox_crtc_mode_set(struct drm_crtc *crtc,
			      struct drm_display_mode *mode,
			      struct drm_display_mode *adjusted_mode,
			      int x, int y, struct drm_framebuffer *old_fb)
{
	struct drm_framebuffer *new_fb = CRTC_FB(crtc);
	struct vbox_bo *bo = gem_to_vbox_bo(to_vbox_framebuffer(new_fb)->obj);
	int ret;

	ret = vbox_bo_pin(bo, TTM_PL_FLAG_VRAM);
	if (ret) {
		DRM_WARN("Error %d pinning new fb, out of video mem?\n", ret);
		return ret;
	}

	vbox_crtc_set_base_and_mode(crtc, new_fb, mode, x, y);

	if (old_fb) {
		bo = gem_to_vbox_bo(to_vbox_framebuffer(old_fb)->obj);
		vbox_bo_unpin(bo);
	}

	return 0;
}

static void vbox_crtc_disable(struct drm_crtc *crtc)
{
}

static void vbox_crtc_prepare(struct drm_crtc *crtc)
{
}

static void vbox_crtc_commit(struct drm_crtc *crtc)
{
}

static const struct drm_crtc_helper_funcs vbox_crtc_helper_funcs = {
	.dpms = vbox_crtc_dpms,
	.mode_fixup = vbox_crtc_mode_fixup,
	.mode_set = vbox_crtc_mode_set,
	.disable = vbox_crtc_disable,
	.prepare = vbox_crtc_prepare,
	.commit = vbox_crtc_commit,
};

static void vbox_crtc_reset(struct drm_crtc *crtc)
{
}

static void vbox_crtc_destroy(struct drm_crtc *crtc)
{
	drm_crtc_cleanup(crtc);
	kfree(crtc);
}

static const struct drm_crtc_funcs vbox_crtc_funcs = {
	.reset = vbox_crtc_reset,
	.set_config = drm_crtc_helper_set_config,
	/* .gamma_set = vbox_crtc_gamma_set, */
	.destroy = vbox_crtc_destroy,
};

static int vbox_cursor_atomic_check(struct drm_plane *plane,
				    struct drm_plane_state *new_state)
{
	u32 width = new_state->crtc_w;
	u32 height = new_state->crtc_h;

	if (width > VBOX_MAX_CURSOR_WIDTH || height > VBOX_MAX_CURSOR_HEIGHT ||
	    width == 0 || height == 0)
		return -EINVAL;

	return 0;
}

/**
 * Copy the ARGB image and generate the mask, which is needed in case the host
 * does not support ARGB cursors.  The mask is a 1BPP bitmap with the bit set
 * if the corresponding alpha value in the ARGB image is greater than 0xF0.
 */
static void copy_cursor_image(u8 *src, u8 *dst, u32 width, u32 height,
			      size_t mask_size)
{
	size_t line_size = (width + 7) / 8;
	u32 i, j;

	memcpy(dst + mask_size, src, width * height * 4);
	for (i = 0; i < height; ++i)
		for (j = 0; j < width; ++j)
			if (((u32 *)src)[i * width + j] > 0xf0000000)
				dst[i * line_size + j / 8] |= (0x80 >> (j % 8));
}

static void vbox_cursor_atomic_update(struct drm_plane *plane,
				      struct drm_plane_state *old_state)
{
	struct vbox_private *vbox =
		container_of(plane->dev, struct vbox_private, ddev);
	struct vbox_crtc *vbox_crtc = to_vbox_crtc(plane->state->crtc);
	struct drm_framebuffer *fb = plane->state->fb;
	struct vbox_bo *bo = gem_to_vbox_bo(to_vbox_framebuffer(fb)->obj);
	u32 width = plane->state->crtc_w;
	u32 height = plane->state->crtc_h;
	size_t data_size, mask_size;
	u32 flags;
	u8 *src;

	/*
	 * VirtualBox uses the host windowing system to draw the cursor so
	 * moves are a no-op, we only need to upload new cursor sprites.
	 */
	if (fb == old_state->fb)
		return;

	mutex_lock(&vbox->hw_mutex);

	vbox_crtc->cursor_enabled = true;

	/* pinning is done in prepare/cleanup framebuffer */
	src = vbox_bo_kmap(bo);
	if (IS_ERR(src)) {
		DRM_WARN("Could not kmap cursor bo, skipping update\n");
		return;
	}

	/*
	 * The mask must be calculated based on the alpha
	 * channel, one bit per ARGB word, and must be 32-bit
	 * padded.
	 */
	mask_size = ((width + 7) / 8 * height + 3) & ~3;
	data_size = width * height * 4 + mask_size;

	copy_cursor_image(src, vbox->cursor_data, width, height, mask_size);
	vbox_bo_kunmap(bo);

	flags = VBOX_MOUSE_POINTER_VISIBLE | VBOX_MOUSE_POINTER_SHAPE |
		VBOX_MOUSE_POINTER_ALPHA;
	hgsmi_update_pointer_shape(vbox->guest_pool, flags,
				   min_t(u32, max(fb->hot_x, 0), width),
				   min_t(u32, max(fb->hot_y, 0), height),
				   width, height, vbox->cursor_data, data_size);

	mutex_unlock(&vbox->hw_mutex);
}

void vbox_cursor_atomic_disable(struct drm_plane *plane,
				struct drm_plane_state *old_state)
{
	struct vbox_private *vbox =
		container_of(plane->dev, struct vbox_private, ddev);
	struct vbox_crtc *vbox_crtc = to_vbox_crtc(old_state->crtc);
	bool cursor_enabled = false;
	struct drm_crtc *crtci;

	mutex_lock(&vbox->hw_mutex);

	vbox_crtc->cursor_enabled = false;

	list_for_each_entry(crtci, &vbox->ddev.mode_config.crtc_list, head) {
		if (to_vbox_crtc(crtci)->cursor_enabled)
			cursor_enabled = true;
	}

	if (!cursor_enabled)
		hgsmi_update_pointer_shape(vbox->guest_pool, 0, 0, 0,
					   0, 0, NULL, 0);

	mutex_unlock(&vbox->hw_mutex);
}

static int vbox_cursor_prepare_fb(struct drm_plane *plane,
				  struct drm_plane_state *new_state)
{
	struct vbox_bo *bo;

	if (!new_state->fb)
		return 0;

	bo = gem_to_vbox_bo(to_vbox_framebuffer(new_state->fb)->obj);
	return vbox_bo_pin(bo, TTM_PL_FLAG_SYSTEM);
}

static void vbox_cursor_cleanup_fb(struct drm_plane *plane,
				   struct drm_plane_state *old_state)
{
	struct vbox_bo *bo;

	if (!plane->state->fb)
		return;

	bo = gem_to_vbox_bo(to_vbox_framebuffer(plane->state->fb)->obj);
	vbox_bo_unpin(bo);
}

static const uint32_t vbox_cursor_plane_formats[] = {
	DRM_FORMAT_ARGB8888,
};

static const struct drm_plane_helper_funcs vbox_cursor_helper_funcs = {
	.atomic_check	= vbox_cursor_atomic_check,
	.atomic_update	= vbox_cursor_atomic_update,
	.atomic_disable	= vbox_cursor_atomic_disable,
	.prepare_fb	= vbox_cursor_prepare_fb,
	.cleanup_fb	= vbox_cursor_cleanup_fb,
};

static const struct drm_plane_funcs vbox_cursor_plane_funcs = {
	.update_plane	= drm_plane_helper_update,
	.disable_plane	= drm_plane_helper_disable,
	.destroy	= drm_primary_helper_destroy,
};

static const uint32_t vbox_primary_plane_formats[] = {
	DRM_FORMAT_XRGB8888,
	DRM_FORMAT_ARGB8888,
};

static const struct drm_plane_funcs vbox_primary_plane_funcs = {
	.update_plane	= drm_primary_helper_update,
	.disable_plane	= drm_primary_helper_disable,
	.destroy	= drm_primary_helper_destroy,
};

static struct drm_plane *vbox_create_plane(struct vbox_private *vbox,
					   unsigned int possible_crtcs,
					   enum drm_plane_type type)
{
	const struct drm_plane_helper_funcs *helper_funcs = NULL;
	const struct drm_plane_funcs *funcs;
	struct drm_plane *plane;
	const uint32_t *formats;
	int num_formats;
	int err;

	if (type == DRM_PLANE_TYPE_PRIMARY) {
		funcs = &vbox_primary_plane_funcs;
		formats = vbox_primary_plane_formats;
		num_formats = ARRAY_SIZE(vbox_primary_plane_formats);
	} else if (type == DRM_PLANE_TYPE_CURSOR) {
		funcs = &vbox_cursor_plane_funcs;
		formats = vbox_cursor_plane_formats;
		helper_funcs = &vbox_cursor_helper_funcs;
		num_formats = ARRAY_SIZE(vbox_cursor_plane_formats);
	} else {
		return ERR_PTR(-EINVAL);
	}

	plane = kzalloc(sizeof(*plane), GFP_KERNEL);
	if (!plane)
		return ERR_PTR(-ENOMEM);

	err = drm_universal_plane_init(&vbox->ddev, plane, possible_crtcs,
				       funcs, formats, num_formats,
				       NULL, type, NULL);
	if (err)
		goto free_plane;

	drm_plane_helper_add(plane, helper_funcs);

	return plane;

free_plane:
	kfree(plane);
	return ERR_PTR(-EINVAL);
}

static struct vbox_crtc *vbox_crtc_init(struct drm_device *dev, unsigned int i)
{
	struct vbox_private *vbox =
		container_of(dev, struct vbox_private, ddev);
	struct drm_plane *cursor = NULL;
	struct vbox_crtc *vbox_crtc;
	struct drm_plane *primary;
	u32 caps = 0;
	int ret;

	ret = hgsmi_query_conf(vbox->guest_pool,
			       VBOX_VBVA_CONF32_CURSOR_CAPABILITIES, &caps);
	if (ret)
		return ERR_PTR(ret);

	vbox_crtc = kzalloc(sizeof(*vbox_crtc), GFP_KERNEL);
	if (!vbox_crtc)
		return ERR_PTR(-ENOMEM);

	primary = vbox_create_plane(vbox, 1 << i, DRM_PLANE_TYPE_PRIMARY);
	if (IS_ERR(primary)) {
		ret = PTR_ERR(primary);
		goto free_mem;
	}

	if ((caps & VBOX_VBVA_CURSOR_CAPABILITY_HARDWARE)) {
		cursor = vbox_create_plane(vbox, 1 << i, DRM_PLANE_TYPE_CURSOR);
		if (IS_ERR(cursor)) {
			ret = PTR_ERR(cursor);
			goto clean_primary;
		}
	} else {
		DRM_WARN("VirtualBox host is too old, no cursor support\n");
	}

	vbox_crtc->crtc_id = i;

	ret = drm_crtc_init_with_planes(dev, &vbox_crtc->base, primary, cursor,
					&vbox_crtc_funcs, NULL);
	if (ret)
		goto clean_cursor;

	drm_mode_crtc_set_gamma_size(&vbox_crtc->base, 256);
	drm_crtc_helper_add(&vbox_crtc->base, &vbox_crtc_helper_funcs);

	return vbox_crtc;

clean_cursor:
	if (cursor) {
		drm_plane_cleanup(cursor);
		kfree(cursor);
	}
clean_primary:
	drm_plane_cleanup(primary);
	kfree(primary);
free_mem:
	kfree(vbox_crtc);
	return ERR_PTR(ret);
}

static void vbox_encoder_destroy(struct drm_encoder *encoder)
{
	drm_encoder_cleanup(encoder);
	kfree(encoder);
}

static struct drm_encoder *vbox_best_single_encoder(struct drm_connector
						    *connector)
{
	int enc_id = connector->encoder_ids[0];

	/* pick the encoder ids */
	if (enc_id)
		return drm_encoder_find(connector->dev, NULL, enc_id);

	return NULL;
}

static const struct drm_encoder_funcs vbox_enc_funcs = {
	.destroy = vbox_encoder_destroy,
};

static struct drm_encoder *vbox_encoder_init(struct drm_device *dev,
					     unsigned int i)
{
	struct vbox_encoder *vbox_encoder;

	vbox_encoder = kzalloc(sizeof(*vbox_encoder), GFP_KERNEL);
	if (!vbox_encoder)
		return NULL;

	drm_encoder_init(dev, &vbox_encoder->base, &vbox_enc_funcs,
			 DRM_MODE_ENCODER_DAC, NULL);

	vbox_encoder->base.possible_crtcs = 1 << i;
	return &vbox_encoder->base;
}

/**
 * Generate EDID data with a mode-unique serial number for the virtual
 *  monitor to try to persuade Unity that different modes correspond to
 *  different monitors and it should not try to force the same resolution on
 *  them.
 */
static void vbox_set_edid(struct drm_connector *connector, int width,
			  int height)
{
	enum { EDID_SIZE = 128 };
	unsigned char edid[EDID_SIZE] = {
		0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00,	/* header */
		0x58, 0x58,	/* manufacturer (VBX) */
		0x00, 0x00,	/* product code */
		0x00, 0x00, 0x00, 0x00,	/* serial number goes here */
		0x01,		/* week of manufacture */
		0x00,		/* year of manufacture */
		0x01, 0x03,	/* EDID version */
		0x80,		/* capabilities - digital */
		0x00,		/* horiz. res in cm, zero for projectors */
		0x00,		/* vert. res in cm */
		0x78,		/* display gamma (120 == 2.2). */
		0xEE,		/* features (standby, suspend, off, RGB, std */
				/* colour space, preferred timing mode) */
		0xEE, 0x91, 0xA3, 0x54, 0x4C, 0x99, 0x26, 0x0F, 0x50, 0x54,
		/* chromaticity for standard colour space. */
		0x00, 0x00, 0x00,	/* no default timings */
		0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
		    0x01, 0x01,
		0x01, 0x01, 0x01, 0x01,	/* no standard timings */
		0x00, 0x00, 0x00, 0x06, 0x00, 0x00, 0x06, 0x00, 0x02, 0x02,
		    0x02, 0x02,
		/* descriptor block 1 goes below */
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		/* descriptor block 2, monitor ranges */
		0x00, 0x00, 0x00, 0xFD, 0x00,
		0x00, 0xC8, 0x00, 0xC8, 0x64, 0x00, 0x0A, 0x20, 0x20, 0x20,
		    0x20, 0x20,
		/* 0-200Hz vertical, 0-200KHz horizontal, 1000MHz pixel clock */
		0x20,
		/* descriptor block 3, monitor name */
		0x00, 0x00, 0x00, 0xFC, 0x00,
		'V', 'B', 'O', 'X', ' ', 'm', 'o', 'n', 'i', 't', 'o', 'r',
		'\n',
		/* descriptor block 4: dummy data */
		0x00, 0x00, 0x00, 0x10, 0x00,
		0x0A, 0x20, 0x20, 0x20, 0x20, 0x20,
		0x20, 0x20, 0x20, 0x20, 0x20, 0x20,
		0x20,
		0x00,		/* number of extensions */
		0x00		/* checksum goes here */
	};
	int clock = (width + 6) * (height + 6) * 60 / 10000;
	unsigned int i, sum = 0;

	edid[12] = width & 0xff;
	edid[13] = width >> 8;
	edid[14] = height & 0xff;
	edid[15] = height >> 8;
	edid[54] = clock & 0xff;
	edid[55] = clock >> 8;
	edid[56] = width & 0xff;
	edid[58] = (width >> 4) & 0xf0;
	edid[59] = height & 0xff;
	edid[61] = (height >> 4) & 0xf0;
	for (i = 0; i < EDID_SIZE - 1; ++i)
		sum += edid[i];
	edid[EDID_SIZE - 1] = (0x100 - (sum & 0xFF)) & 0xFF;
	drm_connector_update_edid_property(connector, (struct edid *)edid);
}

static int vbox_get_modes(struct drm_connector *connector)
{
	struct vbox_connector *vbox_connector = NULL;
	struct drm_display_mode *mode = NULL;
	struct vbox_private *vbox = NULL;
	unsigned int num_modes = 0;
	int preferred_width, preferred_height;

	vbox_connector = to_vbox_connector(connector);
	vbox = connector->dev->dev_private;
	/*
	 * Heuristic: we do not want to tell the host that we support dynamic
	 * resizing unless we feel confident that the user space client using
	 * the video driver can handle hot-plug events.  So the first time modes
	 * are queried after a "master" switch we tell the host that we do not,
	 * and immediately after we send the client a hot-plug notification as
	 * a test to see if they will respond and query again.
	 * That is also the reason why capabilities are reported to the host at
	 * this place in the code rather than elsewhere.
	 * We need to report the flags location before reporting the IRQ
	 * capability.
	 */
	hgsmi_report_flags_location(vbox->guest_pool, GUEST_HEAP_OFFSET(vbox) +
				    HOST_FLAGS_OFFSET);
	if (vbox_connector->vbox_crtc->crtc_id == 0)
		vbox_report_caps(vbox);
	if (!vbox->initial_mode_queried) {
		if (vbox_connector->vbox_crtc->crtc_id == 0) {
			vbox->initial_mode_queried = true;
			vbox_report_hotplug(vbox);
		}
		return drm_add_modes_noedid(connector, 800, 600);
	}
	num_modes = drm_add_modes_noedid(connector, 2560, 1600);
	preferred_width = vbox_connector->mode_hint.width ?
			  vbox_connector->mode_hint.width : 1024;
	preferred_height = vbox_connector->mode_hint.height ?
			   vbox_connector->mode_hint.height : 768;
	mode = drm_cvt_mode(connector->dev, preferred_width, preferred_height,
			    60, false, false, false);
	if (mode) {
		mode->type |= DRM_MODE_TYPE_PREFERRED;
		drm_mode_probed_add(connector, mode);
		++num_modes;
	}
	vbox_set_edid(connector, preferred_width, preferred_height);

	if (vbox_connector->vbox_crtc->x_hint != -1)
		drm_object_property_set_value(&connector->base,
			vbox->ddev.mode_config.suggested_x_property,
			vbox_connector->vbox_crtc->x_hint);
	else
		drm_object_property_set_value(&connector->base,
			vbox->ddev.mode_config.suggested_x_property, 0);

	if (vbox_connector->vbox_crtc->y_hint != -1)
		drm_object_property_set_value(&connector->base,
			vbox->ddev.mode_config.suggested_y_property,
			vbox_connector->vbox_crtc->y_hint);
	else
		drm_object_property_set_value(&connector->base,
			vbox->ddev.mode_config.suggested_y_property, 0);

	return num_modes;
}

static enum drm_mode_status vbox_mode_valid(struct drm_connector *connector,
			   struct drm_display_mode *mode)
{
	return MODE_OK;
}

static void vbox_connector_destroy(struct drm_connector *connector)
{
	drm_connector_unregister(connector);
	drm_connector_cleanup(connector);
	kfree(connector);
}

static enum drm_connector_status
vbox_connector_detect(struct drm_connector *connector, bool force)
{
	struct vbox_connector *vbox_connector;

	vbox_connector = to_vbox_connector(connector);

	return vbox_connector->mode_hint.disconnected ?
	    connector_status_disconnected : connector_status_connected;
}

static int vbox_fill_modes(struct drm_connector *connector, u32 max_x,
			   u32 max_y)
{
	struct vbox_connector *vbox_connector;
	struct drm_device *dev;
	struct drm_display_mode *mode, *iterator;

	vbox_connector = to_vbox_connector(connector);
	dev = vbox_connector->base.dev;
	list_for_each_entry_safe(mode, iterator, &connector->modes, head) {
		list_del(&mode->head);
		drm_mode_destroy(dev, mode);
	}

	return drm_helper_probe_single_connector_modes(connector, max_x, max_y);
}

static const struct drm_connector_helper_funcs vbox_connector_helper_funcs = {
	.mode_valid = vbox_mode_valid,
	.get_modes = vbox_get_modes,
	.best_encoder = vbox_best_single_encoder,
};

static const struct drm_connector_funcs vbox_connector_funcs = {
	.dpms = drm_helper_connector_dpms,
	.detect = vbox_connector_detect,
	.fill_modes = vbox_fill_modes,
	.destroy = vbox_connector_destroy,
};

static int vbox_connector_init(struct drm_device *dev,
			       struct vbox_crtc *vbox_crtc,
			       struct drm_encoder *encoder)
{
	struct vbox_connector *vbox_connector;
	struct drm_connector *connector;

	vbox_connector = kzalloc(sizeof(*vbox_connector), GFP_KERNEL);
	if (!vbox_connector)
		return -ENOMEM;

	connector = &vbox_connector->base;
	vbox_connector->vbox_crtc = vbox_crtc;

	drm_connector_init(dev, connector, &vbox_connector_funcs,
			   DRM_MODE_CONNECTOR_VGA);
	drm_connector_helper_add(connector, &vbox_connector_helper_funcs);

	connector->interlace_allowed = 0;
	connector->doublescan_allowed = 0;

	drm_mode_create_suggested_offset_properties(dev);
	drm_object_attach_property(&connector->base,
				   dev->mode_config.suggested_x_property, 0);
	drm_object_attach_property(&connector->base,
				   dev->mode_config.suggested_y_property, 0);

	drm_connector_attach_encoder(connector, encoder);

	return 0;
}

static struct drm_framebuffer *vbox_user_framebuffer_create(
		struct drm_device *dev,
		struct drm_file *filp,
		const struct drm_mode_fb_cmd2 *mode_cmd)
{
	struct vbox_private *vbox =
		container_of(dev, struct vbox_private, ddev);
	struct drm_gem_object *obj;
	struct vbox_framebuffer *vbox_fb;
	int ret = -ENOMEM;

	obj = drm_gem_object_lookup(filp, mode_cmd->handles[0]);
	if (!obj)
		return ERR_PTR(-ENOENT);

	vbox_fb = kzalloc(sizeof(*vbox_fb), GFP_KERNEL);
	if (!vbox_fb)
		goto err_unref_obj;

	ret = vbox_framebuffer_init(vbox, vbox_fb, mode_cmd, obj);
	if (ret)
		goto err_free_vbox_fb;

	return &vbox_fb->base;

err_free_vbox_fb:
	kfree(vbox_fb);
err_unref_obj:
	drm_gem_object_put_unlocked(obj);
	return ERR_PTR(ret);
}

static const struct drm_mode_config_funcs vbox_mode_funcs = {
	.fb_create = vbox_user_framebuffer_create,
};

int vbox_mode_init(struct vbox_private *vbox)
{
	struct drm_device *dev = &vbox->ddev;
	struct drm_encoder *encoder;
	struct vbox_crtc *vbox_crtc;
	unsigned int i;
	int ret;

	drm_mode_config_init(dev);

	dev->mode_config.funcs = (void *)&vbox_mode_funcs;
	dev->mode_config.min_width = 0;
	dev->mode_config.min_height = 0;
	dev->mode_config.preferred_depth = 24;
	dev->mode_config.max_width = VBE_DISPI_MAX_XRES;
	dev->mode_config.max_height = VBE_DISPI_MAX_YRES;

	for (i = 0; i < vbox->num_crtcs; ++i) {
		vbox_crtc = vbox_crtc_init(dev, i);
		if (IS_ERR(vbox_crtc)) {
			ret = PTR_ERR(vbox_crtc);
			goto err_drm_mode_cleanup;
		}
		encoder = vbox_encoder_init(dev, i);
		if (!encoder) {
			ret = -ENOMEM;
			goto err_drm_mode_cleanup;
		}
		ret = vbox_connector_init(dev, vbox_crtc, encoder);
		if (ret)
			goto err_drm_mode_cleanup;
	}

	return 0;

err_drm_mode_cleanup:
	drm_mode_config_cleanup(dev);
	return ret;
}

void vbox_mode_fini(struct vbox_private *vbox)
{
	drm_mode_config_cleanup(&vbox->ddev);
}
