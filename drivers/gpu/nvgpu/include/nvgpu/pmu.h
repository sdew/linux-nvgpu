/*
 * Copyright (c) 2017, NVIDIA CORPORATION.  All rights reserved.
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

#ifndef __NVGPU_PMU_H__
#define __NVGPU_PMU_H__

#include <nvgpu/kmem.h>
#include <nvgpu/nvgpu_mem.h>
#include <nvgpu/allocator.h>
#include <nvgpu/lock.h>
#include <nvgpu/cond.h>
#include <nvgpu/thread.h>
#include <nvgpu/nvgpu_common.h>
#include <nvgpu/flcnif_cmn.h>
#include <nvgpu/pmuif/nvgpu_gpmu_cmdif.h>

#define nvgpu_pmu_dbg(g, fmt, args...) \
	nvgpu_log(g, gpu_dbg_pmu, fmt, ##args)

/* defined by pmu hw spec */
#define GK20A_PMU_VA_SIZE		(512 * 1024 * 1024)
#define GK20A_PMU_UCODE_SIZE_MAX	(256 * 1024)
#define GK20A_PMU_SEQ_BUF_SIZE		4096

#define GK20A_PMU_TRACE_BUFSIZE		0x4000    /* 4K */
#define GK20A_PMU_DMEM_BLKSIZE2		8

#define PMU_MODE_MISMATCH_STATUS_MAILBOX_R  6
#define PMU_MODE_MISMATCH_STATUS_VAL        0xDEADDEAD

/* Falcon Register index */
#define PMU_FALCON_REG_R0		(0)
#define PMU_FALCON_REG_R1		(1)
#define PMU_FALCON_REG_R2		(2)
#define PMU_FALCON_REG_R3		(3)
#define PMU_FALCON_REG_R4		(4)
#define PMU_FALCON_REG_R5		(5)
#define PMU_FALCON_REG_R6		(6)
#define PMU_FALCON_REG_R7		(7)
#define PMU_FALCON_REG_R8		(8)
#define PMU_FALCON_REG_R9		(9)
#define PMU_FALCON_REG_R10		(10)
#define PMU_FALCON_REG_R11		(11)
#define PMU_FALCON_REG_R12		(12)
#define PMU_FALCON_REG_R13		(13)
#define PMU_FALCON_REG_R14		(14)
#define PMU_FALCON_REG_R15		(15)
#define PMU_FALCON_REG_IV0		(16)
#define PMU_FALCON_REG_IV1		(17)
#define PMU_FALCON_REG_UNDEFINED	(18)
#define PMU_FALCON_REG_EV		(19)
#define PMU_FALCON_REG_SP		(20)
#define PMU_FALCON_REG_PC		(21)
#define PMU_FALCON_REG_IMB		(22)
#define PMU_FALCON_REG_DMB		(23)
#define PMU_FALCON_REG_CSW		(24)
#define PMU_FALCON_REG_CCR		(25)
#define PMU_FALCON_REG_SEC		(26)
#define PMU_FALCON_REG_CTX		(27)
#define PMU_FALCON_REG_EXCI		(28)
#define PMU_FALCON_REG_RSVD0	(29)
#define PMU_FALCON_REG_RSVD1	(30)
#define PMU_FALCON_REG_RSVD2	(31)
#define PMU_FALCON_REG_SIZE		(32)

/* Choices for pmu_state */
#define PMU_STATE_OFF			0 /* PMU is off */
#define PMU_STATE_STARTING		1 /* PMU is on, but not booted */
#define PMU_STATE_INIT_RECEIVED		2 /* PMU init message received */
#define PMU_STATE_ELPG_BOOTING		3 /* PMU is booting */
#define PMU_STATE_ELPG_BOOTED		4 /* ELPG is initialized */
#define PMU_STATE_LOADING_PG_BUF	5 /* Loading PG buf */
#define PMU_STATE_LOADING_ZBC		6 /* Loading ZBC buf */
#define PMU_STATE_STARTED		7 /* Fully unitialized */
#define PMU_STATE_EXIT			8 /* Exit PMU state machine */

#define GK20A_PMU_UCODE_NB_MAX_OVERLAY	    32
#define GK20A_PMU_UCODE_NB_MAX_DATE_LENGTH  64

#define PMU_MAX_NUM_SEQUENCES		(256)
#define PMU_SEQ_BIT_SHIFT		(5)
#define PMU_SEQ_TBL_SIZE	\
		(PMU_MAX_NUM_SEQUENCES >> PMU_SEQ_BIT_SHIFT)

#define PMU_INVALID_SEQ_DESC		(~0)

enum {
	GK20A_PMU_DMAIDX_UCODE		= 0,
	GK20A_PMU_DMAIDX_VIRT		= 1,
	GK20A_PMU_DMAIDX_PHYS_VID	= 2,
	GK20A_PMU_DMAIDX_PHYS_SYS_COH	= 3,
	GK20A_PMU_DMAIDX_PHYS_SYS_NCOH	= 4,
	GK20A_PMU_DMAIDX_RSVD		= 5,
	GK20A_PMU_DMAIDX_PELPG		= 6,
	GK20A_PMU_DMAIDX_END		= 7
};

enum {
	PMU_SEQ_STATE_FREE = 0,
	PMU_SEQ_STATE_PENDING,
	PMU_SEQ_STATE_USED,
	PMU_SEQ_STATE_CANCELLED
};

typedef void (*pmu_callback)(struct gk20a *, struct pmu_msg *, void *, u32,
	u32);

struct pmu_ucode_desc {
	u32 descriptor_size;
	u32 image_size;
	u32 tools_version;
	u32 app_version;
	char date[GK20A_PMU_UCODE_NB_MAX_DATE_LENGTH];
	u32 bootloader_start_offset;
	u32 bootloader_size;
	u32 bootloader_imem_offset;
	u32 bootloader_entry_point;
	u32 app_start_offset;
	u32 app_size;
	u32 app_imem_offset;
	u32 app_imem_entry;
	u32 app_dmem_offset;
	/* Offset from appStartOffset */
	u32 app_resident_code_offset;
	/* Exact size of the resident code
	 * ( potentially contains CRC inside at the end )
	 */
	u32 app_resident_code_size;
	/* Offset from appStartOffset */
	u32 app_resident_data_offset;
	/* Exact size of the resident code
	 * ( potentially contains CRC inside at the end )
	 */
	u32 app_resident_data_size;
	u32 nb_overlays;
	struct {u32 start; u32 size; } load_ovl[GK20A_PMU_UCODE_NB_MAX_OVERLAY];
	u32 compressed;
};

struct pmu_ucode_desc_v1 {
	u32 descriptor_size;
	u32 image_size;
	u32 tools_version;
	u32 app_version;
	char date[GK20A_PMU_UCODE_NB_MAX_DATE_LENGTH];
	u32 bootloader_start_offset;
	u32 bootloader_size;
	u32 bootloader_imem_offset;
	u32 bootloader_entry_point;
	u32 app_start_offset;
	u32 app_size;
	u32 app_imem_offset;
	u32 app_imem_entry;
	u32 app_dmem_offset;
	u32 app_resident_code_offset;
	u32 app_resident_code_size;
	u32 app_resident_data_offset;
	u32 app_resident_data_size;
	u32 nb_imem_overlays;
	u32 nb_dmem_overlays;
	struct {u32 start; u32 size; } load_ovl[64];
	u32 compressed;
};

struct pmu_queue {

	/* used by hw, for BIOS/SMI queue */
	u32 mutex_id;
	u32 mutex_lock;
	/* used by sw, for LPQ/HPQ queue */
	struct nvgpu_mutex mutex;

	/* current write position */
	u32 position;
	/* physical dmem offset where this queue begins */
	u32 offset;
	/* logical queue identifier */
	u32 id;
	/* physical queue index */
	u32 index;
	/* in bytes */
	u32 size;

	/* open-flag */
	u32 oflag;
	bool opened; /* opened implies locked */
};

struct pmu_mutex {
	u32 id;
	u32 index;
	u32 ref_cnt;
};

struct pmu_sequence {
	u8 id;
	u32 state;
	u32 desc;
	struct pmu_msg *msg;
	union {
		struct pmu_allocation_v0 in_v0;
		struct pmu_allocation_v1 in_v1;
		struct pmu_allocation_v2 in_v2;
		struct pmu_allocation_v3 in_v3;
	};
	struct nvgpu_mem *in_mem;
	union {
		struct pmu_allocation_v0 out_v0;
		struct pmu_allocation_v1 out_v1;
		struct pmu_allocation_v2 out_v2;
		struct pmu_allocation_v3 out_v3;
	};
	struct nvgpu_mem *out_mem;
	u8 *out_payload;
	pmu_callback callback;
	void *cb_params;
};

struct nvgpu_pg_init {
	bool state_change;
	struct nvgpu_cond wq;
	struct nvgpu_thread state_task;
};

struct nvgpu_pmu {
	struct gk20a *g;
	struct nvgpu_falcon *flcn;

	union {
		struct pmu_ucode_desc *desc;
		struct pmu_ucode_desc_v1 *desc_v1;
	};
	struct nvgpu_mem ucode;

	struct nvgpu_mem pg_buf;

	/* TBD: remove this if ZBC seq is fixed */
	struct nvgpu_mem seq_buf;
	struct nvgpu_mem trace_buf;
	struct nvgpu_mem wpr_buf;
	bool buf_loaded;

	struct pmu_sha1_gid gid_info;

	struct pmu_queue queue[PMU_QUEUE_COUNT];

	struct pmu_sequence *seq;
	unsigned long pmu_seq_tbl[PMU_SEQ_TBL_SIZE];
	u32 next_seq_desc;

	struct pmu_mutex *mutex;
	u32 mutex_cnt;

	struct nvgpu_mutex pmu_copy_lock;
	struct nvgpu_mutex pmu_seq_lock;

	struct nvgpu_allocator dmem;

	u32 *ucode_image;
	bool pmu_ready;

	u32 zbc_save_done;

	u32 stat_dmem_offset[PMU_PG_ELPG_ENGINE_ID_INVALID_ENGINE];

	u32 elpg_stat;

	u32 mscg_stat;
	u32 mscg_transition_state;

	int pmu_state;

#define PMU_ELPG_ENABLE_ALLOW_DELAY_MSEC	1 /* msec */
	struct nvgpu_pg_init pg_init;
	struct nvgpu_mutex pg_mutex; /* protect pg-RPPG/MSCG enable/disable */
	struct nvgpu_mutex elpg_mutex; /* protect elpg enable/disable */
	/* disable -1, enable +1, <=0 elpg disabled, > 0 elpg enabled */
	int elpg_refcnt;

	union {
		struct pmu_perfmon_counter_v2 perfmon_counter_v2;
		struct pmu_perfmon_counter_v0 perfmon_counter_v0;
	};
	u32 perfmon_state_id[PMU_DOMAIN_GROUP_NUM];

	bool initialized;

	void (*remove_support)(struct nvgpu_pmu *pmu);
	bool sw_ready;
	bool perfmon_ready;

	u32 sample_buffer;
	u32 load_shadow;
	u32 load_avg;

	struct nvgpu_mutex isr_mutex;
	bool isr_enabled;

	bool zbc_ready;
	union {
		struct pmu_cmdline_args_v0 args_v0;
		struct pmu_cmdline_args_v1 args_v1;
		struct pmu_cmdline_args_v2 args_v2;
		struct pmu_cmdline_args_v3 args_v3;
		struct pmu_cmdline_args_v4 args_v4;
		struct pmu_cmdline_args_v5 args_v5;
	};
	unsigned long perfmon_events_cnt;
	bool perfmon_sampling_enabled;
	u8 pmu_mode; /*Added for GM20b, and ACR*/
	u32 falcon_id;
	u32 aelpg_param[5];
	u32 override_done;

	struct nvgpu_firmware *fw;
};

#endif /* __NVGPU_PMU_H__ */