/*
 * Copyright (C) 2011 Samsung Electronics Co.Ltd
 * Authors: Joonyoung Shim <jy0922.shim@samsung.com>
 *
 * This program is free software; you can redistribute  it and/or modify it
 * under  the terms of  the GNU General  Public License as published by the
 * Free Software Foundation;  either version 2 of the  License, or (at your
 * option) any later version.
 *
 */

#include <drm/drmP.h>
#include <drm/drm_atomic.h>

#ifdef CONFIG_DMA_SHARED_BUFFER_USES_KDS
#include <linux/dma-buf.h>
#include <linux/kds.h>
#include <linux/kfifo.h>
#endif

#include <drm/exynos_drm.h>
#include "exynos_drm_drv.h"
#ifdef CONFIG_DMA_SHARED_BUFFER_USES_KDS
#include "exynos_drm_fb.h"
#include "exynos_drm_gem.h"
#endif
#include "exynos_trace.h"

/*
 * This function is to get X or Y size shown via screen. This needs length and
 * start position of CRTC.
 *
 *      <--- length --->
 * CRTC ----------------
 *      ^ start        ^ end
 *
 * There are six cases from a to f.
 *
 *             <----- SCREEN ----->
 *             0                 last
 *   ----------|------------------|----------
 * CRTCs
 * a -------
 *        b -------
 *        c --------------------------
 *                 d --------
 *                           e -------
 *                                  f -------
 */
static int exynos_plane_get_size(int start, unsigned length, unsigned last)
{
	int end = start + length;
	int size = 0;

	if (start <= 0) {
		if (end > 0)
			size = min_t(unsigned, end, last);
	} else if (start <= last) {
		size = min_t(unsigned, last - start, length);
	}

	return size;
}

void exynos_sanitize_plane_coords(struct drm_plane *plane,
		struct drm_crtc *crtc)
{
	struct exynos_drm_plane *exynos_plane = to_exynos_plane(plane);

	exynos_plane->crtc_w = exynos_plane_get_size(exynos_plane->crtc_x,
				exynos_plane->crtc_w, crtc->mode.hdisplay);
	exynos_plane->crtc_h = exynos_plane_get_size(exynos_plane->crtc_y,
				exynos_plane->crtc_h, crtc->mode.vdisplay);

	if (exynos_plane->crtc_x < 0) {
		if (exynos_plane->crtc_w > 0)
			exynos_plane->src_x -= exynos_plane->crtc_x;
		exynos_plane->crtc_x = 0;
	}

	if (exynos_plane->crtc_y < 0) {
		if (exynos_plane->crtc_h)
			exynos_plane->src_y -= exynos_plane->crtc_y;
		exynos_plane->crtc_y = 0;
	}

	exynos_plane->src_w = min(exynos_plane->src_w, exynos_plane->crtc_w);
	exynos_plane->src_h = min(exynos_plane->src_h, exynos_plane->crtc_h);
}

void exynos_plane_copy_state(struct exynos_drm_plane *src,
		struct exynos_drm_plane *dst)
{
	dst->ctx = src->ctx;
	dst->crtc_x = src->crtc_x;
	dst->crtc_y = src->crtc_y;
	dst->crtc_w = src->crtc_w;
	dst->crtc_h = src->crtc_h;
	dst->src_x = src->src_x;
	dst->src_y = src->src_y;
	dst->src_w = src->src_w;
	dst->src_h = src->src_h;
}

struct kds_callback_cookie {
	struct drm_plane *plane;
	struct drm_crtc *crtc;
	struct drm_framebuffer *fb;
};

static int exynos_plane_update(struct drm_plane *plane, struct drm_crtc *crtc,
			struct drm_framebuffer *fb,
			void (*plane_commit_cb)(void *cb, void *cb_extra))
{
	struct exynos_drm_plane *exynos_plane = to_exynos_plane(plane);
#ifdef CONFIG_DMA_SHARED_BUFFER_USES_KDS
	int ret;
	struct exynos_drm_fb *exynos_fb = to_exynos_fb(fb);
	struct exynos_drm_gem_obj *exynos_gem_obj;
	struct dma_buf *buf;
	struct kds_resource *res_list;
	struct kds_callback_cookie *cookie;
	unsigned long shared = 0UL;
#endif

	WARN_ON(!mutex_is_locked(&exynos_plane->pending_lock));

	cookie = kzalloc(sizeof(*cookie), GFP_KERNEL);
	if (!cookie) {
		DRM_ERROR("Failed to allocate kds cookie\n");
		return -ENOMEM;
	}
	cookie->plane = plane;
	cookie->crtc = crtc;
	cookie->fb = fb;

	/* This reference is released once the fb is removed from the screen */
	drm_framebuffer_reference(fb);

#ifndef CONFIG_DMA_SHARED_BUFFER_USES_KDS
	/* If we don't have kds synchronization, just put the fb on the plane */
	plane_commit_cb(cookie, NULL);
	return 0;

#else
	BUG_ON(exynos_plane->kds);

	exynos_gem_obj = exynos_drm_fb_obj(exynos_fb, 0);
	if (!exynos_gem_obj->base.dma_buf) {
		plane_commit_cb(cookie, NULL);
		return 0;
	}

	ret = kds_callback_init(&exynos_plane->kds_cb, 1, plane_commit_cb);
	if (ret) {
		DRM_ERROR("Failed to initialize kds callback ret=%d\n", ret);
		return ret;
	}

	buf = exynos_gem_obj->base.dma_buf;
	res_list = get_dma_buf_kds_resource(buf);

	exynos_drm_fb_attach_dma_buf(exynos_fb, buf);

	/* Waiting for the KDS resource*/
	trace_exynos_page_flip_state(crtc->id, DRM_BASE_ID(fb), "wait_kds");

	ret = kds_async_waitall(&exynos_plane->kds, &exynos_plane->kds_cb,
			cookie, NULL, 1, &shared, &res_list);
	if (ret) {
		DRM_ERROR("Failed kds waitall ret=%d\n", ret);
		return ret;
	}

	return 0;
#endif
}

static void exynos_drm_crtc_send_event(struct drm_plane *plane, int pipe)
{
	struct exynos_drm_plane *exynos_plane = to_exynos_plane(plane);
	struct drm_pending_vblank_event *event = exynos_plane->pending_event;
	unsigned long flags;
	struct timeval now;

	if (!exynos_plane->pending_event)
		return;

	do_gettimeofday(&now);

	spin_lock_irqsave(&plane->dev->event_lock, flags);

	event->pipe = pipe;
	event->event.sequence = 0;
	event->event.tv_sec = now.tv_sec;
	event->event.tv_usec = now.tv_usec;
	list_add_tail(&event->base.link, &event->base.file_priv->event_list);

	spin_unlock_irqrestore(&plane->dev->event_lock, flags);
	wake_up_interruptible(&event->base.file_priv->event_wait);

	exynos_plane->pending_event = NULL;
}

void exynos_plane_helper_finish_update(struct drm_plane *plane, int pipe)
{
	struct exynos_drm_plane *exynos_plane = to_exynos_plane(plane);
	struct drm_framebuffer *old_fb;

	WARN_ON(!mutex_is_locked(&exynos_plane->pending_lock));

	old_fb = exynos_plane->fb;

#ifdef CONFIG_DMA_SHARED_BUFFER_USES_KDS
	if (exynos_plane->kds) {
		kds_resource_set_release(&exynos_plane->kds);
		exynos_plane->kds = NULL;
	}

	if (exynos_plane->kds_cb.user_cb)
		kds_callback_term(&exynos_plane->kds_cb);
#endif

	exynos_plane->fb = exynos_plane->pending_fb;
	exynos_plane->pending_fb = NULL;

	if (pipe >= 0)
		exynos_drm_crtc_send_event(plane, pipe);

	if (old_fb)
		drm_framebuffer_unreference(old_fb);

	mutex_unlock(&exynos_plane->pending_lock);
}

static void exynos_plane_helper_commit_cb(void *cookie, void *unused)
{
	struct kds_callback_cookie *kds_cookie = cookie;
	struct drm_plane *plane = kds_cookie->plane;
	struct exynos_drm_plane *exynos_plane = to_exynos_plane(plane);
	struct drm_framebuffer *fb = kds_cookie->fb;
	struct drm_crtc *crtc = kds_cookie->crtc;

	WARN_ON(!mutex_is_locked(&exynos_plane->pending_lock));

	exynos_plane->helper_funcs->commit_plane(plane, crtc, fb);

	exynos_plane->pending_fb = fb;

	/* If the fb is already on the screen, finish the commit early */
	if (exynos_plane->fb == fb)
		exynos_plane_helper_finish_update(&exynos_plane->base,
			crtc->id);

	kfree(kds_cookie);
}

int exynos_plane_helper_freeze_plane(struct drm_plane *plane)
{
	struct exynos_drm_plane *exynos_plane = to_exynos_plane(plane);
	int ret;

	mutex_lock(&exynos_plane->pending_lock);

	ret = exynos_plane->helper_funcs->disable_plane(plane);

	mutex_unlock(&exynos_plane->pending_lock);

	return ret;
}

void exynos_plane_helper_thaw_plane(struct drm_plane *plane,
	struct drm_crtc *crtc)
{
	struct exynos_drm_plane *exynos_plane = to_exynos_plane(plane);
	struct drm_framebuffer *fb;

	mutex_lock(&exynos_plane->pending_lock);

	fb = exynos_plane->fb;

	/* If the plane has an fb, commit it and then set it as pending so we
	 * don't release the pending lock until it's actually up on the screen.
	 * Otherwise, it should just stay disabled and we'll release the lock
	 * immediately.
	 */
	if (fb) {
		exynos_plane->helper_funcs->commit_plane(plane, crtc, fb);

		/* Take a reference here since we'll drop it in finish_update */
		drm_framebuffer_reference(fb);

		exynos_plane->pending_fb = fb;
	} else {
		mutex_unlock(&exynos_plane->pending_lock);
	}
}

int exynos_plane_helper_update_plane(struct drm_plane *plane,
		struct drm_crtc *crtc, struct drm_framebuffer *fb, int crtc_x,
		int crtc_y, unsigned int crtc_w, unsigned int crtc_h,
		uint32_t src_x, uint32_t src_y, uint32_t src_w, uint32_t src_h)
{
	struct exynos_drm_plane *exynos_plane = to_exynos_plane(plane);
	struct exynos_drm_plane old_plane;
	int ret;

	exynos_plane_copy_state(exynos_plane, &old_plane);

	mutex_lock(&exynos_plane->pending_lock);

	/* Copy the plane parameters so we can restore it later */
	exynos_plane->crtc_x = crtc_x;
	exynos_plane->crtc_y = crtc_y;
	exynos_plane->crtc_w = crtc_w;
	exynos_plane->crtc_h = crtc_h;
	exynos_plane->src_x = src_x >> 16;
	exynos_plane->src_y = src_y >> 16;
	exynos_plane->src_w = src_w >> 16;
	exynos_plane->src_h = src_h >> 16;

	exynos_sanitize_plane_coords(plane, crtc);

	ret = exynos_plane_update(plane, crtc, fb,
			exynos_plane_helper_commit_cb);

	if (ret)
		exynos_plane_copy_state(&old_plane, exynos_plane);

	return ret;
}

int exynos_plane_helper_disable_plane(struct drm_plane *plane)
{
	struct exynos_drm_plane *exynos_plane = to_exynos_plane(plane);

	mutex_lock(&exynos_plane->pending_lock);

	/* We shouldn't have anything pending at this point */
	BUG_ON(exynos_plane->pending_fb);

	if (!exynos_plane->fb)
		goto out;

	exynos_plane->helper_funcs->disable_plane(plane);

out:
	/* Finish any updates that were unfinished and clean up references */
	exynos_plane_helper_finish_update(plane,
		plane->crtc ? plane->crtc->id : -1);

	return 0;
}
