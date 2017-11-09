/*
 * Copyright (c) 2015-2017, NVIDIA CORPORATION.  All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <linux/tegra_gr_comm.h>
#include <linux/tegra_vgpu.h>
#include <uapi/linux/nvgpu.h>

#include "gk20a/gk20a.h"
#include "gk20a/channel_gk20a.h"
#include "gk20a/dbg_gpu_gk20a.h"
#include "vgpu.h"
#include "dbg_vgpu.h"

#include <nvgpu/bug.h>

int vgpu_exec_regops(struct dbg_session_gk20a *dbg_s,
		      struct nvgpu_dbg_gpu_reg_op *ops,
		      u64 num_ops)
{
	struct channel_gk20a *ch;
	struct tegra_vgpu_cmd_msg msg;
	struct tegra_vgpu_reg_ops_params *p = &msg.params.reg_ops;
	void *oob;
	size_t oob_size, ops_size;
	void *handle = NULL;
	int err = 0;

	gk20a_dbg_fn("");
	BUG_ON(sizeof(*ops) != sizeof(struct tegra_vgpu_reg_op));

	handle = tegra_gr_comm_oob_get_ptr(TEGRA_GR_COMM_CTX_CLIENT,
					tegra_gr_comm_get_server_vmid(),
					TEGRA_VGPU_QUEUE_CMD,
					&oob, &oob_size);
	if (!handle)
		return -EINVAL;

	ops_size = sizeof(*ops) * num_ops;
	if (oob_size < ops_size) {
		err = -ENOMEM;
		goto fail;
	}

	memcpy(oob, ops, ops_size);

	msg.cmd = TEGRA_VGPU_CMD_REG_OPS;
	msg.handle = vgpu_get_handle(dbg_s->g);
	ch = nvgpu_dbg_gpu_get_session_channel(dbg_s);
	p->handle = ch ? ch->virt_ctx : 0;
	p->num_ops = num_ops;
	p->is_profiler = dbg_s->is_profiler;
	err = vgpu_comm_sendrecv(&msg, sizeof(msg), sizeof(msg));
	err = err ? err : msg.ret;
	if (!err)
		memcpy(ops, oob, ops_size);

fail:
	tegra_gr_comm_oob_put_ptr(handle);
	return err;
}

int vgpu_dbg_set_powergate(struct dbg_session_gk20a *dbg_s, bool disable_powergate)
{
	struct tegra_vgpu_cmd_msg msg;
	struct tegra_vgpu_set_powergate_params *p = &msg.params.set_powergate;
	int err = 0;
	u32 mode;

	gk20a_dbg_fn("");

	/* Just return if requested mode is the same as the session's mode */
	if (disable_powergate) {
		if (dbg_s->is_pg_disabled)
			return 0;
		dbg_s->is_pg_disabled = true;
		mode = NVGPU_DBG_GPU_POWERGATE_MODE_DISABLE;
	} else {
		if (!dbg_s->is_pg_disabled)
			return 0;
		dbg_s->is_pg_disabled = false;
		mode = NVGPU_DBG_GPU_POWERGATE_MODE_ENABLE;
	}

	msg.cmd = TEGRA_VGPU_CMD_SET_POWERGATE;
	msg.handle = vgpu_get_handle(dbg_s->g);
	p->mode = mode;
	err = vgpu_comm_sendrecv(&msg, sizeof(msg), sizeof(msg));
	err = err ? err : msg.ret;
	return err;
}

static int vgpu_sendrecv_prof_cmd(struct dbg_session_gk20a *dbg_s, u32 mode)
{
	struct tegra_vgpu_cmd_msg msg;
	struct tegra_vgpu_prof_mgt_params *p = &msg.params.prof_management;
	int err = 0;

	msg.cmd = TEGRA_VGPU_CMD_PROF_MGT;
	msg.handle = vgpu_get_handle(dbg_s->g);

	p->mode = mode;

	err = vgpu_comm_sendrecv(&msg, sizeof(msg), sizeof(msg));
	err = err ? err : msg.ret;
	return err;
}

bool vgpu_check_and_set_global_reservation(
				struct dbg_session_gk20a *dbg_s,
				struct dbg_profiler_object_data *prof_obj)
{
	struct gk20a *g = dbg_s->g;

	if (g->profiler_reservation_count > 0)
		return false;

	/* Check that another guest OS doesn't already have a reservation */
	if (!vgpu_sendrecv_prof_cmd(dbg_s, TEGRA_VGPU_PROF_GET_GLOBAL)) {
		g->global_profiler_reservation_held = true;
		g->profiler_reservation_count = 1;
		dbg_s->has_profiler_reservation = true;
		prof_obj->has_reservation = true;
		return true;
	}
	return false;
}

bool vgpu_check_and_set_context_reservation(
				struct dbg_session_gk20a *dbg_s,
				struct dbg_profiler_object_data *prof_obj)
{
	struct gk20a *g = dbg_s->g;

	/* Assumes that we've already checked that no global reservation
	 * is in effect for this guest.
	 *
	 * If our reservation count is non-zero, then no other guest has the
	 * global reservation; if it is zero, need to check with RM server.
	 *
	 */
	if ((g->profiler_reservation_count != 0) ||
		!vgpu_sendrecv_prof_cmd(dbg_s, TEGRA_VGPU_PROF_GET_CONTEXT)) {
		g->profiler_reservation_count++;
		dbg_s->has_profiler_reservation = true;
		prof_obj->has_reservation = true;
		return true;
	}
	return false;
}

void vgpu_release_profiler_reservation(
				struct dbg_session_gk20a *dbg_s,
				struct dbg_profiler_object_data *prof_obj)
{
	struct gk20a *g = dbg_s->g;

	dbg_s->has_profiler_reservation = false;
	prof_obj->has_reservation = false;
	if (prof_obj->ch == NULL)
		g->global_profiler_reservation_held = false;

	/* If new reservation count is zero, notify server */
	g->profiler_reservation_count--;
	if (g->profiler_reservation_count == 0)
		vgpu_sendrecv_prof_cmd(dbg_s, TEGRA_VGPU_PROF_RELEASE);
}

static int vgpu_sendrecv_perfbuf_cmd(struct gk20a *g, u64 offset, u32 size)
{
	struct mm_gk20a *mm = &g->mm;
	struct vm_gk20a *vm = mm->perfbuf.vm;
	struct tegra_vgpu_cmd_msg msg;
	struct tegra_vgpu_perfbuf_mgt_params *p =
						&msg.params.perfbuf_management;
	int err;

	msg.cmd = TEGRA_VGPU_CMD_PERFBUF_MGT;
	msg.handle = vgpu_get_handle(g);

	p->vm_handle = vm->handle;
	p->offset = offset;
	p->size = size;

	err = vgpu_comm_sendrecv(&msg, sizeof(msg), sizeof(msg));
	err = err ? err : msg.ret;
	return err;
}

int vgpu_perfbuffer_enable(struct gk20a *g, u64 offset, u32 size)
{
	return vgpu_sendrecv_perfbuf_cmd(g, offset, size);
}

int vgpu_perfbuffer_disable(struct gk20a *g)
{
	return vgpu_sendrecv_perfbuf_cmd(g, 0, 0);
}