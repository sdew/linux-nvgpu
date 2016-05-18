/*
 * drivers/video/tegra/host/gk20a/channel_sync_gk20a.c
 *
 * GK20A Channel Synchronization Abstraction
 *
 * Copyright (c) 2014-2016, NVIDIA CORPORATION.  All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 */

#include <linux/gk20a.h>
#include <linux/version.h>

#include "channel_sync_gk20a.h"
#include "gk20a.h"
#include "fence_gk20a.h"
#include "semaphore_gk20a.h"
#include "sync_gk20a.h"
#include "mm_gk20a.h"

#ifdef CONFIG_SYNC
#include "../drivers/staging/android/sync.h"
#endif

#ifdef CONFIG_TEGRA_GK20A
#include <linux/nvhost.h>
#endif

#ifdef CONFIG_TEGRA_GK20A

struct gk20a_channel_syncpt {
	struct gk20a_channel_sync ops;
	struct channel_gk20a *c;
	struct platform_device *host1x_pdev;
	u32 id;
};

static void add_wait_cmd(struct gk20a *g, struct priv_cmd_entry *cmd, u32 off,
		u32 id, u32 thresh)
{
	off = cmd->off + off;
	/* syncpoint_a */
	gk20a_mem_wr32(g, cmd->mem, off++, 0x2001001C);
	/* payload */
	gk20a_mem_wr32(g, cmd->mem, off++, thresh);
	/* syncpoint_b */
	gk20a_mem_wr32(g, cmd->mem, off++, 0x2001001D);
	/* syncpt_id, switch_en, wait */
	gk20a_mem_wr32(g, cmd->mem, off++, (id << 8) | 0x10);
}

static int gk20a_channel_syncpt_wait_syncpt(struct gk20a_channel_sync *s,
		u32 id, u32 thresh, struct priv_cmd_entry **entry,
		struct gk20a_fence **fence)
{
	struct gk20a_channel_syncpt *sp =
		container_of(s, struct gk20a_channel_syncpt, ops);
	struct priv_cmd_entry *wait_cmd = NULL;
	struct channel_gk20a *c = sp->c;
	int err = 0;

	if (!nvhost_syncpt_is_valid_pt_ext(sp->host1x_pdev, id)) {
		dev_warn(dev_from_gk20a(c->g),
				"invalid wait id in gpfifo submit, elided");
		return 0;
	}

	if (nvhost_syncpt_is_expired_ext(sp->host1x_pdev, id, thresh))
		return 0;

	err = gk20a_channel_alloc_priv_cmdbuf(c, 4, &wait_cmd);
	if (err) {
		gk20a_err(dev_from_gk20a(c->g),
				"not enough priv cmd buffer space");
		return err;
	}

	add_wait_cmd(c->g, wait_cmd, 0, id, thresh);

	*entry = wait_cmd;
	*fence = NULL;
	return 0;
}

static int gk20a_channel_syncpt_wait_fd(struct gk20a_channel_sync *s, int fd,
		       struct priv_cmd_entry **entry,
		       struct gk20a_fence **fence)
{
#ifdef CONFIG_SYNC
	int i;
	int num_wait_cmds;
	struct sync_fence *sync_fence;
	struct sync_pt *pt;
	struct priv_cmd_entry *wait_cmd = NULL;
	struct gk20a_channel_syncpt *sp =
		container_of(s, struct gk20a_channel_syncpt, ops);
	struct channel_gk20a *c = sp->c;
	u32 wait_id;
	int err = 0;

	sync_fence = nvhost_sync_fdget(fd);
	if (!sync_fence)
		return -EINVAL;

	/* validate syncpt ids */
#if LINUX_VERSION_CODE < KERNEL_VERSION(3,18,0)
	list_for_each_entry(pt, &sync_fence->pt_list_head, pt_list) {
#else
	for (i = 0; i < sync_fence->num_fences; i++) {
		pt = sync_pt_from_fence(sync_fence->cbs[i].sync_pt);
#endif
		wait_id = nvhost_sync_pt_id(pt);
		if (!wait_id || !nvhost_syncpt_is_valid_pt_ext(sp->host1x_pdev,
					wait_id)) {
			sync_fence_put(sync_fence);
			return -EINVAL;
		}
	}

	num_wait_cmds = nvhost_sync_num_pts(sync_fence);
	if (num_wait_cmds == 0) {
		sync_fence_put(sync_fence);
		return 0;
	}

	err = gk20a_channel_alloc_priv_cmdbuf(c, 4 * num_wait_cmds, &wait_cmd);
	if (err) {
		gk20a_err(dev_from_gk20a(c->g),
				"not enough priv cmd buffer space");
		sync_fence_put(sync_fence);
		return err;
	}

	i = 0;
#if LINUX_VERSION_CODE < KERNEL_VERSION(3,18,0)
	list_for_each_entry(pt, &sync_fence->pt_list_head, pt_list) {
#else
	for (i = 0; i < sync_fence->num_fences; i++) {
		struct fence *f = sync_fence->cbs[i].sync_pt;
		struct sync_pt *pt = sync_pt_from_fence(f);
#endif
		u32 wait_id = nvhost_sync_pt_id(pt);
		u32 wait_value = nvhost_sync_pt_thresh(pt);

		if (nvhost_syncpt_is_expired_ext(sp->host1x_pdev,
				wait_id, wait_value)) {
			/* each wait_cmd is 4 u32s */
			gk20a_memset(c->g, wait_cmd->mem,
					(wait_cmd->off + i * 4) * sizeof(u32),
					0, 4 * sizeof(u32));
		} else
			add_wait_cmd(c->g, wait_cmd, i * 4, wait_id,
					wait_value);
#if LINUX_VERSION_CODE < KERNEL_VERSION(3,18,0)
		i++;
#endif
	}
	WARN_ON(i != num_wait_cmds);
	sync_fence_put(sync_fence);

	*entry = wait_cmd;
	*fence = NULL;
	return 0;
#else
	return -ENODEV;
#endif
}

static void gk20a_channel_syncpt_update(void *priv, int nr_completed)
{
	struct channel_gk20a *ch = priv;

	gk20a_channel_update(ch, nr_completed);

	/* note: channel_get() is in __gk20a_channel_syncpt_incr() */
	gk20a_channel_put(ch);
}

static int __gk20a_channel_syncpt_incr(struct gk20a_channel_sync *s,
				       bool wfi_cmd,
				       bool register_irq,
				       struct priv_cmd_entry **entry,
				       struct gk20a_fence **fence,
				       bool need_sync_fence)
{
	u32 thresh;
	int incr_cmd_size;
	int off;
	int err;
	struct priv_cmd_entry *incr_cmd = NULL;
	struct gk20a_channel_syncpt *sp =
		container_of(s, struct gk20a_channel_syncpt, ops);
	struct channel_gk20a *c = sp->c;

	incr_cmd_size = 6;
	if (wfi_cmd)
		incr_cmd_size += 2;

	err = gk20a_channel_alloc_priv_cmdbuf(c, incr_cmd_size, &incr_cmd);
	if (err) {
		gk20a_err(dev_from_gk20a(c->g),
				"not enough priv cmd buffer space");
		return err;
	}

	off = incr_cmd->off;

	/* WAR for hw bug 1491360: syncpt needs to be incremented twice */

	if (wfi_cmd) {
		/* wfi */
		gk20a_mem_wr32(c->g, incr_cmd->mem, off++, 0x2001001E);
		/* handle, ignored */
		gk20a_mem_wr32(c->g, incr_cmd->mem, off++, 0x00000000);
	}
	/* syncpoint_a */
	gk20a_mem_wr32(c->g, incr_cmd->mem, off++, 0x2001001C);
	/* payload, ignored */
	gk20a_mem_wr32(c->g, incr_cmd->mem, off++, 0);
	/* syncpoint_b */
	gk20a_mem_wr32(c->g, incr_cmd->mem, off++, 0x2001001D);
	/* syncpt_id, incr */
	gk20a_mem_wr32(c->g, incr_cmd->mem, off++, (sp->id << 8) | 0x1);
	/* syncpoint_b */
	gk20a_mem_wr32(c->g, incr_cmd->mem, off++, 0x2001001D);
	/* syncpt_id, incr */
	gk20a_mem_wr32(c->g, incr_cmd->mem, off++, (sp->id << 8) | 0x1);

	WARN_ON(off - incr_cmd->off != incr_cmd_size);

	thresh = nvhost_syncpt_incr_max_ext(sp->host1x_pdev, sp->id, 2);

	if (register_irq) {
		struct channel_gk20a *referenced = gk20a_channel_get(c);

		WARN_ON(!referenced);

		if (referenced) {
			/* note: channel_put() is in
			 * gk20a_channel_syncpt_update() */

			err = nvhost_intr_register_notifier(
				sp->host1x_pdev,
				sp->id, thresh,
				gk20a_channel_syncpt_update, c);
			if (err)
				gk20a_channel_put(referenced);

			/* Adding interrupt action should
			 * never fail. A proper error handling
			 * here would require us to decrement
			 * the syncpt max back to its original
			 * value. */
			WARN(err,
			     "failed to set submit complete interrupt");
		}
	}

	*fence = gk20a_fence_from_syncpt(sp->host1x_pdev, sp->id, thresh,
					 wfi_cmd, need_sync_fence);
	*entry = incr_cmd;
	return 0;
}

static int gk20a_channel_syncpt_incr_wfi(struct gk20a_channel_sync *s,
				  struct priv_cmd_entry **entry,
				  struct gk20a_fence **fence)
{
	return __gk20a_channel_syncpt_incr(s,
			true /* wfi */,
			false /* no irq handler */,
			entry, fence, true);
}

static int gk20a_channel_syncpt_incr(struct gk20a_channel_sync *s,
			      struct priv_cmd_entry **entry,
			      struct gk20a_fence **fence,
			      bool need_sync_fence)
{
	/* Don't put wfi cmd to this one since we're not returning
	 * a fence to user space. */
	return __gk20a_channel_syncpt_incr(s,
			false /* no wfi */,
			true /* register irq */,
			entry, fence, need_sync_fence);
}

static int gk20a_channel_syncpt_incr_user(struct gk20a_channel_sync *s,
				   int wait_fence_fd,
				   struct priv_cmd_entry **entry,
				   struct gk20a_fence **fence,
				   bool wfi,
				   bool need_sync_fence)
{
	/* Need to do 'wfi + host incr' since we return the fence
	 * to user space. */
	return __gk20a_channel_syncpt_incr(s,
			wfi,
			true /* register irq */,
			entry, fence, need_sync_fence);
}

static void gk20a_channel_syncpt_set_min_eq_max(struct gk20a_channel_sync *s)
{
	struct gk20a_channel_syncpt *sp =
		container_of(s, struct gk20a_channel_syncpt, ops);
	nvhost_syncpt_set_min_eq_max_ext(sp->host1x_pdev, sp->id);
}

static void gk20a_channel_syncpt_signal_timeline(
		struct gk20a_channel_sync *s)
{
	/* Nothing to do. */
}

static int gk20a_channel_syncpt_id(struct gk20a_channel_sync *s)
{
	struct gk20a_channel_syncpt *sp =
		container_of(s, struct gk20a_channel_syncpt, ops);
	return sp->id;
}

static void gk20a_channel_syncpt_destroy(struct gk20a_channel_sync *s)
{
	struct gk20a_channel_syncpt *sp =
		container_of(s, struct gk20a_channel_syncpt, ops);
	nvhost_syncpt_set_min_eq_max_ext(sp->host1x_pdev, sp->id);
	nvhost_syncpt_put_ref_ext(sp->host1x_pdev, sp->id);
	kfree(sp);
}

static struct gk20a_channel_sync *
gk20a_channel_syncpt_create(struct channel_gk20a *c)
{
	struct gk20a_channel_syncpt *sp;
	char syncpt_name[32];

	sp = kzalloc(sizeof(*sp), GFP_KERNEL);
	if (!sp)
		return NULL;

	sp->c = c;
	sp->host1x_pdev = c->g->host1x_dev;

	snprintf(syncpt_name, sizeof(syncpt_name),
		"%s_%d", dev_name(c->g->dev), c->hw_chid);

	sp->id = nvhost_get_syncpt_host_managed(sp->host1x_pdev,
						c->hw_chid, syncpt_name);
	if (!sp->id) {
		kfree(sp);
		gk20a_err(c->g->dev, "failed to get free syncpt");
		return NULL;
	}

	nvhost_syncpt_set_min_eq_max_ext(sp->host1x_pdev, sp->id);

	atomic_set(&sp->ops.refcount, 0);
	sp->ops.wait_syncpt		= gk20a_channel_syncpt_wait_syncpt;
	sp->ops.wait_fd			= gk20a_channel_syncpt_wait_fd;
	sp->ops.incr			= gk20a_channel_syncpt_incr;
	sp->ops.incr_wfi		= gk20a_channel_syncpt_incr_wfi;
	sp->ops.incr_user		= gk20a_channel_syncpt_incr_user;
	sp->ops.set_min_eq_max		= gk20a_channel_syncpt_set_min_eq_max;
	sp->ops.signal_timeline		= gk20a_channel_syncpt_signal_timeline;
	sp->ops.syncpt_id		= gk20a_channel_syncpt_id;
	sp->ops.destroy			= gk20a_channel_syncpt_destroy;

	return &sp->ops;
}
#endif /* CONFIG_TEGRA_GK20A */

struct gk20a_channel_semaphore {
	struct gk20a_channel_sync ops;
	struct channel_gk20a *c;

	/* A semaphore pool owned by this channel. */
	struct gk20a_semaphore_pool *pool;

	/* A sync timeline that advances when gpu completes work. */
	struct sync_timeline *timeline;
};

#ifdef CONFIG_SYNC
struct wait_fence_work {
	struct sync_fence_waiter waiter;
	struct channel_gk20a *ch;
	struct gk20a_semaphore *sema;
};

static void gk20a_channel_semaphore_launcher(
		struct sync_fence *fence,
		struct sync_fence_waiter *waiter)
{
	int err;
	struct wait_fence_work *w =
		container_of(waiter, struct wait_fence_work, waiter);
	struct gk20a *g = w->ch->g;

	gk20a_dbg_info("waiting for pre fence %p '%s'",
			fence, fence->name);
	err = sync_fence_wait(fence, -1);
	if (err < 0)
		dev_err(g->dev, "error waiting pre-fence: %d\n", err);

	gk20a_dbg_info(
		  "wait completed (%d) for fence %p '%s', triggering gpu work",
		  err, fence, fence->name);
	sync_fence_put(fence);
	gk20a_semaphore_release(w->sema);
	gk20a_semaphore_put(w->sema);
	kfree(w);
}
#endif

static int add_sema_cmd(struct gk20a *g, struct priv_cmd_entry *cmd,
		u64 sema, u32 payload, bool acquire, bool wfi)
{
	u32 off = cmd->off;
	/* semaphore_a */
	gk20a_mem_wr32(g, cmd->mem, off++, 0x20010004);
	/* offset_upper */
	gk20a_mem_wr32(g, cmd->mem, off++, (sema >> 32) & 0xff);
	/* semaphore_b */
	gk20a_mem_wr32(g, cmd->mem, off++, 0x20010005);
	/* offset */
	gk20a_mem_wr32(g, cmd->mem, off++, sema & 0xffffffff);
	/* semaphore_c */
	gk20a_mem_wr32(g, cmd->mem, off++, 0x20010006);
	/* payload */
	gk20a_mem_wr32(g, cmd->mem, off++, payload);
	if (acquire) {
		/* semaphore_d */
		gk20a_mem_wr32(g, cmd->mem, off++, 0x20010007);
		/* operation: acq_geq, switch_en */
		gk20a_mem_wr32(g, cmd->mem, off++, 0x4 | (0x1 << 12));
	} else {
		/* semaphore_d */
		gk20a_mem_wr32(g, cmd->mem, off++, 0x20010007);
		/* operation: release, wfi */
		gk20a_mem_wr32(g, cmd->mem, off++,
				0x2 | ((wfi ? 0x0 : 0x1) << 20));
		/* non_stall_int */
		gk20a_mem_wr32(g, cmd->mem, off++, 0x20010008);
		/* ignored */
		gk20a_mem_wr32(g, cmd->mem, off++, 0);
	}
	return off - cmd->off;
}

static int gk20a_channel_semaphore_wait_syncpt(
		struct gk20a_channel_sync *s, u32 id,
		u32 thresh, struct priv_cmd_entry **entry,
		struct gk20a_fence **fence)
{
	struct gk20a_channel_semaphore *sema =
		container_of(s, struct gk20a_channel_semaphore, ops);
	struct device *dev = dev_from_gk20a(sema->c->g);
	gk20a_err(dev, "trying to use syncpoint synchronization");
	return -ENODEV;
}

static int gk20a_channel_semaphore_wait_fd(
		struct gk20a_channel_sync *s, int fd,
		struct priv_cmd_entry **entry,
		struct gk20a_fence **fence)
{
	struct gk20a_channel_semaphore *sema =
		container_of(s, struct gk20a_channel_semaphore, ops);
	struct channel_gk20a *c = sema->c;
#ifdef CONFIG_SYNC
	struct sync_fence *sync_fence;
	struct priv_cmd_entry *wait_cmd = NULL;
	struct wait_fence_work *w;
	int written;
	int err, ret;
	u64 va;

	sync_fence = gk20a_sync_fence_fdget(fd);
	if (!sync_fence)
		return -EINVAL;

	w = kzalloc(sizeof(*w), GFP_KERNEL);
	if (!w) {
		err = -ENOMEM;
		goto fail;
	}
	sync_fence_waiter_init(&w->waiter, gk20a_channel_semaphore_launcher);
	w->ch = c;
	w->sema = gk20a_semaphore_alloc(sema->pool);
	if (!w->sema) {
		gk20a_err(dev_from_gk20a(c->g), "ran out of semaphores");
		err = -ENOMEM;
		goto fail;
	}

	/* worker takes one reference */
	gk20a_semaphore_get(w->sema);

	err = gk20a_channel_alloc_priv_cmdbuf(c, 8, &wait_cmd);
	if (err) {
		gk20a_err(dev_from_gk20a(c->g),
				"not enough priv cmd buffer space");
		goto fail;
	}

	va = gk20a_semaphore_gpu_va(w->sema, c->vm);
	/* GPU unblocked when when the semaphore value becomes 1. */
	written = add_sema_cmd(c->g, wait_cmd, va, 1, true, false);

	WARN_ON(written != wait_cmd->size);
	ret = sync_fence_wait_async(sync_fence, &w->waiter);

	/*
	 * If the sync_fence has already signaled then the above async_wait
	 * will never trigger. This causes the semaphore release op to never
	 * happen which, in turn, hangs the GPU. That's bad. So let's just
	 * do the semaphore_release right now.
	 */
	if (ret == 1)
		gk20a_semaphore_release(w->sema);

	/* XXX - this fixes an actual bug, we need to hold a ref to this
	   semaphore while the job is in flight. */
	*fence = gk20a_fence_from_semaphore(sema->timeline, w->sema,
					    &c->semaphore_wq,
					    NULL, false);
	*entry = wait_cmd;
	return 0;
fail:
	if (w && w->sema)
		gk20a_semaphore_put(w->sema);
	kfree(w);
	sync_fence_put(sync_fence);
	return err;
#else
	gk20a_err(dev_from_gk20a(c->g),
		  "trying to use sync fds with CONFIG_SYNC disabled");
	return -ENODEV;
#endif
}

static int __gk20a_channel_semaphore_incr(
		struct gk20a_channel_sync *s, bool wfi_cmd,
		struct sync_fence *dependency,
		struct priv_cmd_entry **entry,
		struct gk20a_fence **fence,
		bool need_sync_fence)
{
	u64 va;
	int incr_cmd_size;
	int written;
	struct priv_cmd_entry *incr_cmd = NULL;
	struct gk20a_channel_semaphore *sp =
		container_of(s, struct gk20a_channel_semaphore, ops);
	struct channel_gk20a *c = sp->c;
	struct gk20a_semaphore *semaphore;
	int err = 0;

	semaphore = gk20a_semaphore_alloc(sp->pool);
	if (!semaphore) {
		gk20a_err(dev_from_gk20a(c->g),
				"ran out of semaphores");
		return -ENOMEM;
	}

	incr_cmd_size = 10;
	err = gk20a_channel_alloc_priv_cmdbuf(c, incr_cmd_size, &incr_cmd);
	if (err) {
		gk20a_err(dev_from_gk20a(c->g),
				"not enough priv cmd buffer space");
		gk20a_semaphore_put(semaphore);
		return err;
	}

	/* Release the completion semaphore. */
	va = gk20a_semaphore_gpu_va(semaphore, c->vm);
	written = add_sema_cmd(c->g, incr_cmd, va, 1, false, wfi_cmd);
	WARN_ON(written != incr_cmd_size);

	*fence = gk20a_fence_from_semaphore(sp->timeline, semaphore,
					    &c->semaphore_wq,
					    dependency, wfi_cmd);
	*entry = incr_cmd;
	return 0;
}

static int gk20a_channel_semaphore_incr_wfi(
		struct gk20a_channel_sync *s,
		struct priv_cmd_entry **entry,
		struct gk20a_fence **fence)
{
	return __gk20a_channel_semaphore_incr(s,
			true /* wfi */,
			NULL,
			entry, fence, true);
}

static int gk20a_channel_semaphore_incr(
		struct gk20a_channel_sync *s,
		struct priv_cmd_entry **entry,
		struct gk20a_fence **fence,
		bool need_sync_fence)
{
	/* Don't put wfi cmd to this one since we're not returning
	 * a fence to user space. */
	return __gk20a_channel_semaphore_incr(s, false /* no wfi */,
				      NULL, entry, fence, need_sync_fence);
}

static int gk20a_channel_semaphore_incr_user(
		struct gk20a_channel_sync *s,
		int wait_fence_fd,
		struct priv_cmd_entry **entry,
		struct gk20a_fence **fence,
		bool wfi,
		bool need_sync_fence)
{
#ifdef CONFIG_SYNC
	struct sync_fence *dependency = NULL;
	int err;

	if (wait_fence_fd >= 0) {
		dependency = gk20a_sync_fence_fdget(wait_fence_fd);
		if (!dependency)
			return -EINVAL;
	}

	err = __gk20a_channel_semaphore_incr(s, wfi, dependency,
					     entry, fence, need_sync_fence);
	if (err) {
		if (dependency)
			sync_fence_put(dependency);
		return err;
	}

	return 0;
#else
	struct gk20a_channel_semaphore *sema =
		container_of(s, struct gk20a_channel_semaphore, ops);
	gk20a_err(dev_from_gk20a(sema->c->g),
		  "trying to use sync fds with CONFIG_SYNC disabled");
	return -ENODEV;
#endif
}

static void gk20a_channel_semaphore_set_min_eq_max(struct gk20a_channel_sync *s)
{
	/* Nothing to do. */
}

static void gk20a_channel_semaphore_signal_timeline(
		struct gk20a_channel_sync *s)
{
	struct gk20a_channel_semaphore *sp =
		container_of(s, struct gk20a_channel_semaphore, ops);
	gk20a_sync_timeline_signal(sp->timeline);
}

static int gk20a_channel_semaphore_syncpt_id(struct gk20a_channel_sync *s)
{
	return -EINVAL;
}

static void gk20a_channel_semaphore_destroy(struct gk20a_channel_sync *s)
{
	struct gk20a_channel_semaphore *sema =
		container_of(s, struct gk20a_channel_semaphore, ops);
	if (sema->timeline)
		gk20a_sync_timeline_destroy(sema->timeline);
	if (sema->pool) {
		gk20a_semaphore_pool_unmap(sema->pool, sema->c->vm);
		gk20a_semaphore_pool_put(sema->pool);
	}
	kfree(sema);
}

static struct gk20a_channel_sync *
gk20a_channel_semaphore_create(struct channel_gk20a *c)
{
	int err;
	int asid = -1;
	struct gk20a_channel_semaphore *sema;
	char pool_name[20];

	if (WARN_ON(!c->vm))
		return NULL;

	sema = kzalloc(sizeof(*sema), GFP_KERNEL);
	if (!sema)
		return NULL;
	sema->c = c;

	if (c->vm->as_share)
		asid = c->vm->as_share->id;

	sprintf(pool_name, "semaphore_pool-%d", c->hw_chid);
	sema->pool = gk20a_semaphore_pool_alloc(c->g, pool_name, 1024);
	if (!sema->pool)
		goto clean_up;

	/* Map the semaphore pool to the channel vm. Map as read-write to the
	 * owner channel (all other channels should map as read only!). */
	err = gk20a_semaphore_pool_map(sema->pool, c->vm, gk20a_mem_flag_none);
	if (err)
		goto clean_up;

#ifdef CONFIG_SYNC
	sema->timeline = gk20a_sync_timeline_create(
			"gk20a_ch%d_as%d", c->hw_chid, asid);
	if (!sema->timeline)
		goto clean_up;
#endif
	atomic_set(&sema->ops.refcount, 0);
	sema->ops.wait_syncpt	= gk20a_channel_semaphore_wait_syncpt;
	sema->ops.wait_fd	= gk20a_channel_semaphore_wait_fd;
	sema->ops.incr		= gk20a_channel_semaphore_incr;
	sema->ops.incr_wfi	= gk20a_channel_semaphore_incr_wfi;
	sema->ops.incr_user	= gk20a_channel_semaphore_incr_user;
	sema->ops.set_min_eq_max = gk20a_channel_semaphore_set_min_eq_max;
	sema->ops.signal_timeline = gk20a_channel_semaphore_signal_timeline;
	sema->ops.syncpt_id	= gk20a_channel_semaphore_syncpt_id;
	sema->ops.destroy	= gk20a_channel_semaphore_destroy;

	return &sema->ops;
clean_up:
	gk20a_channel_semaphore_destroy(&sema->ops);
	return NULL;
}

void gk20a_channel_sync_destroy(struct gk20a_channel_sync *sync)
{
	sync->destroy(sync);
}

struct gk20a_channel_sync *gk20a_channel_sync_create(struct channel_gk20a *c)
{
#ifdef CONFIG_TEGRA_GK20A
	if (gk20a_platform_has_syncpoints(c->g->dev))
		return gk20a_channel_syncpt_create(c);
#endif
	return gk20a_channel_semaphore_create(c);
}
