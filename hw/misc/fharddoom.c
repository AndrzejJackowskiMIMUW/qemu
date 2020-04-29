/*
 * The Final HardDoom™ device
 *
 * Copyright (C) 2013-2020 Marcelina Kościelnicka
 *
 * This work is licensed under the terms of the GNU GPL, version 2.  See
 * the COPYING file in the top-level directory.
 */

#include "fharddoom.h"
#include "qemu/osdep.h"
#include "qemu/main-loop.h"
#include "hw/hw.h"
#include "hw/pci/pci.h"

#define TYPE_FHARDDOOM "fharddoom"

#define FHARDDOOM_DEV(obj) \
	OBJECT_CHECK(FinalHardDoomState, (obj), TYPE_FHARDDOOM)

typedef struct {
	PCIDevice dev;
	MemoryRegion mmio;
	QemuThread thread;
	QemuMutex mutex;
	QemuCond cond;
	bool stopping;
	/* Registers.  */
#define REG(name, addr, mask) \
	uint32_t name;
#define REG64(name, addr) \
	uint64_t name;
#define REGS(name, addr, num, mask) \
	uint32_t name[num];
#define REGS8(name, addr, num) \
	uint8_t name[num];
#define FIFO(name, hname) \
	uint32_t name ## _get; \
	uint32_t name ## _put;
#define FIFO_DATA(name, hname, data, window, mask) \
	uint32_t data[hname ## _SIZE];
#define FIFO_DATA_BLOCK(name, hname, data, window) \
	uint8_t data[hname ## _SIZE * FHARDDOOM_BLOCK_SIZE];
#define FIFO_DATA64(name, hname, data, window) \
	uint64_t data[hname ## _SIZE];
#include "fharddoom-regs.h"
	uint32_t status;
	uint8_t fe_code[FHARDDOOM_FEMEM_CODE_SIZE];
	uint8_t fe_data[FHARDDOOM_FEMEM_DATA_SIZE];
} FinalHardDoomState;

static uint16_t le16_read(uint8_t *ptr) {
	return ptr[0] | ptr[1] << 8;
}

static uint32_t le32_read(uint8_t *ptr) {
	return ptr[0] | ptr[1] << 8 | ptr[2] << 16 | ptr[3] << 24;
}

static void le16_write(uint8_t *ptr, uint16_t val) {
	ptr[0] = val;
	ptr[1] = val >> 8;
}

static void le32_write(uint8_t *ptr, uint32_t val) {
	ptr[0] = val;
	ptr[1] = val >> 8;
	ptr[2] = val >> 16;
	ptr[3] = val >> 24;
}

#define REG(name, addr, mask)
#define REG64(name, addr)
#define REGS(name, addr, num, mask)
#define REGS8(name, addr, num)
#define FIFO(name, hname) \
	static bool fharddoom_##name##_empty(FinalHardDoomState *d) { \
		return d->name##_get == d->name##_put; \
	} \
	static bool fharddoom_##name##_full(FinalHardDoomState *d) { \
		uint32_t put_next = d->name##_put + 1; \
		put_next &= hname##_SIZE - 1; \
		return d->name##_get == put_next; \
	} \
	static void fharddoom_##name##_read(FinalHardDoomState *d) { \
		uint32_t get_next = d->name##_get + 1; \
		get_next &= hname##_SIZE - 1; \
		d->name##_get = get_next; \
	} \
	static void fharddoom_##name##_write(FinalHardDoomState *d) { \
		uint32_t put_next = d->name##_put + 1; \
		put_next &= hname##_SIZE - 1; \
		d->name##_put = put_next; \
	}
#define FIFO_DATA(name, hname, data, window, mask)
#define FIFO_DATA_BLOCK(name, hname, data, window)
#define FIFO_DATA64(name, hname, data, window)
#include "fharddoom-regs.h"

/* Recomputes status register and PCI interrupt line.  */
static void fharddoom_status_update(FinalHardDoomState *d) {
	d->status = 0;
	/* CMD busy iff has unread commands in memory or non-empty FIFO.  */
	if ((d->cmd_main_setup & FHARDDOOM_CMD_MAIN_SETUP_ENABLE && d->cmd_main_get != d->cmd_main_put) || d->cmd_sub_len || d->cmd_main_fifo_get != d->cmd_main_fifo_put || d->cmd_sub_fifo_get != d->cmd_sub_fifo_put)
		d->status |= FHARDDOOM_STATUS_CMD;
	/* FE busy iff in the running state.  */
	if ((d->fe_state & FHARDDOOM_FE_STATE_STATE_MASK) == FHARDDOOM_FE_STATE_STATE_RUNNING)
		d->status |= FHARDDOOM_STATUS_FE;
	/* SRD busy iff waiting on SRDSEM/FESEM or has non-0 pending read length.  */
	if (d->srd_state & (FHARDDOOM_SRD_STATE_SRDSEM | FHARDDOOM_SRD_STATE_FESEM | FHARDDOOM_SRD_STATE_READ_LENGTH_MASK))
		d->status |= FHARDDOOM_STATUS_SRD;
	/* SPAN busy iff waiting on SPANSEM or has non-0 pending draw length.  */
	if (d->span_state & (FHARDDOOM_SPAN_STATE_SPANSEM | FHARDDOOM_SPAN_STATE_DRAW_LENGTH_MASK))
		d->status |= FHARDDOOM_STATUS_SPAN;
	/* COL busy iff waiting on COLSEM, has non-0 pending draw length, or waiting on LOAD_CMAP_A.  */
	if (d->col_state & (FHARDDOOM_COL_STATE_COLSEM | FHARDDOOM_COL_STATE_LOAD_CMAP_A | FHARDDOOM_COL_STATE_DRAW_LENGTH_MASK))
		d->status |= FHARDDOOM_STATUS_COL;
	/* FX busy iff has non-0 pending draw length or has LOAD_CMAP or INIT_FUZZ operation in progress.  */
	if (d->fx_state & (FHARDDOOM_FX_STATE_DRAW_LENGTH_MASK | FHARDDOOM_FX_STATE_LOAD_ACTIVE))
		d->status |= FHARDDOOM_STATUS_FX;
	/* SWR busy iff has non-0 pending draw length or has one of the SEM operations in progress.  */
	if (d->swr_state & (FHARDDOOM_SWR_STATE_DRAW_LENGTH_MASK | FHARDDOOM_SWR_STATE_SRDSEM | FHARDDOOM_SWR_STATE_COLSEM | FHARDDOOM_SWR_STATE_SPANSEM))
		d->status |= FHARDDOOM_STATUS_SWR;
	if (!fharddoom_srdcmd_empty(d))
		d->status |= FHARDDOOM_STATUS_FIFO_SRDCMD;
	if (!fharddoom_spancmd_empty(d))
		d->status |= FHARDDOOM_STATUS_FIFO_SPANCMD;
	if (!fharddoom_colcmd_empty(d))
		d->status |= FHARDDOOM_STATUS_FIFO_COLCMD;
	if (!fharddoom_fxcmd_empty(d))
		d->status |= FHARDDOOM_STATUS_FIFO_FXCMD;
	if (!fharddoom_swrcmd_empty(d))
		d->status |= FHARDDOOM_STATUS_FIFO_SWRCMD;
	if (!fharddoom_colin_empty(d))
		d->status |= FHARDDOOM_STATUS_FIFO_COLIN;
	if (!fharddoom_fxin_empty(d))
		d->status |= FHARDDOOM_STATUS_FIFO_FXIN;
	if (d->fesem)
		d->status |= FHARDDOOM_STATUS_FIFO_FESEM;
	if (d->srdsem)
		d->status |= FHARDDOOM_STATUS_FIFO_SRDSEM;
	if (d->colsem)
		d->status |= FHARDDOOM_STATUS_FIFO_COLSEM;
	if (d->spansem)
		d->status |= FHARDDOOM_STATUS_FIFO_SPANSEM;
	if (!fharddoom_spanout_empty(d))
		d->status |= FHARDDOOM_STATUS_FIFO_SPANOUT;
	if (!fharddoom_colout_empty(d))
		d->status |= FHARDDOOM_STATUS_FIFO_COLOUT;
	if (!fharddoom_fxout_empty(d))
		d->status |= FHARDDOOM_STATUS_FIFO_FXOUT;
	/* determine and set PCI interrupt status */
	pci_set_irq(PCI_DEVICE(d), !!(d->intr & d->intr_enable));
}

/* Resets MMU, unbinding everything.  */
static void fharddoom_reset_mmu(FinalHardDoomState *d) {
	int i;
	for (i = 0; i < FHARDDOOM_MMU_SLOT_NUM; i++) {
		d->mmu_slot[i] = 0;
	}
}

/* Resets TLBs, forcing a reread of PT.  */
static void fharddoom_reset_tlb(FinalHardDoomState *d) {
	int i;
	for (i = 0; i < FHARDDOOM_MMU_CLIENT_NUM; i++) {
		d->mmu_client_tlb_tag[i] &= ~FHARDDOOM_MMU_TLB_TAG_VALID;
	}
	for (i = 0; i < FHARDDOOM_MMU_TLB_SIZE; i++) {
		d->mmu_tlb_tag[i] &= ~FHARDDOOM_MMU_TLB_TAG_VALID;
	}
}

/* Clears TLBs for a particular slot, forcing a reread of PT.  */
static void fharddoom_reset_tlb_slot(FinalHardDoomState *d, int which) {
	int i;
	for (i = 0; i < FHARDDOOM_MMU_CLIENT_NUM; i++) {
		int slot = FHARDDOOM_VA_SLOT(d->mmu_client_tlb_tag[i]);
		if (slot != which)
			continue;
		d->mmu_client_tlb_tag[i] &= ~FHARDDOOM_MMU_TLB_TAG_VALID;
	}
	for (i = 0; i < FHARDDOOM_MMU_TLB_SIZE; i++) {
		int slot = FHARDDOOM_VA_SLOT(d->mmu_tlb_tag[i]);
		if (slot != which)
			continue;
		d->mmu_tlb_tag[i] &= ~FHARDDOOM_MMU_TLB_TAG_VALID;
	}
}

/* Sets the given PD and flushes corresponding TLB.  */
static void fharddoom_mmu_bind(FinalHardDoomState *d, int which, uint32_t val) {
	d->mmu_slot[which] = val & FHARDDOOM_MMU_SLOT_MASK;
	fharddoom_reset_tlb_slot(d, which);
}

static uint32_t fharddoom_hash(uint32_t word) {
	uint32_t res = 0;
	while (word) {
		res ^= (word & 0x3f);
		word >>= 6;
	}
	return res;
}

/* Converts virtual slot+offset to a physical address -- handles PT lookup.
 * If something goes wrong, disables the enable bit and fires an interrupt.
 * Returns true if succeeded.  */
static bool fharddoom_mmu_translate(FinalHardDoomState *d, int which, uint32_t va, uint64_t *res) {
	uint8_t buf[4];
	uint32_t tag = (va & FHARDDOOM_MMU_TLB_TAG_MASK) | FHARDDOOM_MMU_TLB_TAG_VALID;
	d->mmu_client_va[which] = va;
	if (d->mmu_client_tlb_tag[which] != tag) {
		/* No PTE, try the pool.  */
		int tlbidx = fharddoom_hash(tag);
		if (d->mmu_tlb_tag[tlbidx] != tag) {
			d->stats[FHARDDOOM_STAT_TLB_MISS(which)]++;
			/* Will need to fetch PTE.  First, get PT address.  */
			uint32_t pt = d->mmu_slot[FHARDDOOM_VA_SLOT(va)];
			if (!(pt & FHARDDOOM_MMU_SLOT_VALID))
				goto oops;
			uint64_t pte_addr = (uint64_t)(pt & FHARDDOOM_MMU_SLOT_PA_MASK) << FHARDDOOM_MMU_SLOT_PA_SHIFT | FHARDDOOM_VA_PTI(va) << 2;
			pci_dma_read(&d->dev, pte_addr, &buf, sizeof buf);
			d->mmu_tlb_tag[tlbidx] = tag;
			d->mmu_tlb_value[tlbidx] = le32_read(buf);
		} else {
			d->stats[FHARDDOOM_STAT_TLB_POOL_HIT(which)]++;
		}
		d->mmu_client_tlb_tag[which] = tag;
		d->mmu_client_tlb_value[which] = d->mmu_tlb_value[tlbidx];
	} else {
		d->stats[FHARDDOOM_STAT_TLB_HIT(which)]++;
	}
	uint32_t pte = d->mmu_client_tlb_value[which];
	if (!(pte & FHARDDOOM_PTE_PRESENT))
		goto oops;
	*res = (uint64_t)(pte & FHARDDOOM_PTE_PA_MASK) << FHARDDOOM_PTE_PA_SHIFT | FHARDDOOM_VA_OFF(va);
	return true;
oops:
	d->intr |= FHARDDOOM_INTR_PAGE_FAULT(which);
	return false;
}

/* Resets a given CACHE, flushing all data.  */
static void fharddoom_reset_cache(FinalHardDoomState *d, int which) {
	for (int i = 0; i < FHARDDOOM_CACHE_SIZE; i++)
		d->cache_tag[which * FHARDDOOM_CACHE_SIZE + i] &= ~FHARDDOOM_CACHE_TAG_VALID;
	d->stats[FHARDDOOM_STAT_CACHE_FLUSH(which)]++;
}

/* Reads a byte thru the CACHE, returns true if succeeded.  */
static bool fharddoom_cache_read(FinalHardDoomState *d, int which, uint32_t va, uint8_t *dst, bool speculative) {
	uint32_t offset = va & FHARDDOOM_BLOCK_MASK;
	uint32_t bva = va & ~FHARDDOOM_BLOCK_MASK;
	uint32_t set = fharddoom_hash(bva);
	uint32_t tag = bva | FHARDDOOM_CACHE_TAG_VALID;
	if (d->cache_tag[which * FHARDDOOM_CACHE_SIZE + set] != tag) {
		if (speculative) {
			d->stats[FHARDDOOM_STAT_CACHE_SPEC_MISS(which)]++;
			return false;
		}
		uint64_t pa;
		if (!fharddoom_mmu_translate(d, which + 4, bva, &pa))
			return false;
		pci_dma_read(&d->dev, pa, &d->cache_data[(which * FHARDDOOM_CACHE_SIZE + set) * FHARDDOOM_BLOCK_SIZE], FHARDDOOM_BLOCK_SIZE);
		d->cache_tag[which * FHARDDOOM_CACHE_SIZE + set] = tag;
		d->stats[FHARDDOOM_STAT_CACHE_MISS(which)]++;
	} else {
		if (speculative)
			d->stats[FHARDDOOM_STAT_CACHE_SPEC_HIT(which)]++;
		else
			d->stats[FHARDDOOM_STAT_CACHE_HIT(which)]++;
	}
	*dst = d->cache_data[(which * FHARDDOOM_CACHE_SIZE + set) * FHARDDOOM_BLOCK_SIZE + offset];
	//printf("CACHE READ %d %08x -> %02x\n", which, addr, *dst);
	return true;
}

static void fharddoom_reset(FinalHardDoomState *d, uint32_t val) {
	int i;
	if (val & FHARDDOOM_RESET_CMD) {
		d->cmd_main_setup &= ~FHARDDOOM_CMD_MAIN_SETUP_ENABLE;
		d->cmd_main_fifo_get = 0;
		d->cmd_main_fifo_put = 0;
		d->cmd_sub_fifo_get = 0;
		d->cmd_sub_fifo_put = 0;
		d->cmd_sub_len = 0;
	}
	if (val & FHARDDOOM_RESET_FE) {
		d->fe_state = 0;
		d->fe_pc = FHARDDOOM_FEMEM_CODE_BASE;
	}
	if (val & FHARDDOOM_RESET_SRD)
		d->srd_state = 0;
	if (val & FHARDDOOM_RESET_SPAN)
		d->span_state = 0;
	if (val & FHARDDOOM_RESET_COL) {
		d->col_state = 0;
		for (i = 0; i < FHARDDOOM_BLOCK_SIZE; i++)
			d->col_cols_state[i] = 0;
	}
	if (val & FHARDDOOM_RESET_FX) {
		d->fx_state = 0;
		for (i = 0; i < FHARDDOOM_BLOCK_SIZE; i++)
			d->fx_col[i] = 0;
	}
	if (val & FHARDDOOM_RESET_SWR)
		d->swr_state = 0;
	if (val & FHARDDOOM_RESET_MMU)
		fharddoom_reset_mmu(d);
	if (val & FHARDDOOM_RESET_STATS) {
		for (i = 0; i < FHARDDOOM_STATS_NUM; i++)
			d->stats[i] = 0;
	}
	if (val & FHARDDOOM_RESET_TLB)
		fharddoom_reset_tlb(d);
	if (val & FHARDDOOM_RESET_CACHE_COL_CMAP_B)
		fharddoom_reset_cache(d, FHARDDOOM_CACHE_CLIENT_COL_CMAP_B);
	if (val & FHARDDOOM_RESET_CACHE_COL_SRC)
		fharddoom_reset_cache(d, FHARDDOOM_CACHE_CLIENT_COL_SRC);
	if (val & FHARDDOOM_RESET_CACHE_SPAN_SRC)
		fharddoom_reset_cache(d, FHARDDOOM_CACHE_CLIENT_SPAN_SRC);
	if (val & FHARDDOOM_RESET_CACHE_SWR_TRANSMAP)
		fharddoom_reset_cache(d, FHARDDOOM_CACHE_CLIENT_SWR_TRANSMAP);
	if (val & FHARDDOOM_RESET_FIFO_SRDCMD)
		d->srdcmd_get = d->srdcmd_put = 0;
	if (val & FHARDDOOM_RESET_FIFO_SPANCMD)
		d->spancmd_get = d->spancmd_put = 0;
	if (val & FHARDDOOM_RESET_FIFO_COLCMD)
		d->colcmd_get = d->colcmd_put = 0;
	if (val & FHARDDOOM_RESET_FIFO_FXCMD)
		d->fxcmd_get = d->fxcmd_put = 0;
	if (val & FHARDDOOM_RESET_FIFO_SWRCMD)
		d->swrcmd_get = d->swrcmd_put = 0;
	if (val & FHARDDOOM_RESET_FIFO_COLIN)
		d->colin_get = d->colin_put = 0;
	if (val & FHARDDOOM_RESET_FIFO_FXIN)
		d->fxin_get = d->fxin_put = 0;
	if (val & FHARDDOOM_RESET_FIFO_FESEM)
		d->fesem = 0;
	if (val & FHARDDOOM_RESET_FIFO_SRDSEM)
		d->srdsem = 0;
	if (val & FHARDDOOM_RESET_FIFO_COLSEM)
		d->colsem = 0;
	if (val & FHARDDOOM_RESET_FIFO_SPANSEM)
		d->spansem = 0;
	if (val & FHARDDOOM_RESET_FIFO_SPANOUT)
		d->spanout_get = d->spanout_put = 0;
	if (val & FHARDDOOM_RESET_FIFO_COLOUT)
		d->colout_get = d->colout_put = 0;
	if (val & FHARDDOOM_RESET_FIFO_FXOUT)
		d->fxout_get = d->fxout_put = 0;
}


static uint32_t fharddoom_cmd_manual_free(FinalHardDoomState *d) {
	if (d->cmd_main_setup & FHARDDOOM_CMD_MAIN_SETUP_ENABLE)
		return 0;
	if (!fharddoom_cmd_main_fifo_empty(d) && !(d->cmd_main_state & FHARDDOOM_CMD_MAIN_STATE_MANUAL))
		return 0;
	return (d->cmd_main_fifo_get - d->cmd_main_fifo_put - 1) & (FHARDDOOM_CMD_MAIN_FIFO_SIZE - 1);
}

static void fharddoom_cmd_manual_feed(FinalHardDoomState *d, uint32_t val) {
	if (d->cmd_main_setup & FHARDDOOM_CMD_MAIN_SETUP_ENABLE) {
		d->intr |= FHARDDOOM_INTR_FEED_ERROR;
		return;
	}
	if (!fharddoom_cmd_main_fifo_empty(d) && !(d->cmd_main_state & FHARDDOOM_CMD_MAIN_STATE_MANUAL)) {
		d->intr |= FHARDDOOM_INTR_FEED_ERROR;
		return;
	}
	if (fharddoom_cmd_main_fifo_full(d)) {
		d->intr |= FHARDDOOM_INTR_FEED_ERROR;
		return;
	}
	d->cmd_main_state = FHARDDOOM_CMD_MAIN_STATE_MANUAL;
	d->cmd_main_fifo_data[d->cmd_main_fifo_put] = val;
	fharddoom_cmd_main_fifo_write(d);
}

static void fharddoom_cmd_error(FinalHardDoomState *d, uint32_t code, uint32_t data) {
	d->cmd_error_code = code;
	d->cmd_error_data = data;
	d->fe_state &= ~FHARDDOOM_FE_STATE_STATE_MASK;
	d->fe_state |= FHARDDOOM_FE_STATE_STATE_ERROR;
	d->intr |= FHARDDOOM_INTR_CMD_ERROR;
}

static void fharddoom_fe_error(FinalHardDoomState *d, uint32_t code, uint32_t data_a, uint32_t data_b) {
	d->fe_error_code = code;
	d->fe_error_data_a = data_a;
	d->fe_error_data_b = data_b;
	d->fe_state &= ~FHARDDOOM_FE_STATE_STATE_MASK;
	d->fe_state |= FHARDDOOM_FE_STATE_STATE_ERROR;
	d->intr |= FHARDDOOM_INTR_FE_ERROR;
}

static uint32_t fharddoom_cmd_main_fetch(FinalHardDoomState *d, bool header) {
	if (d->cmd_main_state & FHARDDOOM_CMD_MAIN_STATE_MANUAL) {
		if (header)
			d->cmd_info = FHARDDOOM_CMD_INFO_MANUAL;
	} else {
		uint32_t ptr = d->cmd_main_state;
		/* Get the tracking pointer, if needed.  */
		if (header) {
			int slot = d->cmd_main_setup >> FHARDDOOM_CMD_MAIN_SETUP_SLOT_SHIFT & FHARDDOOM_SLOT_MASK;
			d->cmd_info = ptr | slot << FHARDDOOM_CMD_INFO_SLOT_SHIFT;
		}
		/* Bump the tracking pointer.  */
		ptr += 4;
		ptr &= FHARDDOOM_CMD_PTR_MASK;
		/* Wrap if necessary.  */
		uint32_t wrap = d->cmd_main_setup & FHARDDOOM_CMD_MAIN_SETUP_WRAP_MASK;
		if (ptr == wrap)
			ptr = 0;
		d->cmd_main_state = ptr;
	}
	/* Get the word.  */
	uint32_t res = d->cmd_main_fifo_data[d->cmd_main_fifo_get];
	fharddoom_cmd_main_fifo_read(d);
	return res;
}

static uint32_t fharddoom_cmd_sub_fetch(FinalHardDoomState *d, bool header) {
	/* Get the tracking pointer, if needed.  */
	if (header)
		d->cmd_info = d->cmd_sub_state | FHARDDOOM_CMD_INFO_SUB;
	/* Bump the tracking pointer.  */
	uint32_t ptr = d->cmd_sub_state & FHARDDOOM_CMD_SUB_STATE_PTR_MASK;
	ptr += 4;
	ptr &= FHARDDOOM_CMD_SUB_STATE_PTR_MASK;
	d->cmd_sub_state &= ~FHARDDOOM_CMD_SUB_STATE_PTR_MASK;
	d->cmd_sub_state |= ptr;
	/* Get the word.  */
	uint32_t res = d->cmd_sub_fifo_data[d->cmd_sub_fifo_get];
	fharddoom_cmd_sub_fifo_read(d);
	return res;
}

static bool fharddoom_cmd_fetch_header(FinalHardDoomState *d, uint32_t *res) {
	if (!fharddoom_cmd_sub_fifo_empty(d)) {
		/* Read from subroutine FIFO.  */
		*res = fharddoom_cmd_sub_fetch(d, true);
		return true;
	} else if (d->cmd_sub_len) {
		/* We should read from subroutine FIFO, but it's not available right now.  */
		return false;
	} else if (!fharddoom_cmd_main_fifo_empty(d)) {
		/* Read from main FIFO.  */
		*res = fharddoom_cmd_main_fetch(d, true);
		return true;
	} else {
		/* We should read from main FIFO, but it's not available right now.  */
		return false;
	}
}

static bool fharddoom_cmd_fetch_arg(FinalHardDoomState *d, uint32_t *res) {
	if (d->cmd_info & FHARDDOOM_CMD_INFO_SUB) {
		/* Read from subroutine FIFO.  */
		if (!fharddoom_cmd_sub_fifo_empty(d)) {
			*res = fharddoom_cmd_sub_fetch(d, false);
			return true;
		} else if (d->cmd_sub_len) {
			/* Need to fetch more words.  */
			return false;
		} else {
			/* We're out of sub FIFO in the middle of command.  Not good.  */
			fharddoom_cmd_error(d, FHARDDOOM_CMD_ERROR_CODE_SUB_INCOMPLETE, d->cmd_sub_get);
			return false;
		}
	} else {
		/* Read from main FIFO.  */
		if (!fharddoom_cmd_main_fifo_empty(d)) {
			*res = fharddoom_cmd_main_fetch(d, false);
			return true;
		} else {
			/* Need to fetch more words.  */
			return false;
		}
	}
}

static void fharddoom_cmd_fence(FinalHardDoomState *d, uint32_t val) {
	val &= FHARDDOOM_CMD_FENCE_LAST_MASK;
	d->cmd_fence_last = val;
	if (d->cmd_fence_wait == val) {
		d->intr |= FHARDDOOM_INTR_FENCE_WAIT;
		d->stats[FHARDDOOM_STAT_FENCE_WAIT]++;
	}
}

static uint8_t fharddoom_mmio_readb(FinalHardDoomState *d, hwaddr addr)
{
#define REG(name, a, mask)
#define REG64(name, a)
#define REGS(name, a, num, mask)
#define REGS8(name, a, num) \
	if (addr >= a(0) && addr < a(num)) \
		return d->name[addr - a(0)];
#define FIFO(name, hname)
#define FIFO_DATA(name, hname, data, window, mask)
#define FIFO_DATA_BLOCK(name, hname, data, window) \
	if (addr >= window && addr < window + FHARDDOOM_BLOCK_SIZE) \
		return d->data[d->name##_get * FHARDDOOM_BLOCK_SIZE + (addr - window)];
#define FIFO_DATA64(name, hname, data, window)
#include "fharddoom-regs.h"
	fprintf(stderr, "fharddoom error: byte-sized read at %03x\n", (int)addr);
	return 0xff;
}

static void fharddoom_mmio_writeb(FinalHardDoomState *d, hwaddr addr, uint8_t val)
{
#define REG(name, a, mask)
#define REG64(name, a)
#define REGS(name, a, num, mask)
#define REGS8(name, a, num) \
	if (addr >= a(0) && addr < a(num)) { \
		d->name[addr - a(0)] = val; \
		return; \
	}
#define FIFO(name, hname)
#define FIFO_DATA(name, hname, data, window, mask)
#define FIFO_DATA_BLOCK(name, hname, data, window) \
	if (addr >= window && addr < window + FHARDDOOM_BLOCK_SIZE) { \
		d->data[d->name##_put * FHARDDOOM_BLOCK_SIZE + (addr - window)] = val; \
		return; \
	}
#define FIFO_DATA64(name, hname, data, window)
#include "fharddoom-regs.h"
	fprintf(stderr, "fharddoom error: byte-sized write at %03x, value %02x\n", (int)addr, val);
}

static uint32_t fharddoom_mmio_readl(FinalHardDoomState *d, hwaddr addr)
{
	switch (addr) {
#define REG(name, a, mask) \
	case a: return d->name;
#define REG64(name, a) \
	case a: return d->name; \
	case a+4: return d->name >> 32;
#define REGS(name, a, num, mask)
#define REGS8(name, a, num)
#define FIFO(name, hname) \
	case hname##_GET: return d->name##_get; \
	case hname##_PUT: return d->name##_put;
#define FIFO_DATA(name, hname, data, window, mask) \
	case window: return d->data[d->name##_get];
#define FIFO_DATA64(name, hname, data, window) \
	case window: return d->data[d->name##_get]; \
	case window + 4: return d->data[d->name##_get] >> 32;
#define FIFO_DATA_BLOCK(name, hname, data, window)
#include "fharddoom-regs.h"
	case FHARDDOOM_STATUS: return d->status;
	case FHARDDOOM_CMD_MANUAL_FREE: return fharddoom_cmd_manual_free(d);
	case FHARDDOOM_FE_CODE_WINDOW: {
		uint32_t res = le32_read(d->fe_code + d->fe_code_addr);
		d->fe_code_addr += 4;
		d->fe_code_addr &= FHARDDOOM_FE_CODE_ADDR_MASK;
		return res;
	}
	case FHARDDOOM_FE_DATA_WINDOW: {
		uint32_t res = le32_read(d->fe_data + d->fe_data_addr);
		d->fe_data_addr += 4;
		d->fe_data_addr &= FHARDDOOM_FE_DATA_ADDR_MASK;
		return res;
	}
	}
#define REG(name, a, mask)
#define REG64(name, a)
#define REGS(name, a, num, mask) \
	if (addr >= a(0) && addr < a(num) && (addr - a(0)) % (a(1) - a(0)) == 0) \
		return d->name[(addr - a(0)) / (a(1) - a(0))];
#define REGS8(name, a, num)
#define FIFO(name, hname)
#define FIFO_DATA(name, hname, data, window, mask)
#define FIFO_DATA64(name, hname, data, window)
#define FIFO_DATA_BLOCK(name, hname, data, window)
#include "fharddoom-regs.h"
	fprintf(stderr, "fharddoom error: invalid register read at %03x\n", (int)addr);
	return 0xffffffff;
}

static void fharddoom_mmio_writel(FinalHardDoomState *d, hwaddr addr, uint32_t val)
{
	/* Has to be a separate switch, because it duplicates values from the header. */
	switch (addr) {
		case FHARDDOOM_INTR:
			d->intr &= ~val;
			return;
		case FHARDDOOM_FE_REG(0):
			/* Nope.  */
			return;
	}
	switch (addr) {
#define REG(name, a, mask) \
	case a: d->name = val & mask; return;
#define REG64(name, a) \
	case a: d->name = (d->name & 0xffffffff00000000ull) | (uint32_t)val; return; \
	case a + 4: d->name = (d->name & 0xffffffffull) | (uint64_t)val << 32; return;
#define REGS(name, a, num, mask)
#define REGS8(name, a, num)
#define FIFO(name, hname) \
	case hname##_GET: d->name##_get = val & (hname##_SIZE - 1); return; \
	case hname##_PUT: d->name##_put = val & (hname##_SIZE - 1); return;
#define FIFO_DATA(name, hname, data, window, mask) \
	case window: d->data[d->name##_put] = val & mask; return;
#define FIFO_DATA64(name, hname, data, window) \
	case window: d->data[d->name##_put] = (d->data[d->name##_put] & 0xffffffff00000000ull) | (uint32_t)val; return; \
	case window + 4: d->data[d->name##_put] = (d->data[d->name##_put] & 0xffffffffull) | (uint64_t)val << 32; return;
#define FIFO_DATA_BLOCK(name, hname, data, window)
#include "fharddoom-regs.h"
	case FHARDDOOM_RESET: fharddoom_reset(d, val); return;
	case FHARDDOOM_CMD_MANUAL_FEED:
		fharddoom_cmd_manual_feed(d, val);
		return;
	case FHARDDOOM_FE_CODE_WINDOW: {
		le32_write(d->fe_code + d->fe_code_addr, val);
		d->fe_code_addr += 4;
		d->fe_code_addr &= FHARDDOOM_FE_CODE_ADDR_MASK;
		return;
	}
	case FHARDDOOM_FE_DATA_WINDOW: {
		le32_write(d->fe_data + d->fe_data_addr, val);
		d->fe_data_addr += 4;
		d->fe_data_addr &= FHARDDOOM_FE_DATA_ADDR_MASK;
		return;
	}
	}
#define REG(name, a, mask)
#define REG64(name, a)
#define REGS(name, a, num, mask) \
	if (addr >= a(0) && addr < a(num) && (addr - a(0)) % (a(1) - a(0)) == 0) { \
		d->name[(addr - a(0)) / (a(1) - a(0))] = val & mask; \
		return; \
	}
#define REGS8(name, a, num)
#define FIFO(name, hname)
#define FIFO_DATA(name, hname, data, window, mask)
#define FIFO_DATA64(name, hname, data, window)
#define FIFO_DATA_BLOCK(name, hname, data, window)
#include "fharddoom-regs.h"
	fprintf(stderr, "fharddoom error: invalid register write at %03x, value %08x\n", (int)addr, val);
}

static uint64_t fharddoom_mmio_read(void *opaque, hwaddr addr, unsigned size) {
	uint64_t res = ~0ULL;
	FinalHardDoomState *d = opaque;
	qemu_mutex_lock(&d->mutex);
	if (size == 1) {
		res = fharddoom_mmio_readb(d, addr);
	} else if (size == 4) {
		res = fharddoom_mmio_readl(d, addr);
	} else {
		fprintf(stderr, "fharddoom error: invalid register read of size %u at %03x\n", size, (int)addr);
	}
	d->stats[FHARDDOOM_STAT_MMIO_READ]++;
	fharddoom_status_update(d);
	qemu_cond_signal(&d->cond);
	qemu_mutex_unlock(&d->mutex);
	return res;
}

static void fharddoom_mmio_write(void *opaque, hwaddr addr, uint64_t val,
                unsigned size)
{
	FinalHardDoomState *d = opaque;
	qemu_mutex_lock(&d->mutex);
	if (size == 1) {
		fharddoom_mmio_writeb(d, addr, val);
	} else if (size == 4) {
		fharddoom_mmio_writel(d, addr, val);
	} else {
		fprintf(stderr, "fharddoom error: invalid register write of size %u at %03x\n", size, (int)addr);
	}
	d->stats[FHARDDOOM_STAT_MMIO_WRITE]++;
	fharddoom_status_update(d);
	qemu_cond_signal(&d->cond);
	qemu_mutex_unlock(&d->mutex);
}

static const MemoryRegionOps fharddoom_mmio_ops = {
	.read = fharddoom_mmio_read,
	.write = fharddoom_mmio_write,
	.endianness = DEVICE_LITTLE_ENDIAN,
};

/* Power-up reset of the device.  */
static void fharddoom_power_reset(DeviceState *ds)
{
	FinalHardDoomState *d = FHARDDOOM_DEV(ds);
	int i;
	qemu_mutex_lock(&d->mutex);
#define REG(name, addr, mask) \
	d->name = mrand48() & mask;
#define REG64(name, addr) \
	d->name = (uint64_t)mrand48() << 32 | (uint32_t)mrand48();
#define REGS(name, addr, num, mask) \
	for (i = 0; i < num; i++) \
		d->name[i] = mrand48() & mask;
#define REGS8(name, addr, num) \
	for (i = 0; i < num; i++) \
		d->name[i] = mrand48();
#define FIFO(name, hname) \
	d->name##_get = mrand48() & (hname##_SIZE - 1); \
	d->name##_put = mrand48() & (hname##_SIZE - 1);
#define FIFO_DATA(name, hname, data, window, mask) \
	for (i = 0; i < hname##_SIZE; i++) \
		d->data[i] = mrand48() & mask;
#define FIFO_DATA64(name, hname, data, window) \
	for (i = 0; i < hname##_SIZE; i++) \
		d->data[i] = (uint64_t)mrand48() << 32 | (uint32_t)mrand48();
#define FIFO_DATA_BLOCK(name, hname, data, window) \
	for (i = 0; i < hname##_SIZE * FHARDDOOM_BLOCK_SIZE; i++) \
		d->data[i] = mrand48();
#include "fharddoom-regs.h"
	for (i = 0; i < FHARDDOOM_FEMEM_CODE_SIZE; i++)
		d->fe_code[i] = mrand48();
	for (i = 0; i < FHARDDOOM_FEMEM_DATA_SIZE; i++)
		d->fe_data[i] = mrand48();
	/* These registers play fair.  */
	d->enable = 0;
	d->intr_enable = 0;
	/* No force can change this register.  */
	d->fe_regs[0] = 0;
	fharddoom_status_update(d);
	qemu_mutex_unlock(&d->mutex);
}

/* Runs CMD (main) for some time.  Returns true if anything has been done.  */
static bool fharddoom_run_cmd_main(FinalHardDoomState *d) {
	if (!(d->enable & FHARDDOOM_ENABLE_CMD))
		return false;
	if (!(d->cmd_main_setup & FHARDDOOM_CMD_MAIN_SETUP_ENABLE))
		return false;
	if (d->cmd_main_get == d->cmd_main_put)
		return false;
	if (!fharddoom_cmd_main_fifo_empty(d) && (d->cmd_main_state & FHARDDOOM_CMD_MAIN_STATE_MANUAL))
		return false;
	if (fharddoom_cmd_main_fifo_full(d))
		return false;
	if (fharddoom_cmd_main_fifo_empty(d))
		d->cmd_main_state = d->cmd_main_get;
	int slot = d->cmd_main_setup >> FHARDDOOM_CMD_MAIN_SETUP_SLOT_SHIFT & FHARDDOOM_SLOT_MASK;
	uint32_t wrap = d->cmd_main_setup & FHARDDOOM_CMD_MAIN_SETUP_WRAP_MASK;
	uint32_t va = FHARDDOOM_VA(d->cmd_main_get, slot);
	uint64_t pa;
	uint8_t buf[0x40];
	if (!fharddoom_mmu_translate(d, FHARDDOOM_MMU_CLIENT_CMD_MAIN, va, &pa)) {
		d->enable &= ~FHARDDOOM_ENABLE_CMD;
		return false;
	}
	uint32_t rsz = sizeof buf;
	uint32_t to_page_end = 0x1000 - (d->cmd_main_get & 0xfff);
	if (rsz > to_page_end)
		rsz = to_page_end;
	pci_dma_read(&d->dev, pa, &buf, rsz);
	d->stats[FHARDDOOM_STAT_CMD_BLOCK]++;
	uint32_t pos = 0;
	while (pos < rsz) {
		uint32_t word = le32_read(buf + pos);
		d->cmd_main_fifo_data[d->cmd_main_fifo_put] = word;
		fharddoom_cmd_main_fifo_write(d);
		d->stats[FHARDDOOM_STAT_CMD_WORD]++;
		pos += 4;
		d->cmd_main_get += 4;
		d->cmd_main_get &= FHARDDOOM_CMD_PTR_MASK;
		if (d->cmd_main_get == wrap) {
			d->cmd_main_get = 0;
			return true;
		}
		if (d->cmd_main_get == d->cmd_main_put)
			return true;
		if (fharddoom_cmd_main_fifo_full(d))
			return true;
	}
	return true;
}

/* Runs CMD (subroutine) for some time.  Returns true if anything has been done.  */
static bool fharddoom_run_cmd_sub(FinalHardDoomState *d) {
	if (!(d->enable & FHARDDOOM_ENABLE_CMD))
		return false;
	if (!d->cmd_sub_len)
		return false;
	if (fharddoom_cmd_sub_fifo_full(d))
		return false;
	int slot = d->cmd_sub_state >> FHARDDOOM_CMD_SUB_STATE_SLOT_SHIFT & FHARDDOOM_SLOT_MASK;
	uint32_t va = FHARDDOOM_VA(d->cmd_sub_get, slot);
	uint64_t pa;
	uint8_t buf[0x40];
	if (!fharddoom_mmu_translate(d, FHARDDOOM_MMU_CLIENT_CMD_SUB, va, &pa)) {
		d->enable &= ~FHARDDOOM_ENABLE_CMD;
		return false;
	}
	uint32_t rsz = sizeof buf;
	if (rsz > d->cmd_sub_len)
		rsz = d->cmd_sub_len;
	uint32_t to_page_end = 0x1000 - (d->cmd_sub_get & 0xfff);
	if (rsz > to_page_end)
		rsz = to_page_end;
	pci_dma_read(&d->dev, pa, &buf, rsz);
	d->stats[FHARDDOOM_STAT_CMD_BLOCK]++;
	uint32_t pos = 0;
	while (pos < rsz) {
		uint32_t word = le32_read(buf + pos);
		d->cmd_sub_fifo_data[d->cmd_sub_fifo_put] = word;
		fharddoom_cmd_sub_fifo_write(d);
		d->cmd_sub_get += 4;
		d->cmd_sub_get &= FHARDDOOM_CMD_SUB_GET_MASK;
		d->cmd_sub_len -= 4;
		pos += 4;
		d->stats[FHARDDOOM_STAT_CMD_WORD]++;
		if (fharddoom_cmd_sub_fifo_full(d))
			return true;
	}
	return true;
}

/* Runs the FE for some time.  Returns true if anything has been done.  */
static bool fharddoom_run_fe(FinalHardDoomState *d) {
	bool any = false;
	if (!(d->enable & FHARDDOOM_ENABLE_FE))
		return any;
	int num_insns = 0x1000;
	while (num_insns--) {
		uint32_t state = d->fe_state & FHARDDOOM_FE_STATE_STATE_MASK;
		uint32_t sdst = (d->fe_state & FHARDDOOM_FE_STATE_DST_MASK) >> FHARDDOOM_FE_STATE_DST_SHIFT;
		uint32_t cmd = (d->fe_state & FHARDDOOM_FE_STATE_CMD_MASK) >> FHARDDOOM_FE_STATE_CMD_SHIFT;
		uint32_t tmp;
		switch (state) {
			case FHARDDOOM_FE_STATE_STATE_RUNNING:
				break;
			case FHARDDOOM_FE_STATE_STATE_ERROR:
				return any;
			case FHARDDOOM_FE_STATE_STATE_CMD_FETCH_HEADER:
				if (!fharddoom_cmd_fetch_header(d, &tmp))
					return any;
				if (sdst)
					d->fe_regs[sdst] = tmp;
				break;
			case FHARDDOOM_FE_STATE_STATE_CMD_FETCH_ARG:
				if (!fharddoom_cmd_fetch_arg(d, &tmp))
					return any;
				if (sdst)
					d->fe_regs[sdst] = tmp;
				break;
			case FHARDDOOM_FE_STATE_STATE_SRDCMD:
				if (fharddoom_srdcmd_full(d))
					return any;
				d->srdcmd_cmd[d->srdcmd_put] = cmd;
				d->srdcmd_data[d->srdcmd_put] = d->fe_write_data;
				fharddoom_srdcmd_write(d);
				break;
			case FHARDDOOM_FE_STATE_STATE_SPANCMD:
				if (fharddoom_spancmd_full(d))
					return any;
				d->spancmd_cmd[d->spancmd_put] = cmd;
				d->spancmd_data[d->spancmd_put] = d->fe_write_data;
				fharddoom_spancmd_write(d);
				break;
			case FHARDDOOM_FE_STATE_STATE_COLCMD:
				if (fharddoom_colcmd_full(d))
					return any;
				d->colcmd_cmd[d->colcmd_put] = cmd;
				d->colcmd_data[d->colcmd_put] = d->fe_write_data;
				fharddoom_colcmd_write(d);
				break;
			case FHARDDOOM_FE_STATE_STATE_FXCMD:
				if (fharddoom_fxcmd_full(d))
					return any;
				d->fxcmd_cmd[d->fxcmd_put] = cmd;
				d->fxcmd_data[d->fxcmd_put] = d->fe_write_data;
				fharddoom_fxcmd_write(d);
				break;
			case FHARDDOOM_FE_STATE_STATE_SWRCMD:
				if (fharddoom_swrcmd_full(d))
					return any;
				d->swrcmd_cmd[d->swrcmd_put] = cmd;
				d->swrcmd_data[d->swrcmd_put] = d->fe_write_data;
				fharddoom_swrcmd_write(d);
				break;
			case FHARDDOOM_FE_STATE_STATE_FESEM:
				if (!d->fesem)
					return any;
				if (sdst)
					d->fe_regs[sdst] = 0;
				d->fesem = 0;
				break;
		}
		/* If we got past this, we should now be in the running state.  */
		d->fe_state &= ~FHARDDOOM_FE_STATE_STATE_MASK;
		/* One way or another, we're going to do something.  */
		any = true;
		if (d->fe_pc < FHARDDOOM_FEMEM_CODE_BASE || d->fe_pc >= FHARDDOOM_FEMEM_CODE_BASE + FHARDDOOM_FEMEM_CODE_SIZE) {
			fharddoom_fe_error(d, FHARDDOOM_FE_ERROR_CODE_BUS_ERROR_EXEC, d->fe_pc, 0);
			return any;
			
		}
		d->stats[FHARDDOOM_STAT_FE_INSN]++;
		uint32_t new_pc = d->fe_pc + 4;
		uint32_t insn = le32_read(d->fe_code + (d->fe_pc - FHARDDOOM_FEMEM_CODE_BASE));
		uint32_t op = insn & 0x7f;
		uint32_t rd = insn >> 7 & 0x1f;
		uint32_t f3 = insn >> 12 & 7;
		uint32_t rs1 = insn >> 15 & 0x1f;
		uint32_t rs2 = insn >> 20 & 0x1f;
		uint32_t f7 = insn >> 25 & 0x7f;
		uint32_t imm_u = insn & 0xfffff000;
		uint32_t imm_i = insn >> 20 & 0xfff;
		if (imm_i & 0x800)
			imm_i |= 0xfffff000;
		uint32_t imm_s = (imm_i & ~0x1f) | rd;
		uint32_t imm_b = imm_s & ~0x801;
		imm_b |= (imm_s & 1) << 11;
		uint32_t imm_j = (insn & 0x000ff000) | (insn >> 20 & 0x7fe) | (insn >> 20 & 1) << 11;
		if (insn >> 31 & 1)
			imm_j |= 0xfff00000;
		switch(op) {
			case 0x03: {
				/* LOAD */
				uint32_t addr = d->fe_regs[rs1] + imm_i;
				uint32_t r;
				d->stats[FHARDDOOM_STAT_FE_LOAD]++;
				switch (f3) {
					case 0:
						/* LB */
					case 4:
						/* LBU */
						if (addr >= FHARDDOOM_FEMEM_CODE_BASE && addr < FHARDDOOM_FEMEM_CODE_BASE + FHARDDOOM_FEMEM_CODE_SIZE)

							r = d->fe_code[addr - FHARDDOOM_FEMEM_CODE_BASE];
						else if (addr >= FHARDDOOM_FEMEM_DATA_BASE && addr < FHARDDOOM_FEMEM_DATA_BASE + FHARDDOOM_FEMEM_DATA_SIZE)

							r = d->fe_data[addr - FHARDDOOM_FEMEM_DATA_BASE];
						else
							goto bus_error_load;
						if (f3 == 0 && r & 0x80)
							r |= 0xffffff00;
						break;
					case 1:
						/* LH */
					case 5:
						/* LHU */
						if (addr & 1)
							goto bus_error_load;
						if (addr >= FHARDDOOM_FEMEM_CODE_BASE && addr < FHARDDOOM_FEMEM_CODE_BASE + FHARDDOOM_FEMEM_CODE_SIZE)

							r = le16_read(d->fe_code + (addr - FHARDDOOM_FEMEM_CODE_BASE));
						else if (addr >= FHARDDOOM_FEMEM_DATA_BASE && addr < FHARDDOOM_FEMEM_DATA_BASE + FHARDDOOM_FEMEM_DATA_SIZE)

							r = le16_read(d->fe_data + (addr - FHARDDOOM_FEMEM_DATA_BASE));
						else
							goto bus_error_load;
						if (f3 == 0 && r & 0x8000)
							r |= 0xffff0000;
						break;
					case 2:
						/* LW */
						if (addr & 3)
							goto bus_error_load;
						if (addr >= FHARDDOOM_FEMEM_CODE_BASE && addr < FHARDDOOM_FEMEM_CODE_BASE + FHARDDOOM_FEMEM_CODE_SIZE)

							r = le32_read(d->fe_code + (addr - FHARDDOOM_FEMEM_CODE_BASE));
						else if (addr >= FHARDDOOM_FEMEM_DATA_BASE && addr < FHARDDOOM_FEMEM_DATA_BASE + FHARDDOOM_FEMEM_DATA_SIZE)

							r = le32_read(d->fe_data + (addr - FHARDDOOM_FEMEM_DATA_BASE));
						else if (addr == FHARDDOOM_FEMEM_CMD_FETCH_HEADER) {
							state = FHARDDOOM_FE_STATE_STATE_CMD_FETCH_HEADER;
							goto long_load;
						}
						else if (addr == FHARDDOOM_FEMEM_CMD_FETCH_ARG) {
							state = FHARDDOOM_FE_STATE_STATE_CMD_FETCH_ARG;
							goto long_load;
						}
						else if (addr == FHARDDOOM_FEMEM_CMD_INFO)
							r = d->cmd_info;
						else if (addr == FHARDDOOM_FEMEM_FESEM) {
							state = FHARDDOOM_FE_STATE_STATE_FESEM;
							goto long_load;
						}
						else
							goto bus_error_load;
						break;
					default:
						goto illegal;
					bus_error_load:
						fharddoom_fe_error(d, FHARDDOOM_FE_ERROR_CODE_BUS_ERROR_LOAD, addr, 0);
						return true;
					long_load:
						d->fe_state &= ~FHARDDOOM_FE_STATE_STATE_MASK;
						d->fe_state &= ~FHARDDOOM_FE_STATE_DST_MASK;
						d->fe_state |= state;
						d->fe_state |= rd << FHARDDOOM_FE_STATE_DST_SHIFT;
						goto next_insn;
				}
				if (rd)
					d->fe_regs[rd] = r;
				break;
				}
			case 0x23: {
				/* STORE */
				uint32_t addr = d->fe_regs[rs1] + imm_s;
				uint32_t val = d->fe_regs[rs2];
				uint32_t cmd = addr >> 2 & FHARDDOOM_FIFO_CMD_MASK;
				d->stats[FHARDDOOM_STAT_FE_STORE]++;
				switch (f3) {
					case 0:
						/* SB */
						if (addr >= FHARDDOOM_FEMEM_DATA_BASE && addr < FHARDDOOM_FEMEM_DATA_BASE + FHARDDOOM_FEMEM_DATA_SIZE)

							d->fe_data[addr - FHARDDOOM_FEMEM_DATA_BASE] = val;
						else
							goto bus_error_store;
						break;
					case 1:
						/* SH */
						if (addr & 1)
							goto bus_error_store;
						if (addr >= FHARDDOOM_FEMEM_DATA_BASE && addr < FHARDDOOM_FEMEM_DATA_BASE + FHARDDOOM_FEMEM_DATA_SIZE)

							le16_write(d->fe_data + (addr - FHARDDOOM_FEMEM_DATA_BASE), val);
						else
							goto bus_error_store;
						break;
					case 2:
						/* SW */
						if (addr & 3)
							goto bus_error_store;
						if (addr >= FHARDDOOM_FEMEM_DATA_BASE && addr < FHARDDOOM_FEMEM_DATA_BASE + FHARDDOOM_FEMEM_DATA_SIZE)

							le32_write(d->fe_data + (addr - FHARDDOOM_FEMEM_DATA_BASE), val);
						else if (addr == FHARDDOOM_FEMEM_CMD_CALL_SLOT) {
							d->cmd_sub_state &= FHARDDOOM_CMD_SUB_STATE_PTR_MASK;
							d->cmd_sub_state |= val << FHARDDOOM_CMD_SUB_STATE_SLOT_SHIFT & FHARDDOOM_CMD_SUB_STATE_SLOT_MASK;
						}
						else if (addr == FHARDDOOM_FEMEM_CMD_CALL_ADDR) {
							d->cmd_sub_state &= FHARDDOOM_CMD_SUB_STATE_SLOT_MASK;
							d->cmd_sub_state |= val & FHARDDOOM_CMD_SUB_GET_MASK;
							d->cmd_sub_get = val & FHARDDOOM_CMD_SUB_GET_MASK;
						}
						else if (addr == FHARDDOOM_FEMEM_CMD_CALL_LEN) {
							d->cmd_sub_len = val & FHARDDOOM_CMD_SUB_LEN_MASK;
						}
						else if (addr == FHARDDOOM_FEMEM_CMD_FENCE) {
							fharddoom_cmd_fence(d, val);
						}
						else if (addr == FHARDDOOM_FEMEM_CMD_FLUSH_CACHES) {
							int i;
							for (i = 0; i < FHARDDOOM_CACHE_CLIENT_NUM; i++)
								if (val >> i & 1)
									fharddoom_reset_cache(d, i);
						}
						else if ((addr & ~0x3c) == FHARDDOOM_FEMEM_CMD_ERROR(0)) {
							fharddoom_cmd_error(d, addr >> 2 & 0xf, val);
							return true;
						}
						else if ((addr & ~0x3c) == FHARDDOOM_FEMEM_SRDCMD(0)) {
							state = FHARDDOOM_FE_STATE_STATE_SRDCMD;
							goto long_store;
						}
						else if ((addr & ~0x3c) == FHARDDOOM_FEMEM_SPANCMD(0)) {
							state = FHARDDOOM_FE_STATE_STATE_SPANCMD;
							goto long_store;
						}
						else if ((addr & ~0x3c) == FHARDDOOM_FEMEM_COLCMD(0)) {
							state = FHARDDOOM_FE_STATE_STATE_COLCMD;
							goto long_store;
						}
						else if ((addr & ~0x3c) == FHARDDOOM_FEMEM_FXCMD(0)) {
							state = FHARDDOOM_FE_STATE_STATE_FXCMD;
							goto long_store;
						}
						else if ((addr & ~0x3c) == FHARDDOOM_FEMEM_SWRCMD(0)) {
							state = FHARDDOOM_FE_STATE_STATE_SWRCMD;
							goto long_store;
						}
						else if ((addr & ~0xfc) == FHARDDOOM_FEMEM_MMU_BIND(0)) {
							fharddoom_mmu_bind(d, addr >> 2 & 0x3f, val);
						}
						else if ((addr & ~0x7c) == FHARDDOOM_FEMEM_STAT_BUMP(0)) {
							d->stats[addr >> 2 & 0x1f] += val;
						}
						else
							goto bus_error_store;
						break;
					default:
						goto illegal;
					bus_error_store:
						fharddoom_fe_error(d, FHARDDOOM_FE_ERROR_CODE_BUS_ERROR_STORE, addr, val);
						return true;
					long_store:
						d->fe_state &= ~FHARDDOOM_FE_STATE_STATE_MASK;
						d->fe_state &= ~FHARDDOOM_FE_STATE_CMD_MASK;
						d->fe_state |= state;
						d->fe_state |= cmd << FHARDDOOM_FE_STATE_CMD_SHIFT;
						d->fe_write_data = val;
						goto next_insn;
				}
				break;
				}
			case 0x0f:
				/* MISC-MEM */
				if (f3 == 0) {
					/* FENCE */
				} else
					goto illegal;
				break;
			case 0x13: {
				/* OP-IMM */
				uint32_t s1 = d->fe_regs[rs1];
				uint32_t r;
				switch (f3) {
					case 0:
						/* ADDI */
						r = s1 + imm_i;
						break;
					case 1:
						/* SLLI */
						if (f7 == 0)
							r = s1 << rs2;
						else
							goto illegal;
						break;
					case 2:
						/* SLTI */
						r = (s1 ^ 0x80000000) < (imm_i ^ 0x80000000);
						break;
					case 3:
						/* SLTIU */
						r = s1 < imm_i;
						break;
					case 4:
						/* XORI */
						r = s1 ^ imm_i;
						break;
					case 5:
						if (f7 == 0) {
							/* SRLI */
							r = s1 >> rs2;
						} else if (f7 == 0x20) {
							/* SRAI */
							r = s1 >> rs2;
							if (rs2 && s1 >> 31 & 1)
								r |= 0xffffffff << (32 - rs2);
						} else
							goto illegal;
						break;
					case 6:
						/* ORI */
						r = s1 | imm_i;
						break;
					case 7:
						/* ANDI */
						r = s1 & imm_i;
						break;
					default:
						goto illegal;
				}
				if (rd)
					d->fe_regs[rd] = r;
				break;
				}
			case 0x17:
				/* AUIPC */
				if (rd)
					d->fe_regs[rd] = imm_u + d->fe_pc;
				break;
			case 0x33: {
				/* OP */
				uint32_t s1 = d->fe_regs[rs1];
				uint32_t s2 = d->fe_regs[rs2];
				uint64_t ls1;
				uint64_t ls2;
				uint32_t r;
				bool sign;
				switch (f7 << 3 | f3) {
					case 0x000:
						/* ADD */
						r = s1 + s2;
						break;
					case 0x100:
						/* SUB */
						r = s1 - s2;
						break;
					case 0x001:
						/* SLL */
						r = s1 << (s2 & 0x1f);
						break;
					case 0x002:
						/* SLT */
						r = (s1 ^ 0x80000000) < (s2 ^ 0x80000000);
						break;
					case 0x003:
						/* SLTU */
						r = s1 < s2;
						break;
					case 0x004:
						/* XOR */
						r = s1 ^ s2;
						break;
					case 0x005:
						/* SRL */
						r = s1 >> (s2 & 0x1f);
						break;
					case 0x105:
						/* SRA */
						r = s1 >> (s2 & 0x1f);
						if (s2 & 0x1f && s1 >> 31 & 1)
							r |= 0xffffffff << (32 - (s2 & 0x1f));
						break;
					case 0x006:
						/* OR */
						r = s1 | s2;
						break;
					case 0x007:
						/* AND */
						r = s1 & s2;
						break;
					case 0x008:
						/* MUL */
						r = s1 * s2;
						break;
					case 0x009:
						/* MULH */
						ls1 = s1;
						ls2 = s2;
						if (s1 & 0x80000000)
							ls1 |= 0xffffffff00000000ull;
						if (s2 & 0x80000000)
							ls2 |= 0xffffffff00000000ull;
						r = ls1 * ls2 >> 32;
						break;
					case 0x00a:
						/* MULHSU */
						ls1 = s1;
						ls2 = s2;
						if (s1 & 0x80000000)
							ls1 |= 0xffffffff00000000ull;
						r = ls1 * ls2 >> 32;
						break;
					case 0x00b:
						/* MULHU */
						ls1 = s1;
						ls2 = s2;
						r = ls1 * ls2 >> 32;
						break;
					case 0x00c:
						/* DIV */
						sign = false;
						if (s1 & 0x80000000)
							s1 = -s1, sign = !sign;
						if (s2 & 0x80000000)
							s2 = -s2, sign = !sign;
						if (s2)
							r = s1 / s2;
						else
							r = 0xffffffff;
						if (sign)
							r = -r;
						break;
					case 0x00d:
						/* DIVU */
						if (s2)
							r = s1 / s2;
						else
							r = 0xffffffff;
						break;
					case 0x00e:
						/* REM */
						sign = false;
						if (s1 & 0x80000000)
							s1 = -s1, sign = !sign;
						if (s2 & 0x80000000)
							s2 = -s2;
						if (s2)
							r = s1 % s2;
						else
							r = s1;
						if (sign)
							r = -r;
						break;
					case 0x00f:
						/* REMU */
						if (s2)
							r = s1 % s2;
						else
							r = s1;
						break;
					default:
						goto illegal;
				}
				if (rd)
					d->fe_regs[rd] = r;
				break;
				}
			case 0x37:
				/* LUI */
				if (rd)
					d->fe_regs[rd] = imm_u;
				break;
			case 0x63: {
				/* BRANCH */
				bool cond;
				uint32_t s1 = d->fe_regs[rs1];
				uint32_t s2 = d->fe_regs[rs2];
				switch (f3) {
					case 0:
						/* BEQ */
						cond = s1 == s2;
						break;
					case 1:
						/* BNE */
						cond = s1 != s2;
						break;
					case 4:
						/* BLT */
						cond = (s1 ^ 0x80000000) < (s2 ^ 0x80000000);
						break;
					case 5:
						/* BGE */
						cond = (s1 ^ 0x80000000) >= (s2 ^ 0x80000000);
						break;
					case 6:
						/* BLTU */
						cond = s1 < s2;
						break;
					case 7:
						/* BGEU */
						cond = s1 >= s2;
						break;
					default:
						goto illegal;
				}
				if (cond)
					new_pc = d->fe_pc + imm_b;
				break;
				}
			case 0x67:
				/* JALR */
				if (f3 != 0)
					goto illegal;
				if (rd)
					d->fe_regs[rd] = d->fe_pc + 4;
				new_pc = (d->fe_regs[rs1] + imm_i) & ~1;
				break;
			case 0x6f:
				/* JAL */
				if (rd)
					d->fe_regs[rd] = d->fe_pc + 4;
				new_pc = d->fe_pc + imm_j;
				break;
			default:
illegal:
				fharddoom_fe_error(d, FHARDDOOM_FE_ERROR_CODE_ILLEGAL_INSTRUCTION, d->fe_pc, insn);
				return true;
		}
next_insn:
		if (new_pc & 3) {
			fharddoom_fe_error(d, FHARDDOOM_FE_ERROR_CODE_UNALIGNED_INSTRUCTION, d->fe_pc, new_pc);
			return true;
		}
		d->fe_pc = new_pc;
	}
	return any;
}

/* Runs SRD for some time.  Returns true if anything has been done.  */
static bool fharddoom_run_srd(FinalHardDoomState *d) {
	if (!(d->enable & FHARDDOOM_ENABLE_SRD))
		return false;
	bool any = false;
	while (true) {
		if (d->srd_state & FHARDDOOM_SRD_STATE_SRDSEM) {
			if (!d->srdsem)
				return any;
			d->srdsem = 0;
			d->srd_state &= ~FHARDDOOM_SRD_STATE_SRDSEM;
			any = true;
		} else if (d->srd_state & FHARDDOOM_SRD_STATE_FESEM) {
			if (d->fesem)
				return any;
			d->fesem = 1;
			d->srd_state &= ~FHARDDOOM_SRD_STATE_FESEM;
			any = true;
		} else if (d->srd_state & FHARDDOOM_SRD_STATE_READ_LENGTH_MASK) {
			uint8_t *ptr;
			if (d->srd_state & FHARDDOOM_SRD_STATE_COL) {
				if (fharddoom_colin_full(d))
					return any;
				ptr = &d->colin_data[d->colin_put * FHARDDOOM_BLOCK_SIZE];
			} else {
				if (fharddoom_fxin_full(d))
					return any;
				ptr = &d->fxin_data[d->fxin_put * FHARDDOOM_BLOCK_SIZE];
			}
			uint64_t pa;
			if (!fharddoom_mmu_translate(d, FHARDDOOM_MMU_CLIENT_SRD, d->srd_src_va, &pa)) {
				d->enable &= ~FHARDDOOM_ENABLE_SRD;
				return any;
			}
			pci_dma_read(&d->dev, pa, ptr, FHARDDOOM_BLOCK_SIZE);
			if (d->srd_state & FHARDDOOM_SRD_STATE_COL)
				fharddoom_colin_write(d);
			else
				fharddoom_fxin_write(d);
			d->srd_src_va = FHARDDOOM_VA(d->srd_src_va + d->srd_src_pitch, FHARDDOOM_VA_SLOT(d->srd_src_va));
			d->srd_state--;
			d->stats[FHARDDOOM_STAT_SRD_BLOCK]++;
			any = true;
		} else {
			/* No command. get one.  */
			if (fharddoom_srdcmd_empty(d))
				return any;
			uint32_t cmd = d->srdcmd_cmd[d->srdcmd_get];
			uint32_t data = d->srdcmd_data[d->srdcmd_get];
			fharddoom_srdcmd_read(d);
			//printf("SRDCMD %08x %08x\n", cmd, data);
			d->stats[FHARDDOOM_STAT_SRD_CMD]++;
			any = true;
			switch (cmd) {
				case FHARDDOOM_SRDCMD_TYPE_SRC_SLOT:
					d->srd_src_va = FHARDDOOM_VA(d->srd_src_va, data & FHARDDOOM_SLOT_MASK);
					break;
				case FHARDDOOM_SRDCMD_TYPE_SRC_PTR:
					d->srd_src_va = FHARDDOOM_VA(data & FHARDDOOM_BLOCK_PTR_MASK, FHARDDOOM_VA_SLOT(d->srd_src_va));
					break;
				case FHARDDOOM_SRDCMD_TYPE_SRC_PITCH:
					d->srd_src_pitch = data & FHARDDOOM_BLOCK_PITCH_MASK;
					break;
				case FHARDDOOM_SRDCMD_TYPE_READ:
					d->stats[FHARDDOOM_STAT_SRD_READ]++;
					d->srd_state = data & (FHARDDOOM_SRD_STATE_READ_LENGTH_MASK | FHARDDOOM_SRD_STATE_COL);
					break;
				case FHARDDOOM_SRDCMD_TYPE_SRDSEM:
					d->srd_state |= FHARDDOOM_SRD_STATE_SRDSEM;
					break;
				case FHARDDOOM_SRDCMD_TYPE_FESEM:
					d->stats[FHARDDOOM_STAT_SRD_FESEM]++;
					d->srd_state |= FHARDDOOM_SRD_STATE_FESEM;
					break;
			}
		}
	}
}

/* Runs SPAN for some time.  Returns true if anything has been done.  */
static bool fharddoom_run_span(FinalHardDoomState *d) {
	if (!(d->enable & FHARDDOOM_ENABLE_SPAN))
		return false;
	bool any = false;
	while (true) {
		if (d->span_state & FHARDDOOM_SPAN_STATE_SPANSEM) {
			if (!d->spansem)
				return any;
			d->spansem = 0;
			d->span_state &= ~FHARDDOOM_SPAN_STATE_SPANSEM;
			fharddoom_reset_cache(d, FHARDDOOM_CACHE_CLIENT_SPAN_SRC);
			any = true;
		} else if (d->span_state & FHARDDOOM_SPAN_STATE_DRAW_LENGTH_MASK) {
			if (fharddoom_spanout_full(d))
				return any;
			int ulog = FHARDDOOM_SPANCMD_DATA_EXTR_UVMASK_ULOG(d->span_uvmask);
			int vlog = FHARDDOOM_SPANCMD_DATA_EXTR_UVMASK_VLOG(d->span_uvmask);
			uint32_t umask = 0xffffffff;
			uint32_t vmask = 0xffffffff;
			if (ulog < 0x10)
				umask >>= (0x10 - ulog);
			if (vlog < 0x10)
				vmask >>= (0x10 - vlog);
			uint32_t xoff = (d->span_state & FHARDDOOM_SPAN_STATE_DRAW_XOFF_MASK) >> FHARDDOOM_SPAN_STATE_DRAW_XOFF_SHIFT;
			int slot = d->span_src >> FHARDDOOM_SPAN_SRC_SLOT_SHIFT;
			uint32_t pitch = d->span_src & FHARDDOOM_SPAN_SRC_PITCH_MASK;
			while (true) {
				uint32_t ui = d->span_ustart >> 16;
				uint32_t vi = d->span_vstart >> 16;
				uint32_t va = FHARDDOOM_VA(ui + pitch * vi, slot);
				uint8_t *dst = &d->spanout_data[d->spanout_put * FHARDDOOM_BLOCK_SIZE + xoff];
				if (!fharddoom_cache_read(d, FHARDDOOM_CACHE_CLIENT_SPAN_SRC, va, dst, false)) {
					d->enable &= ~FHARDDOOM_ENABLE_SPAN;
					d->span_state &= ~FHARDDOOM_SPAN_STATE_DRAW_XOFF_MASK;
					d->span_state |= xoff << FHARDDOOM_SPAN_STATE_DRAW_XOFF_SHIFT;
					return any;
				}
				d->span_ustart = ((d->span_ustart + d->span_ustep) & umask) | (d->span_ustart & ~umask);
				d->span_vstart = ((d->span_vstart + d->span_vstep) & vmask) | (d->span_vstart & ~vmask);
				xoff++;
				d->span_state--;
				d->stats[FHARDDOOM_STAT_SPAN_PIXEL]++;
				any = true;
				if (xoff == FHARDDOOM_BLOCK_SIZE || !(d->span_state & FHARDDOOM_SPAN_STATE_DRAW_LENGTH_MASK)) {
					fharddoom_spanout_write(d);
					d->stats[FHARDDOOM_STAT_SPAN_BLOCK]++;
					xoff = 0;
					break;
				}
			}
			d->span_state &= ~FHARDDOOM_SPAN_STATE_DRAW_XOFF_MASK;
			d->span_state |= xoff << FHARDDOOM_SPAN_STATE_DRAW_XOFF_SHIFT;
		} else {
			/* No command. get one.  */
			if (fharddoom_spancmd_empty(d))
				return any;
			uint32_t cmd = d->spancmd_cmd[d->spancmd_get];
			uint32_t data = d->spancmd_data[d->spancmd_get];
			fharddoom_spancmd_read(d);
			//printf("SPANCMD %08x %08x\n", cmd, data);
			d->stats[FHARDDOOM_STAT_SPAN_CMD]++;
			any = true;
			switch (cmd) {
				case FHARDDOOM_SPANCMD_TYPE_SRC_SLOT:
					d->span_src &= ~FHARDDOOM_SPAN_SRC_SLOT_MASK;
					d->span_src |= data << FHARDDOOM_SPAN_SRC_SLOT_SHIFT & FHARDDOOM_SPAN_SRC_SLOT_MASK;
					break;
				case FHARDDOOM_SPANCMD_TYPE_SRC_PITCH:
					d->span_src &= ~FHARDDOOM_SPAN_SRC_PITCH_MASK;
					d->span_src |= data & FHARDDOOM_SPAN_SRC_PITCH_MASK;
					break;
				case FHARDDOOM_SPANCMD_TYPE_UVMASK:
					d->span_uvmask = data & FHARDDOOM_SPAN_UVMASK_MASK;
					break;
				case FHARDDOOM_SPANCMD_TYPE_USTART:
					d->span_ustart = data;
					break;
				case FHARDDOOM_SPANCMD_TYPE_VSTART:
					d->span_vstart = data;
					break;
				case FHARDDOOM_SPANCMD_TYPE_USTEP:
					d->span_ustep = data;
					break;
				case FHARDDOOM_SPANCMD_TYPE_VSTEP:
					d->span_vstep = data;
					break;
				case FHARDDOOM_SPANCMD_TYPE_DRAW:
					d->stats[FHARDDOOM_STAT_SPAN_DRAW]++;
					d->span_state = data & (FHARDDOOM_SPAN_STATE_DRAW_LENGTH_MASK | FHARDDOOM_SPAN_STATE_DRAW_XOFF_MASK);
					break;
				case FHARDDOOM_SPANCMD_TYPE_SPANSEM:
					d->span_state |= FHARDDOOM_SPAN_STATE_SPANSEM;
					break;
			}
		}
	}
}

/* Runs COL for some time.  Returns true if anything has been done.  */
static bool fharddoom_run_col(FinalHardDoomState *d) {
	if (!(d->enable & FHARDDOOM_ENABLE_COL))
		return false;
	bool any = false;
	while (true) {
		if (d->col_state & FHARDDOOM_COL_STATE_COLSEM) {
			if (!d->colsem)
				return any;
			d->colsem = 0;
			d->col_state &= ~FHARDDOOM_COL_STATE_COLSEM;
			fharddoom_reset_cache(d, FHARDDOOM_CACHE_CLIENT_COL_SRC);
			fharddoom_reset_cache(d, FHARDDOOM_CACHE_CLIENT_COL_CMAP_B);
			any = true;
		} else if (d->col_state & FHARDDOOM_COL_STATE_LOAD_CMAP_A) {
			if (fharddoom_colin_empty(d))
				return any;
			int pos = (d->col_state & FHARDDOOM_COL_STATE_CMAP_A_POS_MASK) >> FHARDDOOM_COL_STATE_CMAP_A_POS_SHIFT;
			for (int i = 0; i < FHARDDOOM_BLOCK_SIZE; i++)
				d->col_cmap_a[pos * FHARDDOOM_BLOCK_SIZE + i] = d->colin_data[d->colin_get * FHARDDOOM_BLOCK_SIZE + i];
			fharddoom_colin_read(d);
			pos++;
			if (pos == 4) {
				d->col_state &= ~(FHARDDOOM_COL_STATE_LOAD_CMAP_A | FHARDDOOM_COL_STATE_CMAP_A_POS_MASK);
			} else {
				d->col_state &= ~FHARDDOOM_COL_STATE_CMAP_A_POS_MASK;
				d->col_state |= pos << FHARDDOOM_COL_STATE_CMAP_A_POS_SHIFT;
			}
			any = true;
		} else if (d->col_state & FHARDDOOM_COL_STATE_DRAW_LENGTH_MASK) {
			if (fharddoom_colout_full(d))
				return any;
			int xoff = (d->col_state & FHARDDOOM_COL_STATE_XOFF_MASK) >> FHARDDOOM_COL_STATE_XOFF_SHIFT;
			while (true) {
				if (d->col_cols_state[xoff] & FHARDDOOM_COL_COLS_STATE_COL_EN) {
					uint32_t get = (d->col_cols_state[xoff] & FHARDDOOM_COL_COLS_STATE_DATA_GET_MASK) >> FHARDDOOM_COL_COLS_STATE_DATA_GET_SHIFT;
					uint32_t put = (d->col_cols_state[xoff] & FHARDDOOM_COL_COLS_STATE_DATA_PUT_MASK) >> FHARDDOOM_COL_COLS_STATE_DATA_PUT_SHIFT;
					uint32_t cmap = (d->col_cols_state[xoff] & FHARDDOOM_COL_COLS_STATE_DATA_CMAP_MASK) >> FHARDDOOM_COL_COLS_STATE_DATA_CMAP_SHIFT;
					if (get == cmap) {
						if (cmap == put) {
							while (true) {
								uint32_t ui = d->col_cols_ustart[xoff] >> 16;
								if (d->col_cols_src_height[xoff])
									ui %= d->col_cols_src_height[xoff];
								uint32_t addr = FHARDDOOM_VA(d->col_cols_src_va[xoff] + d->col_cols_src_pitch[xoff] * ui, FHARDDOOM_VA_SLOT(d->col_cols_src_va[xoff]));
								uint8_t *dst = &d->col_data[xoff * FHARDDOOM_COL_DATA_SIZE + put];
								bool spec = cmap != put;
								if (!fharddoom_cache_read(d, FHARDDOOM_CACHE_CLIENT_COL_SRC, addr, dst, spec)) {
									if (!spec) {
										d->enable &= ~FHARDDOOM_ENABLE_COL;
										return any;
									} else {
										break;
									}
								}
								if (d->col_state & FHARDDOOM_COL_STATE_CMAP_A_EN) {
									*dst = d->col_cmap_a[*dst];
								}
								d->col_cols_ustart[xoff] += d->col_cols_ustep[xoff];
								if (d->col_cols_src_height[xoff])
									d->col_cols_ustart[xoff] %= d->col_cols_src_height[xoff] << 16;
								put++;
								put %= FHARDDOOM_COL_DATA_SIZE;
								d->col_cols_state[xoff] &= ~FHARDDOOM_COL_COLS_STATE_DATA_PUT_MASK;
								d->col_cols_state[xoff] |= put << FHARDDOOM_COL_COLS_STATE_DATA_PUT_SHIFT;
								if ((put + 1) % FHARDDOOM_COL_DATA_SIZE == get)
									break;

							}
						}
						if (d->col_cols_state[xoff] & FHARDDOOM_COL_COLS_STATE_CMAP_B_EN) {
							while (true) {
								uint8_t *dst = &d->col_data[xoff * FHARDDOOM_COL_DATA_SIZE + cmap];
								uint32_t addr = d->col_cols_cmap_b_va[xoff] + *dst;
								bool spec = get != cmap;
								if (!fharddoom_cache_read(d, FHARDDOOM_CACHE_CLIENT_COL_CMAP_B, addr, dst, spec)) {
									if (!spec) {
										d->enable &= ~FHARDDOOM_ENABLE_COL;
										return any;
									} else {
										break;
									}
								}
								cmap++;
								cmap %= FHARDDOOM_COL_DATA_SIZE;
								d->col_cols_state[xoff] &= ~FHARDDOOM_COL_COLS_STATE_DATA_CMAP_MASK;
								d->col_cols_state[xoff] |= cmap << FHARDDOOM_COL_COLS_STATE_DATA_CMAP_SHIFT;
								if (cmap == put)
									break;
							}
						} else {
							cmap = put;
						}
						d->col_cols_state[xoff] &= ~FHARDDOOM_COL_COLS_STATE_DATA_CMAP_MASK;
						d->col_cols_state[xoff] |= cmap << FHARDDOOM_COL_COLS_STATE_DATA_CMAP_SHIFT;
					}
					uint8_t byte = d->col_data[xoff * FHARDDOOM_COL_DATA_SIZE + get];
					d->colout_data[d->colout_put * FHARDDOOM_BLOCK_SIZE + xoff] = byte;
					get++;
					get %= FHARDDOOM_COL_DATA_SIZE;
					d->col_cols_state[xoff] &= ~FHARDDOOM_COL_COLS_STATE_DATA_GET_MASK;
					d->col_cols_state[xoff] |= get << FHARDDOOM_COL_COLS_STATE_DATA_GET_SHIFT;
					d->stats[FHARDDOOM_STAT_COL_PIXEL]++;
					if (d->col_cols_state[xoff] & FHARDDOOM_COL_COLS_STATE_CMAP_B_EN)
						d->stats[FHARDDOOM_STAT_COL_PIXEL_CMAP_B]++;
				}
				xoff++;
				any = true;
				d->col_state &= ~FHARDDOOM_COL_STATE_XOFF_MASK;
				if (xoff == FHARDDOOM_BLOCK_SIZE) {
					xoff = 0;
					uint64_t mask = 0;
					for (int i = 0; i < FHARDDOOM_BLOCK_SIZE; i++)
						if (d->col_cols_state[i] & FHARDDOOM_COL_COLS_STATE_COL_EN)
							mask |= 1ull << i;
					d->colout_mask[d->colout_put] = mask;
					fharddoom_colout_write(d);
					d->stats[FHARDDOOM_STAT_COL_BLOCK]++;
					if (d->col_state & FHARDDOOM_COL_STATE_CMAP_A_EN)
						d->stats[FHARDDOOM_STAT_COL_BLOCK_CMAP_A]++;
					d->col_state--;
					break;
				}
				d->col_state |= xoff << FHARDDOOM_COL_STATE_XOFF_SHIFT;
			}
		} else {
			if (fharddoom_colcmd_empty(d))
				return any;
			uint32_t cmd = d->colcmd_cmd[d->colcmd_get];
			uint32_t data = d->colcmd_data[d->colcmd_get];
			fharddoom_colcmd_read(d);
			//printf("COLCMD %08x %08x\n", cmd, data);
			d->stats[FHARDDOOM_STAT_COL_CMD]++;
			any = true;
			switch (cmd) {
				case FHARDDOOM_COLCMD_TYPE_COL_CMAP_B_VA:
					d->col_stage_cmap_b_va = data & FHARDDOOM_COL_COLS_CMAP_B_VA_MASK;
					break;
				case FHARDDOOM_COLCMD_TYPE_COL_SRC_VA:
					d->col_stage_src_va = data & FHARDDOOM_VA_MASK;
					break;
				case FHARDDOOM_COLCMD_TYPE_COL_SRC_PITCH:
					d->col_stage_src_pitch = data & FHARDDOOM_COL_COLS_SRC_PITCH_MASK;
					break;
				case FHARDDOOM_COLCMD_TYPE_COL_USTART:
					d->col_stage_ustart = data;
					break;
				case FHARDDOOM_COLCMD_TYPE_COL_USTEP:
					d->col_stage_ustep = data;
					break;
				case FHARDDOOM_COLCMD_TYPE_COL_SETUP:
					{
					int which = FHARDDOOM_COLCMD_DATA_EXTR_COL_SETUP_X(data);
					d->col_cols_state[which] = 0;
					if (FHARDDOOM_COLCMD_DATA_EXTR_COL_SETUP_COL_EN(data))
						d->col_cols_state[which] |= FHARDDOOM_COL_COLS_STATE_COL_EN;
					if (FHARDDOOM_COLCMD_DATA_EXTR_COL_SETUP_CMAP_B_EN(data))
						d->col_cols_state[which] |= FHARDDOOM_COL_COLS_STATE_CMAP_B_EN;
					d->col_cols_src_height[which] = FHARDDOOM_COLCMD_DATA_EXTR_COL_SETUP_SRC_HEIGHT(data);
					d->col_cols_cmap_b_va[which] = d->col_stage_cmap_b_va;
					d->col_cols_src_va[which] = d->col_stage_src_va;
					d->col_cols_src_pitch[which] = d->col_stage_src_pitch;
					d->col_cols_ustart[which] = d->col_stage_ustart;
					d->col_cols_ustep[which] = d->col_stage_ustep;
					}
					break;
				case FHARDDOOM_COLCMD_TYPE_LOAD_CMAP_A:
					d->stats[FHARDDOOM_STAT_COL_LOAD_CMAP_A]++;
					d->col_state |= FHARDDOOM_COL_STATE_LOAD_CMAP_A;
					d->col_state &= ~0xc0000000;
					break;
				case FHARDDOOM_COLCMD_TYPE_DRAW:
					d->stats[FHARDDOOM_STAT_COL_DRAW]++;
					if (data & FHARDDOOM_COL_STATE_CMAP_A_EN)
						d->stats[FHARDDOOM_STAT_COL_DRAW_CMAP_A]++;
					d->col_state = data & (FHARDDOOM_COL_STATE_DRAW_LENGTH_MASK | FHARDDOOM_COL_STATE_CMAP_A_EN);
					break;
				case FHARDDOOM_COLCMD_TYPE_COLSEM:
					d->col_state |= FHARDDOOM_COL_STATE_COLSEM;
					break;
			}
		}
	}
}

/* Runs FX for some time.  Returns true if anything has been done.  */
static bool fharddoom_run_fx(FinalHardDoomState *d) {
	if (!(d->enable & FHARDDOOM_ENABLE_FX))
		return false;
	bool any = false;
	while (true) {
		if (d->fx_state & FHARDDOOM_FX_STATE_LOAD_ACTIVE) {
			if (fharddoom_fxin_empty(d))
				return any;
			any = true;
			uint32_t pos = (d->fx_state & FHARDDOOM_FX_STATE_LOAD_CNT_MASK) >> FHARDDOOM_FX_STATE_LOAD_CNT_SHIFT;
			d->fx_state &= ~FHARDDOOM_FX_STATE_LOAD_CNT_MASK;
			switch (d->fx_state & FHARDDOOM_FX_STATE_LOAD_MODE_MASK) {
				case FHARDDOOM_FX_STATE_LOAD_MODE_CMAP_A:
					memcpy(&d->fx_cmap_a[pos * FHARDDOOM_BLOCK_SIZE], &d->fxin_data[d->fxin_get * FHARDDOOM_BLOCK_SIZE], FHARDDOOM_BLOCK_SIZE);
					pos++;
					if (pos == 4)
						d->fx_state &= ~FHARDDOOM_FX_STATE_LOAD_ACTIVE;
					else
						d->fx_state |= pos << FHARDDOOM_FX_STATE_LOAD_CNT_SHIFT;
					break;
				case FHARDDOOM_FX_STATE_LOAD_MODE_CMAP_B:
					memcpy(&d->fx_cmap_b[pos * FHARDDOOM_BLOCK_SIZE], &d->fxin_data[d->fxin_get * FHARDDOOM_BLOCK_SIZE], FHARDDOOM_BLOCK_SIZE);
					pos++;
					if (pos == 4)
						d->fx_state &= ~FHARDDOOM_FX_STATE_LOAD_ACTIVE;
					else
						d->fx_state |= pos << FHARDDOOM_FX_STATE_LOAD_CNT_SHIFT;
					break;
				case FHARDDOOM_FX_STATE_LOAD_MODE_BLOCK:
					memcpy(&d->fx_buf[pos * FHARDDOOM_BLOCK_SIZE], &d->fxin_data[d->fxin_get * FHARDDOOM_BLOCK_SIZE], FHARDDOOM_BLOCK_SIZE);
					d->fx_state &= ~FHARDDOOM_FX_STATE_LOAD_ACTIVE;
					break;
				case FHARDDOOM_FX_STATE_LOAD_MODE_FUZZ:
					memcpy(&d->fx_buf[pos * FHARDDOOM_BLOCK_SIZE], &d->fxin_data[d->fxin_get * FHARDDOOM_BLOCK_SIZE], FHARDDOOM_BLOCK_SIZE);
					pos++;
					if (pos >= 2)
						d->fx_state &= ~FHARDDOOM_FX_STATE_LOAD_ACTIVE;
					else
						d->fx_state |= pos << FHARDDOOM_FX_STATE_LOAD_CNT_SHIFT;
					break;
			}
			fharddoom_fxin_read(d);
		} else if (d->fx_state & FHARDDOOM_FX_STATE_DRAW_LENGTH_MASK) {
			int pos[3] = {0};
			if (d->fx_state & FHARDDOOM_FX_STATE_DRAW_FUZZ_EN) {
				pos[0] = (d->fx_state & FHARDDOOM_FX_STATE_DRAW_FUZZ_POS_MASK) >> FHARDDOOM_FX_STATE_DRAW_FUZZ_POS_SHIFT;
				pos[1] = (pos[0] + 1) % 4;
				pos[2] = (pos[0] + 2) % 4;
			}
			if (!(d->fx_state & FHARDDOOM_FX_STATE_DRAW_FETCH_DONE)) {
				if (d->fx_state & FHARDDOOM_FX_STATE_DRAW_FETCH_SRD) {
					if (fharddoom_fxin_empty(d))
						return any;
					memcpy(&d->fx_buf[pos[2] * FHARDDOOM_BLOCK_SIZE], &d->fxin_data[d->fxin_get * FHARDDOOM_BLOCK_SIZE], FHARDDOOM_BLOCK_SIZE);
					fharddoom_fxin_read(d);
				} else if (d->fx_state & FHARDDOOM_FX_STATE_DRAW_FETCH_SPAN) {
					if (fharddoom_spanout_empty(d))
						return any;
					memcpy(&d->fx_buf[pos[2] * FHARDDOOM_BLOCK_SIZE], &d->spanout_data[d->spanout_get * FHARDDOOM_BLOCK_SIZE], FHARDDOOM_BLOCK_SIZE);
					fharddoom_spanout_read(d);
				}
				if (d->fx_state & FHARDDOOM_FX_STATE_DRAW_CMAP_A_EN)
					for (int i = 0; i < FHARDDOOM_BLOCK_SIZE; i++)
						d->fx_buf[i] = d->fx_cmap_a[d->fx_buf[i]];
				if (d->fx_state & FHARDDOOM_FX_STATE_DRAW_CMAP_B_EN)
					for (int i = 0; i < FHARDDOOM_BLOCK_SIZE; i++)
						d->fx_buf[i] = d->fx_cmap_b[d->fx_buf[i]];
				if (d->fx_state & FHARDDOOM_FX_STATE_DRAW_FUZZ_EN) {
					for (int i = 0; i < FHARDDOOM_BLOCK_SIZE; i++) {
						if (!(d->fx_col[i] & FHARDDOOM_FX_COL_ENABLE))
							continue;
						uint32_t fuzzpos = d->fx_col[i] & FHARDDOOM_FX_COL_FUZZPOS_MASK;
						bool m = 0x121e650de224aull >> fuzzpos & 1;
						fuzzpos++;
						fuzzpos %= 50;
						d->fx_col[i] = FHARDDOOM_FX_COL_ENABLE | fuzzpos;
						d->fx_buf[pos[1] * FHARDDOOM_BLOCK_SIZE + i] =
							d->fx_cmap_a[d->fx_buf[pos[m ? 0 : 2] * FHARDDOOM_BLOCK_SIZE + i]];
					}
				}
				d->fx_state |= FHARDDOOM_FX_STATE_DRAW_FETCH_DONE;
				any = true;
			}
			if (fharddoom_fxout_full(d))
				return any;
			memcpy(&d->fxout_data[d->fxout_put * FHARDDOOM_BLOCK_SIZE], &d->fx_buf[pos[1] * FHARDDOOM_BLOCK_SIZE], FHARDDOOM_BLOCK_SIZE);
			d->fx_state--;
			uint64_t mask = 0xffffffffffffffffull;
			if (d->fx_skip & FHARDDOOM_FX_SKIP_ALWAYS || !(d->fx_state & FHARDDOOM_FX_STATE_DRAW_NON_FIRST)) {
				mask &= 0xffffffffffffffffull << FHARDDOOM_FXCMD_DATA_EXTR_SKIP_BEGIN(d->fx_skip);
			}
			if (d->fx_skip & FHARDDOOM_FX_SKIP_ALWAYS || !(d->fx_state & FHARDDOOM_FX_STATE_DRAW_LENGTH_MASK)) {
				mask &= 0xffffffffffffffffull >> FHARDDOOM_FXCMD_DATA_EXTR_SKIP_END(d->fx_skip);
			}
			d->fxout_mask[d->fxout_put] = mask;
			fharddoom_fxout_write(d);
			d->stats[FHARDDOOM_STAT_FX_BLOCK]++;
			if (d->fx_state & FHARDDOOM_FX_STATE_DRAW_FUZZ_EN) {
				pos[0]++;
				pos[0] %= 4;
				d->fx_state &= ~FHARDDOOM_FX_STATE_DRAW_FUZZ_POS_MASK;
				d->fx_state |= pos[0] << FHARDDOOM_FX_STATE_DRAW_FUZZ_POS_SHIFT;
				d->stats[FHARDDOOM_STAT_FX_BLOCK_FUZZ]++;
			} else {
				if (d->fx_state & FHARDDOOM_FX_STATE_DRAW_CMAP_A_EN) {
					d->stats[FHARDDOOM_STAT_FX_BLOCK_CMAP_A]++;
				}
				if (d->fx_state & FHARDDOOM_FX_STATE_DRAW_CMAP_B_EN) {
					d->stats[FHARDDOOM_STAT_FX_BLOCK_CMAP_B]++;
				}
			}
			d->fx_state |= FHARDDOOM_FX_STATE_DRAW_NON_FIRST;
			d->fx_state &= ~FHARDDOOM_FX_STATE_DRAW_FETCH_DONE;
			any = true;
		} else {
			if (fharddoom_fxcmd_empty(d))
				return any;
			uint32_t cmd = d->fxcmd_cmd[d->fxcmd_get];
			uint32_t data = d->fxcmd_data[d->fxcmd_get];
			fharddoom_fxcmd_read(d);
			//printf("FXCMD %08x %08x\n", cmd, data);
			d->stats[FHARDDOOM_STAT_FX_CMD]++;
			any = true;
			switch (cmd) {
				case FHARDDOOM_FXCMD_TYPE_LOAD_CMAP:
					d->stats[FHARDDOOM_STAT_FX_LOAD_CMAP]++;
					d->fx_state &= ~(FHARDDOOM_FX_STATE_LOAD_MODE_MASK | FHARDDOOM_FX_STATE_LOAD_CNT_MASK);
					if (data & 1)
						d->fx_state |= FHARDDOOM_FX_STATE_LOAD_MODE_CMAP_B;
					else
						d->fx_state |= FHARDDOOM_FX_STATE_LOAD_MODE_CMAP_A;
					d->fx_state |= FHARDDOOM_FX_STATE_LOAD_ACTIVE;
					break;
				case FHARDDOOM_FXCMD_TYPE_LOAD_BLOCK:
					d->fx_state &= ~(FHARDDOOM_FX_STATE_LOAD_MODE_MASK | FHARDDOOM_FX_STATE_LOAD_CNT_MASK);
					d->fx_state |= FHARDDOOM_FX_STATE_LOAD_MODE_BLOCK;
					d->fx_state |= FHARDDOOM_FX_STATE_LOAD_ACTIVE;
					break;
				case FHARDDOOM_FXCMD_TYPE_LOAD_FUZZ:
					d->stats[FHARDDOOM_STAT_FX_LOAD_FUZZ]++;
					d->fx_state &= ~(FHARDDOOM_FX_STATE_LOAD_MODE_MASK | FHARDDOOM_FX_STATE_LOAD_CNT_MASK | FHARDDOOM_FX_STATE_DRAW_FUZZ_POS_MASK);
					d->fx_state |= FHARDDOOM_FX_STATE_LOAD_MODE_FUZZ;
					d->fx_state |= FHARDDOOM_FX_STATE_LOAD_ACTIVE;
					break;
				case FHARDDOOM_FXCMD_TYPE_FILL_COLOR:
					memset(d->fx_buf, (uint8_t)data, FHARDDOOM_FX_BUF_SIZE);
					break;
				case FHARDDOOM_FXCMD_TYPE_COL_SETUP:
					d->fx_col[FHARDDOOM_FXCMD_DATA_EXTR_COL_SETUP_X(data)] =
						FHARDDOOM_FXCMD_DATA_EXTR_COL_SETUP_FUZZPOS(data) |
						(FHARDDOOM_FXCMD_DATA_EXTR_COL_SETUP_ENABLE(data) ? FHARDDOOM_FX_COL_ENABLE : 0);
					break;
				case FHARDDOOM_FXCMD_TYPE_SKIP:
					d->fx_skip = data & FHARDDOOM_FX_SKIP_MASK;
					break;
				case FHARDDOOM_FXCMD_TYPE_DRAW:
					d->stats[FHARDDOOM_STAT_FX_DRAW]++;
					if (data & FHARDDOOM_FX_STATE_DRAW_FUZZ_EN)
						d->fx_state &= FHARDDOOM_FX_STATE_DRAW_FUZZ_POS_MASK;
					else
						d->fx_state = 0;
					d->fx_state |= data & (FHARDDOOM_FX_STATE_DRAW_LENGTH_MASK | FHARDDOOM_FX_STATE_DRAW_CMAP_A_EN | FHARDDOOM_FX_STATE_DRAW_CMAP_B_EN | FHARDDOOM_FX_STATE_DRAW_FUZZ_EN | FHARDDOOM_FX_STATE_DRAW_FETCH_SRD | FHARDDOOM_FX_STATE_DRAW_FETCH_SPAN);
					break;
			}
		}
	}
}

/* Runs SWR for some time.  Returns true if anything has been done.  */
static bool fharddoom_run_swr(FinalHardDoomState *d) {
	if (!(d->enable & FHARDDOOM_ENABLE_SWR))
		return false;
	bool any = false;
	while (true) {
		if (d->swr_state & FHARDDOOM_SWR_STATE_SRDSEM) {
			if (d->srdsem)
				return any;
			d->srdsem = 1;
			d->swr_state &= ~FHARDDOOM_SWR_STATE_SRDSEM;
			any = true;
		} else if (d->swr_state & FHARDDOOM_SWR_STATE_COLSEM) {
			if (d->colsem)
				return any;
			d->colsem = 1;
			d->swr_state &= ~FHARDDOOM_SWR_STATE_COLSEM;
			any = true;
		} else if (d->swr_state & FHARDDOOM_SWR_STATE_SPANSEM) {
			if (d->spansem)
				return any;
			d->spansem = 1;
			d->swr_state &= ~FHARDDOOM_SWR_STATE_SPANSEM;
			any = true;
		} else if (d->swr_state & FHARDDOOM_SWR_STATE_DRAW_LENGTH_MASK) {
			if (!(d->swr_state & FHARDDOOM_SWR_STATE_SRC_BUF_FULL)) {
				if (d->swr_state & FHARDDOOM_SWR_STATE_COL_EN) {
					if (fharddoom_colout_empty(d))
						return any;
					memcpy(d->swr_src_buf, &d->colout_data[d->colout_get * FHARDDOOM_BLOCK_SIZE], FHARDDOOM_BLOCK_SIZE);
					d->swr_block_mask = d->colout_mask[d->colout_get];
					fharddoom_colout_read(d);
				} else {
					if (fharddoom_fxout_empty(d))
						return any;
					memcpy(d->swr_src_buf, &d->fxout_data[d->fxout_get * FHARDDOOM_BLOCK_SIZE], FHARDDOOM_BLOCK_SIZE);
					d->swr_block_mask = d->fxout_mask[d->fxout_get];
					fharddoom_fxout_read(d);
				}
				d->swr_state |= FHARDDOOM_SWR_STATE_SRC_BUF_FULL;
				any = true;
			}
			if (!(d->swr_state & FHARDDOOM_SWR_STATE_DST_BUF_FULL)) {
				if (d->swr_block_mask != 0xffffffffffffffffull || d->swr_state & FHARDDOOM_SWR_STATE_TRANS_EN) {
					uint64_t pa;
					if (!fharddoom_mmu_translate(d, FHARDDOOM_MMU_CLIENT_SWR_DST, d->swr_dst_va, &pa)) {
						d->enable &= ~FHARDDOOM_ENABLE_SWR;
						return any;
					}
					pci_dma_read(&d->dev, pa, &d->swr_dst_buf, FHARDDOOM_BLOCK_SIZE);
					d->stats[FHARDDOOM_STAT_SWR_BLOCK_READ]++;
				}
				d->swr_state |= FHARDDOOM_SWR_STATE_DST_BUF_FULL;
				any = true;
			}
			int pos = (d->swr_state & FHARDDOOM_SWR_STATE_TRANS_POS_MASK) >> FHARDDOOM_SWR_STATE_TRANS_POS_SHIFT;
			while (pos < FHARDDOOM_BLOCK_SIZE) {
				if (d->swr_block_mask & 1ull << pos) {
					if (d->swr_state & FHARDDOOM_SWR_STATE_TRANS_EN) {
						uint32_t idx = d->swr_dst_buf[pos] << 8 | d->swr_src_buf[pos];
						uint32_t addr = idx + d->swr_transmap_va;
						if (!fharddoom_cache_read(d, FHARDDOOM_CACHE_CLIENT_SWR_TRANSMAP, addr, &d->swr_trans_buf[pos], false)) {
							d->enable &= ~FHARDDOOM_ENABLE_SWR;
							return any;
						}
					} else {
						d->swr_trans_buf[pos] = d->swr_src_buf[pos];
					}
				} else {
					d->swr_trans_buf[pos] = d->swr_dst_buf[pos];
				}
				pos++;
				d->swr_state &= ~FHARDDOOM_SWR_STATE_TRANS_POS_MASK;
				d->swr_state |= pos << FHARDDOOM_SWR_STATE_TRANS_POS_SHIFT;
				any = true;
			}
			uint64_t pa;
			if (!fharddoom_mmu_translate(d, FHARDDOOM_MMU_CLIENT_SWR_DST, d->swr_dst_va, &pa)) {
				d->enable &= ~FHARDDOOM_ENABLE_SWR;
				return any;
			}
			pci_dma_write(&d->dev, pa, d->swr_trans_buf, FHARDDOOM_BLOCK_SIZE);
			d->swr_state--;
			d->swr_state &= ~(FHARDDOOM_SWR_STATE_SRC_BUF_FULL | FHARDDOOM_SWR_STATE_DST_BUF_FULL | FHARDDOOM_SWR_STATE_TRANS_POS_MASK);
			d->swr_dst_va = FHARDDOOM_VA(d->swr_dst_va + d->swr_dst_pitch, FHARDDOOM_VA_SLOT(d->swr_dst_va));
			d->stats[FHARDDOOM_STAT_SWR_BLOCK]++;
			if (d->swr_state & FHARDDOOM_SWR_STATE_TRANS_EN)
				d->stats[FHARDDOOM_STAT_SWR_BLOCK_TRANS]++;
			any = true;
		} else {
			if (fharddoom_swrcmd_empty(d))
				return any;
			uint32_t cmd = d->swrcmd_cmd[d->swrcmd_get];
			uint32_t data = d->swrcmd_data[d->swrcmd_get];
			fharddoom_swrcmd_read(d);
			// printf("SWRCMD %08x %08x\n", cmd, data);
			d->stats[FHARDDOOM_STAT_SWR_CMD]++;
			any = true;
			switch (cmd) {
				case FHARDDOOM_SWRCMD_TYPE_TRANSMAP_VA:
					d->swr_transmap_va = data & FHARDDOOM_SWR_TRANSMAP_VA_MASK;
					break;
				case FHARDDOOM_SWRCMD_TYPE_DST_SLOT:
					d->swr_dst_va = FHARDDOOM_VA(d->swr_dst_va, data & FHARDDOOM_SLOT_MASK);
					break;
				case FHARDDOOM_SWRCMD_TYPE_DST_PTR:
					d->swr_dst_va = FHARDDOOM_VA(data & FHARDDOOM_BLOCK_PTR_MASK, FHARDDOOM_VA_SLOT(d->swr_dst_va));
					break;
				case FHARDDOOM_SWRCMD_TYPE_DST_PITCH:
					d->swr_dst_pitch = data & FHARDDOOM_BLOCK_PITCH_MASK;
					break;
				case FHARDDOOM_SWRCMD_TYPE_DRAW:
					d->stats[FHARDDOOM_STAT_SWR_DRAW]++;
					d->swr_state = data & (FHARDDOOM_SWR_STATE_DRAW_LENGTH_MASK | FHARDDOOM_SWR_STATE_COL_EN | FHARDDOOM_SWR_STATE_TRANS_EN);
					break;
				case FHARDDOOM_SWRCMD_TYPE_SRDSEM:
					d->stats[FHARDDOOM_STAT_SWR_SRDSEM]++;
					d->swr_state |= FHARDDOOM_SWR_STATE_SRDSEM;
					break;
				case FHARDDOOM_SWRCMD_TYPE_COLSEM:
					d->stats[FHARDDOOM_STAT_SWR_COLSEM]++;
					d->swr_state |= FHARDDOOM_SWR_STATE_COLSEM;
					break;
				case FHARDDOOM_SWRCMD_TYPE_SPANSEM:
					d->stats[FHARDDOOM_STAT_SWR_SPANSEM]++;
					d->swr_state |= FHARDDOOM_SWR_STATE_SPANSEM;
					break;
			}
		}
	}
}

static void *fharddoom_thread(void *opaque)
{
	FinalHardDoomState *d = opaque;
	qemu_mutex_lock(&d->mutex);
	while (1) {
		if (d->stopping)
			break;
		qemu_mutex_unlock(&d->mutex);
		qemu_mutex_lock_iothread();
		qemu_mutex_lock(&d->mutex);
		bool idle = true;
		if (fharddoom_run_cmd_main(d))
			idle = false;
		if (fharddoom_run_cmd_sub(d))
			idle = false;
		if (fharddoom_run_fe(d))
			idle = false;
		if (fharddoom_run_srd(d))
			idle = false;
		if (fharddoom_run_span(d))
			idle = false;
		if (fharddoom_run_col(d))
			idle = false;
		if (fharddoom_run_fx(d))
			idle = false;
		if (fharddoom_run_swr(d))
			idle = false;
		fharddoom_status_update(d);
		qemu_mutex_unlock_iothread();
		if (idle) {
			qemu_cond_wait(&d->cond, &d->mutex);
		}
	}
	qemu_mutex_unlock(&d->mutex);
	return 0;
}

static void fharddoom_realize(PCIDevice *pci_dev, Error **errp)
{
	FinalHardDoomState *d = FHARDDOOM_DEV(pci_dev);
	uint8_t *pci_conf = d->dev.config;

	pci_config_set_interrupt_pin(pci_conf, 1);

	memory_region_init_io(&d->mmio, OBJECT(d), &fharddoom_mmio_ops, d, "fharddoom", FHARDDOOM_BAR_SIZE);
	pci_register_bar(&d->dev, 0, PCI_BASE_ADDRESS_SPACE_MEMORY, &d->mmio);

	qemu_mutex_init(&d->mutex);
	qemu_cond_init(&d->cond);

	fharddoom_power_reset(&pci_dev->qdev);

	d->stopping = false;
	qemu_thread_create(&d->thread, "fharddoom", fharddoom_thread,
			d, QEMU_THREAD_JOINABLE);
}

static void fharddoom_exit(PCIDevice *pci_dev)
{
	FinalHardDoomState *d = FHARDDOOM_DEV(pci_dev);
	qemu_mutex_lock(&d->mutex);
	d->stopping = true;
	qemu_mutex_unlock(&d->mutex);
	qemu_cond_signal(&d->cond);
	qemu_thread_join(&d->thread);
	qemu_cond_destroy(&d->cond);
	qemu_mutex_destroy(&d->mutex);
}

static void fharddoom_class_init(ObjectClass *klass, void *data)
{
	DeviceClass *dc = DEVICE_CLASS(klass);
	PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);

	k->realize = fharddoom_realize;
	k->exit = fharddoom_exit;
	k->vendor_id = FHARDDOOM_VENDOR_ID;
	k->device_id = FHARDDOOM_DEVICE_ID;
	k->class_id = PCI_CLASS_PROCESSOR_CO;
	dc->reset = fharddoom_power_reset;
}

static InterfaceInfo fharddoom_interfaces[] = {
	{ INTERFACE_CONVENTIONAL_PCI_DEVICE },
	{ },
};

static TypeInfo fharddoom_info = {
	.name          = TYPE_FHARDDOOM,
	.parent        = TYPE_PCI_DEVICE,
	.instance_size = sizeof(FinalHardDoomState),
	.class_init    = fharddoom_class_init,
	.interfaces    = fharddoom_interfaces,
};

static void fharddoom_register_types(void)
{
	type_register_static(&fharddoom_info);
}

type_init(fharddoom_register_types)
