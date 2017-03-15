/*
 * GM20B MMU
 *
 * Copyright (c) 2014-2017, NVIDIA CORPORATION.  All rights reserved.
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

#include <linux/delay.h>

#include "gk20a/gk20a.h"

#include "mm_gm20b.h"

#include <nvgpu/timers.h>

#include <nvgpu/hw/gm20b/hw_gmmu_gm20b.h>
#include <nvgpu/hw/gm20b/hw_ram_gm20b.h>
#include <nvgpu/hw/gm20b/hw_bus_gm20b.h>

static void gm20b_mm_set_big_page_size(struct gk20a *g,
				struct mem_desc *mem, int size)
{
	u32 val;

	gk20a_dbg_fn("");

	gk20a_dbg_info("big page size %d\n", size);
	val = gk20a_mem_rd32(g, mem, ram_in_big_page_size_w());
	val &= ~ram_in_big_page_size_m();

	if (size == SZ_64K)
		val |= ram_in_big_page_size_64kb_f();
	else
		val |= ram_in_big_page_size_128kb_f();

	gk20a_mem_wr32(g, mem, ram_in_big_page_size_w(), val);
	gk20a_dbg_fn("done");
}

static u32 gm20b_mm_get_big_page_sizes(void)
{
	return SZ_64K | SZ_128K;
}

static bool gm20b_mm_support_sparse(struct gk20a *g)
{
	return true;
}

static int gm20b_mm_bar1_bind(struct gk20a *g, struct mem_desc *bar1_inst)
{
	int retry = 1000;
	u64 iova = gk20a_mm_inst_block_addr(g, bar1_inst);
	u32 ptr_v = (u32)(iova >> bar1_instance_block_shift_gk20a());

	gk20a_dbg_info("bar1 inst block ptr: 0x%08x", ptr_v);

	gk20a_writel(g, bus_bar1_block_r(),
		     gk20a_aperture_mask(g, bar1_inst,
		       bus_bar1_block_target_sys_mem_ncoh_f(),
		       bus_bar1_block_target_vid_mem_f()) |
		     bus_bar1_block_mode_virtual_f() |
		     bus_bar1_block_ptr_f(ptr_v));
	do {
		u32 val = gk20a_readl(g, bus_bind_status_r());
		u32 pending = bus_bind_status_bar1_pending_v(val);
		u32 outstanding = bus_bind_status_bar1_outstanding_v(val);
		if (!pending && !outstanding)
			break;

		udelay(5);
		retry--;
	} while (retry >= 0 || !tegra_platform_is_silicon());

	return retry ? -EINVAL : 0;
}

static bool gm20b_mm_is_bar1_supported(struct gk20a *g)
{
	return true;
}

void gm20b_init_mm(struct gpu_ops *gops)
{
	gops->mm.support_sparse = gm20b_mm_support_sparse;
	gops->mm.gmmu_map = gk20a_locked_gmmu_map;
	gops->mm.gmmu_unmap = gk20a_locked_gmmu_unmap;
	gops->mm.vm_remove = gk20a_vm_remove_support;
	gops->mm.vm_alloc_share = gk20a_vm_alloc_share;
	gops->mm.vm_bind_channel = gk20a_vm_bind_channel;
	gops->mm.fb_flush = gk20a_mm_fb_flush;
	gops->mm.l2_invalidate = gk20a_mm_l2_invalidate;
	gops->mm.l2_flush = gk20a_mm_l2_flush;
	gops->mm.cbc_clean = gk20a_mm_cbc_clean;
	gops->mm.set_big_page_size = gm20b_mm_set_big_page_size;
	gops->mm.get_big_page_sizes = gm20b_mm_get_big_page_sizes;
	gops->mm.get_iova_addr = gk20a_mm_iova_addr;
	gops->mm.get_physical_addr_bits = gk20a_mm_get_physical_addr_bits;
	gops->mm.get_mmu_levels = gk20a_mm_get_mmu_levels;
	gops->mm.init_pdb = gk20a_mm_init_pdb;
	gops->mm.init_mm_setup_hw = gk20a_init_mm_setup_hw;
	gops->mm.bar1_bind = gm20b_mm_bar1_bind;
	gops->mm.is_bar1_supported = gm20b_mm_is_bar1_supported;
	gops->mm.init_inst_block = gk20a_init_inst_block;
}
