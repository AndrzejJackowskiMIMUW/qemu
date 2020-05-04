/*
 * The Ultimate HardDoom™ device
 *
 * Copyright (C) 2013-2020 Marcelina Kościelnicka
 *
 * This work is licensed under the terms of the GNU GPL, version 2.  See
 * the COPYING file in the top-level directory.
 */

#include "uharddoom.h"
#include "qemu/osdep.h"
#include "qemu/main-loop.h"
#include "hw/hw.h"
#include "hw/pci/pci.h"

#define TYPE_UHARDDOOM "uharddoom"

#define UHARDDOOM_DEV(obj) \
	OBJECT_CHECK(UltimateHardDoomState, (obj), TYPE_UHARDDOOM)

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
	uint8_t data[hname ## _SIZE * UHARDDOOM_BLOCK_SIZE];
#define FIFO_DATA64(name, hname, data, window) \
	uint64_t data[hname ## _SIZE];
#include "uharddoom-regs.h"
	uint32_t status;
	uint8_t fe_code[UHARDDOOM_FEMEM_CODE_SIZE];
	uint8_t fe_data[UHARDDOOM_FEMEM_DATA_SIZE];
} UltimateHardDoomState;

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
	static bool uharddoom_##name##_empty(UltimateHardDoomState *d) { \
		return d->name##_get == d->name##_put; \
	} \
	static bool uharddoom_##name##_full(UltimateHardDoomState *d) { \
		uint32_t put_next = d->name##_put + 1; \
		put_next &= hname##_SIZE - 1; \
		return d->name##_get == put_next; \
	} \
	static void uharddoom_##name##_read(UltimateHardDoomState *d) { \
		uint32_t get_next = d->name##_get + 1; \
		get_next &= hname##_SIZE - 1; \
		d->name##_get = get_next; \
	} \
	static void uharddoom_##name##_write(UltimateHardDoomState *d) { \
		uint32_t put_next = d->name##_put + 1; \
		put_next &= hname##_SIZE - 1; \
		d->name##_put = put_next; \
	}
#define FIFO_DATA(name, hname, data, window, mask)
#define FIFO_DATA_BLOCK(name, hname, data, window)
#define FIFO_DATA64(name, hname, data, window)
#include "uharddoom-regs.h"

/* Recomputes status register and PCI interrupt line.  */
static void uharddoom_status_update(UltimateHardDoomState *d) {
	d->status = 0;
	/* BATCH busy iff read != write.  */
	if (d->batch_put != d->batch_get)
		d->status |= UHARDDOOM_STATUS_BATCH;
	/* JOB busy iff active.  */
	if (d->job_state & UHARDDOOM_JOB_STATE_ACTIVE)
		d->status |= UHARDDOOM_STATUS_JOB;
	/* CMD busy iff has unread commands in memory or non-empty FIFO.  */
	if (d->cmd_read_size || d->cmd_fifo_get != d->cmd_fifo_put)
		d->status |= UHARDDOOM_STATUS_CMD;
	/* FE busy iff in the running state.  */
	if ((d->fe_state & UHARDDOOM_FE_STATE_STATE_MASK) == UHARDDOOM_FE_STATE_STATE_RUNNING)
		d->status |= UHARDDOOM_STATUS_FE;
	/* SRD busy iff waiting on SRDSEM/FESEM or has non-0 pending read length.  */
	if (d->srd_state & (UHARDDOOM_SRD_STATE_SRDSEM | UHARDDOOM_SRD_STATE_FESEM | UHARDDOOM_SRD_STATE_READ_LENGTH_MASK))
		d->status |= UHARDDOOM_STATUS_SRD;
	/* SPAN busy iff waiting on SPANSEM or has non-0 pending draw length.  */
	if (d->span_state & (UHARDDOOM_SPAN_STATE_SPANSEM | UHARDDOOM_SPAN_STATE_DRAW_LENGTH_MASK))
		d->status |= UHARDDOOM_STATUS_SPAN;
	/* COL busy iff waiting on COLSEM, has non-0 pending draw length, or waiting on LOAD_CMAP_A.  */
	if (d->col_state & (UHARDDOOM_COL_STATE_COLSEM | UHARDDOOM_COL_STATE_LOAD_CMAP_A | UHARDDOOM_COL_STATE_DRAW_LENGTH_MASK))
		d->status |= UHARDDOOM_STATUS_COL;
	/* FX busy iff has non-0 pending draw length or has LOAD_CMAP or INIT_FUZZ operation in progress.  */
	if (d->fx_state & (UHARDDOOM_FX_STATE_DRAW_LENGTH_MASK | UHARDDOOM_FX_STATE_LOAD_MODE_MASK))
		d->status |= UHARDDOOM_STATUS_FX;
	/* SWR busy iff has non-0 pending draw length or has one of the SEM operations in progress.  */
	if (d->swr_state & (UHARDDOOM_SWR_STATE_DRAW_LENGTH_MASK | UHARDDOOM_SWR_STATE_SRDSEM | UHARDDOOM_SWR_STATE_COLSEM | UHARDDOOM_SWR_STATE_SPANSEM))
		d->status |= UHARDDOOM_STATUS_SWR;
	if (!uharddoom_srdcmd_empty(d))
		d->status |= UHARDDOOM_STATUS_FIFO_SRDCMD;
	if (!uharddoom_spancmd_empty(d))
		d->status |= UHARDDOOM_STATUS_FIFO_SPANCMD;
	if (!uharddoom_colcmd_empty(d))
		d->status |= UHARDDOOM_STATUS_FIFO_COLCMD;
	if (!uharddoom_fxcmd_empty(d))
		d->status |= UHARDDOOM_STATUS_FIFO_FXCMD;
	if (!uharddoom_swrcmd_empty(d))
		d->status |= UHARDDOOM_STATUS_FIFO_SWRCMD;
	if (!uharddoom_colin_empty(d))
		d->status |= UHARDDOOM_STATUS_FIFO_COLIN;
	if (!uharddoom_fxin_empty(d))
		d->status |= UHARDDOOM_STATUS_FIFO_FXIN;
	if (d->fesem)
		d->status |= UHARDDOOM_STATUS_FIFO_FESEM;
	if (d->srdsem)
		d->status |= UHARDDOOM_STATUS_FIFO_SRDSEM;
	if (d->colsem)
		d->status |= UHARDDOOM_STATUS_FIFO_COLSEM;
	if (d->spansem)
		d->status |= UHARDDOOM_STATUS_FIFO_SPANSEM;
	if (!uharddoom_spanout_empty(d))
		d->status |= UHARDDOOM_STATUS_FIFO_SPANOUT;
	if (!uharddoom_colout_empty(d))
		d->status |= UHARDDOOM_STATUS_FIFO_COLOUT;
	if (!uharddoom_fxout_empty(d))
		d->status |= UHARDDOOM_STATUS_FIFO_FXOUT;
	/* determine and set PCI interrupt status */
	pci_set_irq(PCI_DEVICE(d), !!(d->intr & d->intr_enable));
}

/* Resets TLBs, forcing a reread of PD/PT.  */
static void uharddoom_reset_tlb(UltimateHardDoomState *d, int which) {
	int i;
	for (i = 0; i < UHARDDOOM_TLB_CLIENT_NUM; i++) {
		bool isuser = i != UHARDDOOM_TLB_CLIENT_BATCH;
		if (which != isuser)
			continue;
		d->tlb_client_pte_tag[i] &= ~UHARDDOOM_TLB_TAG_VALID;
	}
	for (i = 0; i < UHARDDOOM_TLB_POOL_PDE_SIZE; i++) {
		bool isuser = !(d->tlb_pool_pde_tag[i] & UHARDDOOM_TLB_TAG_KERNEL);
		if (which != isuser)
			continue;
		d->tlb_pool_pde_tag[i] &= ~UHARDDOOM_TLB_TAG_VALID;
	}
	for (i = 0; i < UHARDDOOM_TLB_POOL_PTE_SIZE; i++) {
		bool isuser = !(d->tlb_pool_pte_tag[i] & UHARDDOOM_TLB_TAG_KERNEL);
		if (which != isuser)
			continue;
		d->tlb_pool_pte_tag[i] &= ~UHARDDOOM_TLB_TAG_VALID;
	}
}

/* Sets the given PD and flushes corresponding TLB.  */
static void uharddoom_set_pdp(UltimateHardDoomState *d, int which, uint32_t val) {
	val &= UHARDDOOM_PDP_MASK;
	if (which) {
		d->tlb_user_pdp = val;
	} else {
		d->tlb_kernel_pdp = val;
	}
	uharddoom_reset_tlb(d, which);
}

static uint32_t uharddoom_hash(uint32_t word) {
	uint32_t res = 0;
	while (word) {
		res ^= (word & 0x3f);
		word >>= 6;
	}
	return res;
}

/* Converts virtual offset to a physical address -- handles PT lookup.
 * If something goes wrong, disables the enable bit and fires an interrupt.
 * Returns true if succeeded.  */
static bool uharddoom_translate_addr(UltimateHardDoomState *d, int which, uint32_t offset, uint64_t *res, bool need_write) {
	uint8_t buf[4];
	uint32_t pte_tag = offset & 0xfffff000;
	uint32_t pde_tag = offset & 0xffc00000;
	pte_tag |= UHARDDOOM_TLB_TAG_VALID;
	pde_tag |= UHARDDOOM_TLB_TAG_VALID;
	if (which == UHARDDOOM_TLB_CLIENT_BATCH) {
		pte_tag |= UHARDDOOM_TLB_TAG_KERNEL;
		pde_tag |= UHARDDOOM_TLB_TAG_KERNEL;
	}
	d->tlb_client_va[which] = offset;
	if (d->tlb_client_pte_tag[which] != pte_tag) {
		/* No PTE, try the pool.  */
		int pteidx = uharddoom_hash(pte_tag);
		if (d->tlb_pool_pte_tag[pteidx] != pte_tag) {
			/* Will need to fetch PTE.  First, get PDE.  */
			int pdeidx = which != UHARDDOOM_TLB_CLIENT_BATCH;
			if (d->tlb_pool_pde_tag[pdeidx] != pde_tag) {
				/* Need to get a PDE.  */
				uint32_t pdp = which == UHARDDOOM_TLB_CLIENT_BATCH ? d->tlb_kernel_pdp : d->tlb_user_pdp;
				uint64_t pde_addr = (uint64_t)pdp << UHARDDOOM_PDP_SHIFT | UHARDDOOM_VA_PDI(offset) << 2;
				pci_dma_read(&d->dev, pde_addr, &buf, sizeof buf);
				d->tlb_pool_pde_tag[pdeidx] = pde_tag;
				d->tlb_pool_pde_value[pdeidx] = le32_read(buf);
			}
			uint32_t pde = d->tlb_pool_pde_value[pdeidx];
			if (!(pde & UHARDDOOM_PDE_PRESENT))
				goto oops;
			uint64_t pte_addr = (uint64_t)(pde & UHARDDOOM_PDE_PA_MASK) << UHARDDOOM_PDE_PA_SHIFT | UHARDDOOM_VA_PTI(offset) << 2;
			pci_dma_read(&d->dev, pte_addr, &buf, sizeof buf);
			d->tlb_pool_pte_tag[pteidx] = pte_tag;
			d->tlb_pool_pte_value[pteidx] = le32_read(buf);
		}
		d->tlb_client_pte_tag[which] = pte_tag;
		d->tlb_client_pte_value[which] = d->tlb_pool_pte_value[pteidx];
	}
	uint32_t pte = d->tlb_client_pte_value[which];
	if (!(pte & UHARDDOOM_PTE_PRESENT))
		goto oops;
	if (!(pte & UHARDDOOM_PTE_WRITABLE) && need_write)
		goto oops;
	*res = (uint64_t)(pte & UHARDDOOM_PTE_PA_MASK) << UHARDDOOM_PTE_PA_SHIFT | UHARDDOOM_VA_OFF(offset);
	return true;
oops:
	d->intr |= UHARDDOOM_INTR_PAGE_FAULT(which);
	return false;
}

/* Resets a given CACHE, flushing all data.  */
static void uharddoom_reset_cache(UltimateHardDoomState *d, int which) {
	for (int i = 0; i < UHARDDOOM_CACHE_SIZE; i++)
		d->cache_tag[which * UHARDDOOM_CACHE_SIZE + i] &= ~UHARDDOOM_CACHE_TAG_VALID;
}

/* Reads a byte thru the CACHE, returns true if succeeded.  */
static bool uharddoom_cache_read(UltimateHardDoomState *d, int which, uint32_t addr, uint8_t *dst, bool speculative) {
	uint32_t offset = addr & UHARDDOOM_BLOCK_MASK;
	uint32_t va = addr & ~UHARDDOOM_BLOCK_MASK;
	uint32_t set = uharddoom_hash(va);
	uint32_t tag = va | UHARDDOOM_CACHE_TAG_VALID;
	if (d->cache_tag[which * UHARDDOOM_CACHE_SIZE + set] != tag) {
		if (speculative)
			return false;
		uint64_t pa;
		if (!uharddoom_translate_addr(d, which + 4, va, &pa, false))
			return false;
		pci_dma_read(&d->dev, pa, &d->cache_data[(which * UHARDDOOM_CACHE_SIZE + set) * UHARDDOOM_BLOCK_SIZE], UHARDDOOM_BLOCK_SIZE);
		d->cache_tag[which * UHARDDOOM_CACHE_SIZE + set] = tag;
	}
	*dst = d->cache_data[(which * UHARDDOOM_CACHE_SIZE + set) * UHARDDOOM_BLOCK_SIZE + offset];
	//printf("CACHE READ %d %08x -> %02x\n", which, addr, *dst);
	return true;
}

static void uharddoom_reset(UltimateHardDoomState *d, uint32_t val) {
	int i;
	if (val & UHARDDOOM_RESET_JOB)
		d->job_state = 0;
	if (val & UHARDDOOM_RESET_CMD) {
		d->cmd_fifo_get = 0;
		d->cmd_fifo_put = 0;
		d->cmd_read_size = 0;
	}
	if (val & UHARDDOOM_RESET_FE) {
		d->fe_state = 0;
		d->fe_pc = UHARDDOOM_FEMEM_CODE_BASE;
	}
	if (val & UHARDDOOM_RESET_SRD)
		d->srd_state = 0;
	if (val & UHARDDOOM_RESET_SPAN)
		d->span_state = 0;
	if (val & UHARDDOOM_RESET_COL) {
		d->col_state = 0;
		for (i = 0; i < UHARDDOOM_BLOCK_SIZE; i++)
			d->col_cols_state[i] = 0;
	}
	if (val & UHARDDOOM_RESET_FX) {
		d->fx_state = 0;
		for (i = 0; i < UHARDDOOM_BLOCK_SIZE; i++)
			d->fx_col[i] = 0;
	}
	if (val & UHARDDOOM_RESET_SWR)
		d->swr_state = 0;
	if (val & UHARDDOOM_RESET_STATS) {
		/* XXX */
	}
	if (val & UHARDDOOM_RESET_TLB_KERNEL)
		uharddoom_reset_tlb(d, 0);
	if (val & UHARDDOOM_RESET_TLB_USER)
		uharddoom_reset_tlb(d, 1);
	if (val & UHARDDOOM_RESET_CACHE_COL_CMAP_B)
		uharddoom_reset_cache(d, UHARDDOOM_CACHE_CLIENT_COL_CMAP_B);
	if (val & UHARDDOOM_RESET_CACHE_COL_SRC)
		uharddoom_reset_cache(d, UHARDDOOM_CACHE_CLIENT_COL_SRC);
	if (val & UHARDDOOM_RESET_CACHE_SPAN_SRC)
		uharddoom_reset_cache(d, UHARDDOOM_CACHE_CLIENT_SPAN_SRC);
	if (val & UHARDDOOM_RESET_CACHE_SWR_TRANSMAP)
		uharddoom_reset_cache(d, UHARDDOOM_CACHE_CLIENT_SWR_TRANSMAP);
	if (val & UHARDDOOM_RESET_FIFO_SRDCMD)
		d->srdcmd_get = d->srdcmd_put = 0;
	if (val & UHARDDOOM_RESET_FIFO_SPANCMD)
		d->spancmd_get = d->spancmd_put = 0;
	if (val & UHARDDOOM_RESET_FIFO_COLCMD)
		d->colcmd_get = d->colcmd_put = 0;
	if (val & UHARDDOOM_RESET_FIFO_FXCMD)
		d->fxcmd_get = d->fxcmd_put = 0;
	if (val & UHARDDOOM_RESET_FIFO_SWRCMD)
		d->swrcmd_get = d->swrcmd_put = 0;
	if (val & UHARDDOOM_RESET_FIFO_COLIN)
		d->colin_get = d->colin_put = 0;
	if (val & UHARDDOOM_RESET_FIFO_FXIN)
		d->fxin_get = d->fxin_put = 0;
	if (val & UHARDDOOM_RESET_FIFO_FESEM)
		d->fesem = 0;
	if (val & UHARDDOOM_RESET_FIFO_SRDSEM)
		d->srdsem = 0;
	if (val & UHARDDOOM_RESET_FIFO_COLSEM)
		d->colsem = 0;
	if (val & UHARDDOOM_RESET_FIFO_SPANSEM)
		d->spansem = 0;
	if (val & UHARDDOOM_RESET_FIFO_SPANOUT)
		d->spanout_get = d->spanout_put = 0;
	if (val & UHARDDOOM_RESET_FIFO_COLOUT)
		d->colout_get = d->colout_put = 0;
	if (val & UHARDDOOM_RESET_FIFO_FXOUT)
		d->fxout_get = d->fxout_put = 0;
}

static void uharddoom_job_trigger(UltimateHardDoomState *d, bool from_batch) {
	int i;
	d->job_state = UHARDDOOM_JOB_STATE_ACTIVE;
	if (from_batch)
		d->job_state |= UHARDDOOM_JOB_STATE_FROM_BATCH;
	uharddoom_set_pdp(d, 1, d->job_pdp);
	for (i = 0; i < UHARDDOOM_CACHE_CLIENT_NUM; i++)
		uharddoom_reset_cache(d, i);
	d->cmd_fe_ptr = d->cmd_read_ptr = d->job_cmd_ptr;
	d->cmd_read_size = d->job_cmd_size;
}

static void uharddoom_job_done(UltimateHardDoomState *d) {
	d->job_state &= ~UHARDDOOM_JOB_STATE_ACTIVE;
	d->intr |= UHARDDOOM_INTR_JOB_DONE;
	if (d->job_state & UHARDDOOM_JOB_STATE_FROM_BATCH) {
		d->batch_get += UHARDDOOM_BATCH_JOB_SIZE;
		if (d->batch_get == d->batch_wrap_from)
			d->batch_get = d->batch_wrap_to;
		if (d->batch_get == d->batch_wait)
			d->intr |= UHARDDOOM_INTR_BATCH_WAIT;
	}
}

static uint8_t uharddoom_mmio_readb(UltimateHardDoomState *d, hwaddr addr)
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
	if (addr >= window && addr < window + UHARDDOOM_BLOCK_SIZE) \
		return d->data[d->name##_get * UHARDDOOM_BLOCK_SIZE + (addr - window)];
#define FIFO_DATA64(name, hname, data, window)
#include "uharddoom-regs.h"
	fprintf(stderr, "uharddoom error: byte-sized read at %03x\n", (int)addr);
	return 0xff;
}

static void uharddoom_mmio_writeb(UltimateHardDoomState *d, hwaddr addr, uint8_t val)
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
	if (addr >= window && addr < window + UHARDDOOM_BLOCK_SIZE) { \
		d->data[d->name##_put * UHARDDOOM_BLOCK_SIZE + (addr - window)] = val; \
		return; \
	}
#define FIFO_DATA64(name, hname, data, window)
#include "uharddoom-regs.h"
	fprintf(stderr, "uharddoom error: byte-sized write at %03x, value %02x\n", (int)addr, val);
}

static uint32_t uharddoom_mmio_readl(UltimateHardDoomState *d, hwaddr addr)
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
#include "uharddoom-regs.h"
	case UHARDDOOM_STATUS: return d->status;
	case UHARDDOOM_FE_CODE_WINDOW: {
		uint32_t res = le32_read(d->fe_code + d->fe_code_addr);
		d->fe_code_addr += 4;
		d->fe_code_addr &= UHARDDOOM_FE_CODE_ADDR_MASK;
		return res;
	}
	case UHARDDOOM_FE_DATA_WINDOW: {
		uint32_t res = le32_read(d->fe_data + d->fe_data_addr);
		d->fe_data_addr += 4;
		d->fe_data_addr &= UHARDDOOM_FE_DATA_ADDR_MASK;
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
#include "uharddoom-regs.h"
	fprintf(stderr, "uharddoom error: invalid register read at %03x\n", (int)addr);
	return 0xffffffff;
}

static void uharddoom_mmio_writel(UltimateHardDoomState *d, hwaddr addr, uint32_t val)
{
	/* Has to be a separate switch, because it duplicates values from the header. */
	switch (addr) {
		case UHARDDOOM_INTR:
			d->intr &= ~val;
			return;
		case UHARDDOOM_BATCH_PDP:
			d->batch_pdp = val & UHARDDOOM_PDP_MASK;
			uharddoom_set_pdp(d, 0, d->batch_pdp);
			return;
		case UHARDDOOM_FE_REG(0):
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
#include "uharddoom-regs.h"
	case UHARDDOOM_RESET: uharddoom_reset(d, val); return;
	case UHARDDOOM_JOB_TRIGGER: uharddoom_job_trigger(d, false); return;
	case UHARDDOOM_FE_CODE_WINDOW: {
		le32_write(d->fe_code + d->fe_code_addr, val);
		d->fe_code_addr += 4;
		d->fe_code_addr &= UHARDDOOM_FE_CODE_ADDR_MASK;
		return;
	}
	case UHARDDOOM_FE_DATA_WINDOW: {
		le32_write(d->fe_data + d->fe_data_addr, val);
		d->fe_data_addr += 4;
		d->fe_data_addr &= UHARDDOOM_FE_DATA_ADDR_MASK;
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
#include "uharddoom-regs.h"
	fprintf(stderr, "uharddoom error: invalid register write at %03x, value %08x\n", (int)addr, val);
}

static uint64_t uharddoom_mmio_read(void *opaque, hwaddr addr, unsigned size) {
	uint64_t res = ~0ULL;
	UltimateHardDoomState *d = opaque;
	qemu_mutex_lock(&d->mutex);
	if (size == 1) {
		res = uharddoom_mmio_readb(d, addr);
	} else if (size == 4) {
		res = uharddoom_mmio_readl(d, addr);
	} else {
		fprintf(stderr, "uharddoom error: invalid register read of size %u at %03x\n", size, (int)addr);
	}
	uharddoom_status_update(d);
	qemu_cond_signal(&d->cond);
	qemu_mutex_unlock(&d->mutex);
	return res;
}

static void uharddoom_mmio_write(void *opaque, hwaddr addr, uint64_t val,
                unsigned size)
{
	UltimateHardDoomState *d = opaque;
	qemu_mutex_lock(&d->mutex);
	if (size == 1) {
		uharddoom_mmio_writeb(d, addr, val);
	} else if (size == 4) {
		uharddoom_mmio_writel(d, addr, val);
	} else {
		fprintf(stderr, "uharddoom error: invalid register write of size %u at %03x\n", size, (int)addr);
	}
	uharddoom_status_update(d);
	qemu_cond_signal(&d->cond);
	qemu_mutex_unlock(&d->mutex);
}

static const MemoryRegionOps uharddoom_mmio_ops = {
	.read = uharddoom_mmio_read,
	.write = uharddoom_mmio_write,
	.endianness = DEVICE_LITTLE_ENDIAN,
};

/* Power-up reset of the device.  */
static void uharddoom_power_reset(DeviceState *ds)
{
	UltimateHardDoomState *d = UHARDDOOM_DEV(ds);
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
	for (i = 0; i < hname##_SIZE * UHARDDOOM_BLOCK_SIZE; i++) \
		d->data[i] = mrand48();
#include "uharddoom-regs.h"
	for (i = 0; i < UHARDDOOM_FEMEM_CODE_SIZE; i++)
		d->fe_code[i] = mrand48();
	for (i = 0; i < UHARDDOOM_FEMEM_DATA_SIZE; i++)
		d->fe_data[i] = mrand48();
	/* These registers play fair.  */
	d->enable = 0;
	d->intr_enable = 0;
	/* No force can change this register.  */
	d->fe_regs[0] = 0;
	uharddoom_status_update(d);
	qemu_mutex_unlock(&d->mutex);
}

/* Runs BATCH for some time.  Returns true if anything has been done.  */
static bool uharddoom_run_batch(UltimateHardDoomState *d) {
	if (!(d->enable & UHARDDOOM_ENABLE_BATCH))
		return false;
	if (d->batch_get == d->batch_put)
		return false;
	if (d->job_state & UHARDDOOM_JOB_STATE_ACTIVE)
		return false;
	uint64_t pa;
	uint8_t buf[UHARDDOOM_BATCH_JOB_SIZE];
	if (!uharddoom_translate_addr(d, UHARDDOOM_TLB_CLIENT_BATCH, d->batch_get, &pa, false)) {
		d->enable &= ~UHARDDOOM_ENABLE_BATCH;
		return false;
	}
	pci_dma_read(&d->dev, pa, &buf, sizeof buf);
	d->job_pdp = le32_read(buf) & UHARDDOOM_PDP_MASK;
	d->job_cmd_ptr = le32_read(buf + 4) & UHARDDOOM_CMD_PTR_MASK;
	d->job_cmd_size = le32_read(buf + 8) & UHARDDOOM_CMD_SIZE_MASK;
	uharddoom_job_trigger(d, true);
	return true;
}

/* Runs CMD for some time.  Returns true if anything has been done.  */
static bool uharddoom_run_cmd(UltimateHardDoomState *d) {
	if (!(d->enable & UHARDDOOM_ENABLE_CMD))
		return false;
	if (!d->cmd_read_size)
		return false;
	if (uharddoom_cmd_fifo_full(d))
		return false;
	uint64_t pa;
	uint8_t buf[0x40];
	if (!uharddoom_translate_addr(d, UHARDDOOM_TLB_CLIENT_CMD, d->cmd_read_ptr, &pa, false)) {
		d->enable &= ~UHARDDOOM_ENABLE_CMD;
		return false;
	}
	uint32_t rsz = sizeof buf;
	if (rsz > d->cmd_read_size)
		rsz = d->cmd_read_size;
	uint32_t to_page_end = 0x1000 - (d->cmd_read_ptr & 0xfff);
	if (rsz > to_page_end)
		rsz = to_page_end;
	pci_dma_read(&d->dev, pa, &buf, rsz);
	uint32_t pos = 0;
	while (pos < rsz) {
		uint32_t word = le32_read(buf + pos);
		d->cmd_fifo_data[d->cmd_fifo_put] = word;
		uharddoom_cmd_fifo_write(d);
		d->cmd_read_ptr += 4;
		d->cmd_read_size -= 4;
		pos += 4;
		if (uharddoom_cmd_fifo_full(d))
			return true;
	}
	return true;
}

static void uharddoom_fe_error(UltimateHardDoomState *d, uint32_t code, uint32_t data_a, uint32_t data_b) {
	d->fe_error_code = code;
	d->fe_error_data_a = data_a;
	d->fe_error_data_b = data_b;
	d->fe_state &= ~UHARDDOOM_FE_STATE_STATE_MASK;
	d->fe_state |= UHARDDOOM_FE_STATE_STATE_ERROR;
	d->intr |= UHARDDOOM_INTR_FE_ERROR;
}

/* Runs the FE for some time.  Returns true if anything has been done.  */
static bool uharddoom_run_fe(UltimateHardDoomState *d) {
	bool any = false;
	if (!(d->enable & UHARDDOOM_ENABLE_FE))
		return any;
	int num_insns = 0x1000;
	while (num_insns--) {
		uint32_t state = d->fe_state & UHARDDOOM_FE_STATE_STATE_MASK;
		uint32_t sdst = (d->fe_state & UHARDDOOM_FE_STATE_DST_MASK) >> UHARDDOOM_FE_STATE_DST_SHIFT;
		uint32_t cmd = (d->fe_state & UHARDDOOM_FE_STATE_CMD_MASK) >> UHARDDOOM_FE_STATE_CMD_SHIFT;
		switch (state) {
			case UHARDDOOM_FE_STATE_STATE_RUNNING:
				break;
			case UHARDDOOM_FE_STATE_STATE_ERROR:
				return any;
			case UHARDDOOM_FE_STATE_STATE_JOB_WAIT:
				if (!(d->job_state & UHARDDOOM_JOB_STATE_ACTIVE))
					return any;
				d->fe_regs[sdst] = 0;
				break;
			case UHARDDOOM_FE_STATE_STATE_CMD_FETCH:
				if (uharddoom_cmd_fifo_empty(d)) {
					if (d->enable & UHARDDOOM_ENABLE_CMD && !d->cmd_read_size) {
						d->intr |= UHARDDOOM_INTR_CMD_ERROR;
						d->enable &= ~UHARDDOOM_ENABLE_CMD;
					}
					return any;
				}
				if (sdst)
					d->fe_regs[sdst] = d->cmd_fifo_data[d->cmd_fifo_get];
				d->cmd_fe_ptr += 4;
				uharddoom_cmd_fifo_read(d);
				break;
			case UHARDDOOM_FE_STATE_STATE_SRDCMD:
				if (uharddoom_srdcmd_full(d))
					return any;
				d->srdcmd_cmd[d->srdcmd_put] = cmd;
				d->srdcmd_data[d->srdcmd_put] = d->fe_write_data;
				uharddoom_srdcmd_write(d);
				break;
			case UHARDDOOM_FE_STATE_STATE_SPANCMD:
				if (uharddoom_spancmd_full(d))
					return any;
				d->spancmd_cmd[d->spancmd_put] = cmd;
				d->spancmd_data[d->spancmd_put] = d->fe_write_data;
				uharddoom_spancmd_write(d);
				break;
			case UHARDDOOM_FE_STATE_STATE_COLCMD:
				if (uharddoom_colcmd_full(d))
					return any;
				d->colcmd_cmd[d->colcmd_put] = cmd;
				d->colcmd_data[d->colcmd_put] = d->fe_write_data;
				uharddoom_colcmd_write(d);
				break;
			case UHARDDOOM_FE_STATE_STATE_FXCMD:
				if (uharddoom_fxcmd_full(d))
					return any;
				d->fxcmd_cmd[d->fxcmd_put] = cmd;
				d->fxcmd_data[d->fxcmd_put] = d->fe_write_data;
				uharddoom_fxcmd_write(d);
				break;
			case UHARDDOOM_FE_STATE_STATE_SWRCMD:
				if (uharddoom_swrcmd_full(d))
					return any;
				d->swrcmd_cmd[d->swrcmd_put] = cmd;
				d->swrcmd_data[d->swrcmd_put] = d->fe_write_data;
				uharddoom_swrcmd_write(d);
				break;
			case UHARDDOOM_FE_STATE_STATE_FESEM:
				if (!d->fesem)
					return any;
				d->fe_regs[sdst] = 0;
				d->fesem = 0;
				break;
		}
		/* If we got past this, we should now be in the running state.  */
		d->fe_state &= ~UHARDDOOM_FE_STATE_STATE_MASK;
		/* One way or another, we're going to do something.  */
		any = true;
		if (d->fe_pc < UHARDDOOM_FEMEM_CODE_BASE || d->fe_pc >= UHARDDOOM_FEMEM_CODE_BASE + UHARDDOOM_FEMEM_CODE_SIZE) {
			uharddoom_fe_error(d, UHARDDOOM_FE_ERROR_CODE_BUS_ERROR_EXEC, d->fe_pc, 0);
			return any;
			
		}
		uint32_t new_pc = d->fe_pc + 4;
		uint32_t insn = le32_read(d->fe_code + (d->fe_pc - UHARDDOOM_FEMEM_CODE_BASE));
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
				switch (f3) {
					case 0:
						/* LB */
					case 4:
						/* LBU */
						if (addr >= UHARDDOOM_FEMEM_CODE_BASE && addr < UHARDDOOM_FEMEM_CODE_BASE + UHARDDOOM_FEMEM_CODE_SIZE)

							r = d->fe_code[addr - UHARDDOOM_FEMEM_CODE_BASE];
						else if (addr >= UHARDDOOM_FEMEM_DATA_BASE && addr < UHARDDOOM_FEMEM_DATA_BASE + UHARDDOOM_FEMEM_DATA_SIZE)

							r = d->fe_data[addr - UHARDDOOM_FEMEM_DATA_BASE];
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
						if (addr >= UHARDDOOM_FEMEM_CODE_BASE && addr < UHARDDOOM_FEMEM_CODE_BASE + UHARDDOOM_FEMEM_CODE_SIZE)

							r = le16_read(d->fe_code + (addr - UHARDDOOM_FEMEM_CODE_BASE));
						else if (addr >= UHARDDOOM_FEMEM_DATA_BASE && addr < UHARDDOOM_FEMEM_DATA_BASE + UHARDDOOM_FEMEM_DATA_SIZE)

							r = le16_read(d->fe_data + (addr - UHARDDOOM_FEMEM_DATA_BASE));
						else
							goto bus_error_load;
						if (f3 == 0 && r & 0x8000)
							r |= 0xffff0000;
						break;
					case 2:
						/* LW */
						if (addr & 3)
							goto bus_error_load;
						if (addr >= UHARDDOOM_FEMEM_CODE_BASE && addr < UHARDDOOM_FEMEM_CODE_BASE + UHARDDOOM_FEMEM_CODE_SIZE)

							r = le32_read(d->fe_code + (addr - UHARDDOOM_FEMEM_CODE_BASE));
						else if (addr >= UHARDDOOM_FEMEM_DATA_BASE && addr < UHARDDOOM_FEMEM_DATA_BASE + UHARDDOOM_FEMEM_DATA_SIZE)

							r = le32_read(d->fe_data + (addr - UHARDDOOM_FEMEM_DATA_BASE));
						else if (addr == UHARDDOOM_FEMEM_JOB_WAIT) {
							state = UHARDDOOM_FE_STATE_STATE_JOB_WAIT;
							goto long_load;
						}
						else if (addr == UHARDDOOM_FEMEM_CMD_PTR)
							r = d->cmd_fe_ptr;
						else if (addr == UHARDDOOM_FEMEM_CMD_END)
							r = !d->cmd_read_size && uharddoom_cmd_fifo_empty(d);
						else if (addr == UHARDDOOM_FEMEM_CMD_FETCH) {
							state = UHARDDOOM_FE_STATE_STATE_CMD_FETCH;
							goto long_load;
						}
						else if (addr == UHARDDOOM_FEMEM_FESEM) {
							state = UHARDDOOM_FE_STATE_STATE_FESEM;
							goto long_load;
						}
						else
							goto bus_error_load;
						break;
					default:
						goto illegal;
					bus_error_load:
						uharddoom_fe_error(d, UHARDDOOM_FE_ERROR_CODE_BUS_ERROR_LOAD, addr, 0);
						return true;
					long_load:
						d->fe_state &= ~UHARDDOOM_FE_STATE_STATE_MASK;
						d->fe_state &= ~UHARDDOOM_FE_STATE_DST_MASK;
						d->fe_state |= state;
						d->fe_state |= rd << UHARDDOOM_FE_STATE_DST_SHIFT;
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
				uint32_t cmd = addr >> 2 & UHARDDOOM_FIFO_CMD_MASK;
				switch (f3) {
					case 0:
						/* SB */
						if (addr >= UHARDDOOM_FEMEM_DATA_BASE && addr < UHARDDOOM_FEMEM_DATA_BASE + UHARDDOOM_FEMEM_DATA_SIZE)

							d->fe_data[addr - UHARDDOOM_FEMEM_DATA_BASE] = val;
						else
							goto bus_error_store;
						break;
					case 1:
						/* SH */
						if (addr & 1)
							goto bus_error_store;
						if (addr >= UHARDDOOM_FEMEM_DATA_BASE && addr < UHARDDOOM_FEMEM_DATA_BASE + UHARDDOOM_FEMEM_DATA_SIZE)

							le16_write(d->fe_data + (addr - UHARDDOOM_FEMEM_DATA_BASE), val);
						else
							goto bus_error_store;
						break;
					case 2:
						/* SW */
						if (addr & 3)
							goto bus_error_store;
						if (addr >= UHARDDOOM_FEMEM_DATA_BASE && addr < UHARDDOOM_FEMEM_DATA_BASE + UHARDDOOM_FEMEM_DATA_SIZE)

							le32_write(d->fe_data + (addr - UHARDDOOM_FEMEM_DATA_BASE), val);
						else if (addr == UHARDDOOM_FEMEM_FE_ERROR_DATA_A)
							d->fe_error_data_a = val;
						else if (addr == UHARDDOOM_FEMEM_FE_ERROR_DATA_B)
							d->fe_error_data_b = val;
						else if (addr == UHARDDOOM_FEMEM_FE_ERROR_CODE) {
							d->fe_error_code = val;
							d->fe_state &= ~UHARDDOOM_FE_STATE_STATE_MASK;
							d->fe_state |= UHARDDOOM_FE_STATE_STATE_ERROR;
							d->intr |= UHARDDOOM_INTR_FE_ERROR;
						}
						else if ((addr & ~0x3c) == UHARDDOOM_FEMEM_SRDCMD(0)) {
							state = UHARDDOOM_FE_STATE_STATE_SRDCMD;
							goto long_store;
						}
						else if ((addr & ~0x3c) == UHARDDOOM_FEMEM_SPANCMD(0)) {
							state = UHARDDOOM_FE_STATE_STATE_SPANCMD;
							goto long_store;
						}
						else if ((addr & ~0x3c) == UHARDDOOM_FEMEM_COLCMD(0)) {
							state = UHARDDOOM_FE_STATE_STATE_COLCMD;
							goto long_store;
						}
						else if ((addr & ~0x3c) == UHARDDOOM_FEMEM_FXCMD(0)) {
							state = UHARDDOOM_FE_STATE_STATE_FXCMD;
							goto long_store;
						}
						else if ((addr & ~0x3c) == UHARDDOOM_FEMEM_SWRCMD(0)) {
							state = UHARDDOOM_FE_STATE_STATE_SWRCMD;
							goto long_store;
						}
						else if (addr == UHARDDOOM_FEMEM_JOB_DONE)
							uharddoom_job_done(d);
						else
							goto bus_error_store;
						break;
					default:
						goto illegal;
					bus_error_store:
						uharddoom_fe_error(d, UHARDDOOM_FE_ERROR_CODE_BUS_ERROR_STORE, addr, val);
						return true;
					long_store:
						d->fe_state &= ~UHARDDOOM_FE_STATE_STATE_MASK;
						d->fe_state &= ~UHARDDOOM_FE_STATE_CMD_MASK;
						d->fe_state |= state;
						d->fe_state |= cmd << UHARDDOOM_FE_STATE_CMD_SHIFT;
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
				uharddoom_fe_error(d, UHARDDOOM_FE_ERROR_CODE_ILLEGAL_INSTRUCTION, d->fe_pc, insn);
				return true;
		}
next_insn:
		if (new_pc & 3) {
			uharddoom_fe_error(d, UHARDDOOM_FE_ERROR_CODE_UNALIGNED_INSTRUCTION, d->fe_pc, new_pc);
			return true;
		}
		d->fe_pc = new_pc;
	}
	return any;
}

/* Runs SRD for some time.  Returns true if anything has been done.  */
static bool uharddoom_run_srd(UltimateHardDoomState *d) {
	if (!(d->enable & UHARDDOOM_ENABLE_SRD))
		return false;
	bool any = false;
	while (true) {
		if (d->srd_state & UHARDDOOM_SRD_STATE_SRDSEM) {
			if (!d->srdsem)
				return any;
			d->srdsem = 0;
			d->srd_state &= ~UHARDDOOM_SRD_STATE_SRDSEM;
			any = true;
		} else if (d->srd_state & UHARDDOOM_SRD_STATE_FESEM) {
			if (d->fesem)
				return any;
			d->fesem = 1;
			d->srd_state &= ~UHARDDOOM_SRD_STATE_FESEM;
			any = true;
		} else if (d->srd_state & UHARDDOOM_SRD_STATE_READ_LENGTH_MASK) {
			uint8_t *ptr;
			if (d->srd_state & UHARDDOOM_SRD_STATE_COL) {
				if (uharddoom_colin_full(d))
					return any;
				ptr = &d->colin_data[d->colin_put * UHARDDOOM_BLOCK_SIZE];
			} else {
				if (uharddoom_fxin_full(d))
					return any;
				ptr = &d->fxin_data[d->fxin_put * UHARDDOOM_BLOCK_SIZE];
			}
			uint64_t pa;
			if (!uharddoom_translate_addr(d, UHARDDOOM_TLB_CLIENT_SRD, d->srd_src_ptr, &pa, false)) {
				d->enable &= ~UHARDDOOM_ENABLE_SRD;
				return any;
			}
			pci_dma_read(&d->dev, pa, ptr, UHARDDOOM_BLOCK_SIZE);
			if (d->srd_state & UHARDDOOM_SRD_STATE_COL)
				uharddoom_colin_write(d);
			else
				uharddoom_fxin_write(d);
			d->srd_src_ptr += d->srd_src_pitch;
			d->srd_state--;
			any = true;
		} else {
			/* No command. get one.  */
			if (uharddoom_srdcmd_empty(d))
				return any;
			uint32_t cmd = d->srdcmd_cmd[d->srdcmd_get];
			uint32_t data = d->srdcmd_data[d->srdcmd_get];
			uharddoom_srdcmd_read(d);
			//printf("SRDCMD %08x %08x\n", cmd, data);
			any = true;
			switch (cmd) {
				case UHARDDOOM_SRDCMD_TYPE_SRC_PTR:
					d->srd_src_ptr = data & UHARDDOOM_BLOCK_PTR_MASK;
					break;
				case UHARDDOOM_SRDCMD_TYPE_SRC_PITCH:
					d->srd_src_pitch = data & UHARDDOOM_BLOCK_PITCH_MASK;
					break;
				case UHARDDOOM_SRDCMD_TYPE_READ:
					d->srd_state = data & (UHARDDOOM_SRD_STATE_READ_LENGTH_MASK | UHARDDOOM_SRD_STATE_COL);
					break;
				case UHARDDOOM_SRDCMD_TYPE_SRDSEM:
					d->srd_state |= UHARDDOOM_SRD_STATE_SRDSEM;
					break;
				case UHARDDOOM_SRDCMD_TYPE_FESEM:
					d->srd_state |= UHARDDOOM_SRD_STATE_FESEM;
					break;
			}
		}
	}
}

/* Runs SPAN for some time.  Returns true if anything has been done.  */
static bool uharddoom_run_span(UltimateHardDoomState *d) {
	if (!(d->enable & UHARDDOOM_ENABLE_SPAN))
		return false;
	bool any = false;
	while (true) {
		if (d->span_state & UHARDDOOM_SPAN_STATE_SPANSEM) {
			if (!d->spansem)
				return any;
			d->spansem = 0;
			d->span_state &= ~UHARDDOOM_SPAN_STATE_SPANSEM;
			uharddoom_reset_cache(d, UHARDDOOM_CACHE_CLIENT_SPAN_SRC);
			any = true;
		} else if (d->span_state & UHARDDOOM_SPAN_STATE_DRAW_LENGTH_MASK) {
			if (uharddoom_spanout_full(d))
				return any;
			uint32_t xoff = (d->span_state & UHARDDOOM_SPAN_STATE_DRAW_XOFF_MASK) >> UHARDDOOM_SPAN_STATE_DRAW_XOFF_SHIFT;
			while (true) {
				uint32_t ui = d->span_ustart >> 16 & ((1u << UHARDDOOM_SPANCMD_DATA_EXTR_UVMASK_ULOG(d->span_uvmask)) - 1);
				uint32_t vi = d->span_vstart >> 16 & ((1u << UHARDDOOM_SPANCMD_DATA_EXTR_UVMASK_VLOG(d->span_uvmask)) - 1);
				uint32_t addr = d->span_src_ptr + ui + d->span_src_pitch * vi;
				uint8_t *dst = &d->spanout_data[d->spanout_put * UHARDDOOM_BLOCK_SIZE + xoff];
				if (!uharddoom_cache_read(d, UHARDDOOM_CACHE_CLIENT_SPAN_SRC, addr, dst, false)) {
					d->enable &= ~UHARDDOOM_ENABLE_SPAN;
					d->span_state &= ~UHARDDOOM_SPAN_STATE_DRAW_XOFF_MASK;
					d->span_state |= xoff << UHARDDOOM_SPAN_STATE_DRAW_XOFF_SHIFT;
					return any;
				}
				d->span_ustart += d->span_ustep;
				d->span_vstart += d->span_vstep;
				xoff++;
				d->span_state--;
				any = true;
				if (xoff == UHARDDOOM_BLOCK_SIZE || !(d->span_state & UHARDDOOM_SPAN_STATE_DRAW_LENGTH_MASK)) {
					uharddoom_spanout_write(d);
					xoff = 0;
					break;
				}
			}
			d->span_state &= ~UHARDDOOM_SPAN_STATE_DRAW_XOFF_MASK;
			d->span_state |= xoff << UHARDDOOM_SPAN_STATE_DRAW_XOFF_SHIFT;
		} else {
			/* No command. get one.  */
			if (uharddoom_spancmd_empty(d))
				return any;
			uint32_t cmd = d->spancmd_cmd[d->spancmd_get];
			uint32_t data = d->spancmd_data[d->spancmd_get];
			uharddoom_spancmd_read(d);
			//printf("SPANCMD %08x %08x\n", cmd, data);
			any = true;
			switch (cmd) {
				case UHARDDOOM_SPANCMD_TYPE_SRC_PTR:
					d->span_src_ptr = data;
					break;
				case UHARDDOOM_SPANCMD_TYPE_SRC_PITCH:
					d->span_src_pitch = data;
					break;
				case UHARDDOOM_SPANCMD_TYPE_UVMASK:
					d->span_uvmask = data & UHARDDOOM_SPAN_UVMASK_MASK;
					break;
				case UHARDDOOM_SPANCMD_TYPE_USTART:
					d->span_ustart = data;
					break;
				case UHARDDOOM_SPANCMD_TYPE_VSTART:
					d->span_vstart = data;
					break;
				case UHARDDOOM_SPANCMD_TYPE_USTEP:
					d->span_ustep = data;
					break;
				case UHARDDOOM_SPANCMD_TYPE_VSTEP:
					d->span_vstep = data;
					break;
				case UHARDDOOM_SPANCMD_TYPE_DRAW:
					d->span_state = data & (UHARDDOOM_SPAN_STATE_DRAW_LENGTH_MASK | UHARDDOOM_SPAN_STATE_DRAW_XOFF_MASK);
					break;
				case UHARDDOOM_SPANCMD_TYPE_SPANSEM:
					d->span_state |= UHARDDOOM_SPAN_STATE_SPANSEM;
					break;
			}
		}
	}
}

/* Runs COL for some time.  Returns true if anything has been done.  */
static bool uharddoom_run_col(UltimateHardDoomState *d) {
	if (!(d->enable & UHARDDOOM_ENABLE_COL))
		return false;
	bool any = false;
	while (true) {
		if (d->col_state & UHARDDOOM_COL_STATE_COLSEM) {
			if (!d->colsem)
				return any;
			d->colsem = 0;
			d->col_state &= ~UHARDDOOM_COL_STATE_COLSEM;
			uharddoom_reset_cache(d, UHARDDOOM_CACHE_CLIENT_COL_SRC);
			uharddoom_reset_cache(d, UHARDDOOM_CACHE_CLIENT_COL_CMAP_B);
			any = true;
		} else if (d->col_state & UHARDDOOM_COL_STATE_LOAD_CMAP_A) {
			if (uharddoom_colin_empty(d))
				return any;
			int pos = (d->col_state & UHARDDOOM_COL_STATE_CMAP_A_POS_MASK) >> UHARDDOOM_COL_STATE_CMAP_A_POS_SHIFT;
			for (int i = 0; i < UHARDDOOM_BLOCK_SIZE; i++)
				d->col_cmap_a[pos * UHARDDOOM_BLOCK_SIZE + i] = d->colin_data[d->colin_get * UHARDDOOM_BLOCK_SIZE + i];
			uharddoom_colin_read(d);
			pos++;
			if (pos == 4) {
				d->col_state &= ~(UHARDDOOM_COL_STATE_LOAD_CMAP_A | UHARDDOOM_COL_STATE_CMAP_A_POS_MASK);
			} else {
				d->col_state &= ~UHARDDOOM_COL_STATE_CMAP_A_POS_MASK;
				d->col_state |= pos << UHARDDOOM_COL_STATE_CMAP_A_POS_SHIFT;
			}
			any = true;
		} else if (d->col_state & UHARDDOOM_COL_STATE_DRAW_LENGTH_MASK) {
			if (uharddoom_colout_full(d))
				return any;
			int xoff = (d->col_state & UHARDDOOM_COL_STATE_XOFF_MASK) >> UHARDDOOM_COL_STATE_XOFF_SHIFT;
			while (true) {
				if (d->col_cols_state[xoff] & UHARDDOOM_COL_COLS_STATE_COL_EN) {
					uint32_t get = (d->col_cols_state[xoff] & UHARDDOOM_COL_COLS_STATE_DATA_GET_MASK) >> UHARDDOOM_COL_COLS_STATE_DATA_GET_SHIFT;
					uint32_t put = (d->col_cols_state[xoff] & UHARDDOOM_COL_COLS_STATE_DATA_PUT_MASK) >> UHARDDOOM_COL_COLS_STATE_DATA_PUT_SHIFT;
					uint32_t cmap = (d->col_cols_state[xoff] & UHARDDOOM_COL_COLS_STATE_DATA_CMAP_MASK) >> UHARDDOOM_COL_COLS_STATE_DATA_CMAP_SHIFT;
					if (get == cmap) {
						if (cmap == put) {
							while (true) {
								uint32_t ui = d->col_cols_ustart[xoff] >> 16;
								if (d->col_cols_src_height[xoff])
									ui %= d->col_cols_src_height[xoff];
								uint32_t addr = d->col_cols_src_ptr[xoff] + d->col_cols_src_pitch[xoff] * ui;
								uint8_t *dst = &d->col_data[xoff * UHARDDOOM_COL_DATA_SIZE + put];
								bool spec = cmap != put;
								if (!uharddoom_cache_read(d, UHARDDOOM_CACHE_CLIENT_COL_SRC, addr, dst, spec)) {
									if (!spec) {
										d->enable &= ~UHARDDOOM_ENABLE_COL;
										return any;
									} else {
										break;
									}
								}
								if (d->col_state & UHARDDOOM_COL_STATE_CMAP_A_EN) {
									*dst = d->col_cmap_a[*dst];
								}
								d->col_cols_ustart[xoff] += d->col_cols_ustep[xoff];
								if (d->col_cols_src_height[xoff])
									d->col_cols_ustart[xoff] %= d->col_cols_src_height[xoff] << 16;
								put++;
								put %= UHARDDOOM_COL_DATA_SIZE;
								d->col_cols_state[xoff] &= ~UHARDDOOM_COL_COLS_STATE_DATA_PUT_MASK;
								d->col_cols_state[xoff] |= put << UHARDDOOM_COL_COLS_STATE_DATA_PUT_SHIFT;
								if ((put + 1) % UHARDDOOM_COL_DATA_SIZE == get)
									break;

							}
						}
						if (d->col_cols_state[xoff] & UHARDDOOM_COL_COLS_STATE_CMAP_B_EN) {
							while (true) {
								uint8_t *dst = &d->col_data[xoff * UHARDDOOM_COL_DATA_SIZE + cmap];
								uint32_t addr = d->col_cols_cmap_b_ptr[xoff] + *dst;
								bool spec = get != cmap;
								if (!uharddoom_cache_read(d, UHARDDOOM_CACHE_CLIENT_COL_CMAP_B, addr, dst, spec)) {
									if (!spec) {
										d->enable &= ~UHARDDOOM_ENABLE_COL;
										return any;
									} else {
										break;
									}
								}
								cmap++;
								cmap %= UHARDDOOM_COL_DATA_SIZE;
								d->col_cols_state[xoff] &= ~UHARDDOOM_COL_COLS_STATE_DATA_CMAP_MASK;
								d->col_cols_state[xoff] |= cmap << UHARDDOOM_COL_COLS_STATE_DATA_CMAP_SHIFT;
								if (cmap == put)
									break;
							}
						} else {
							cmap = put;
						}
						d->col_cols_state[xoff] &= ~UHARDDOOM_COL_COLS_STATE_DATA_CMAP_MASK;
						d->col_cols_state[xoff] |= cmap << UHARDDOOM_COL_COLS_STATE_DATA_CMAP_SHIFT;
					}
					uint8_t byte = d->col_data[xoff * UHARDDOOM_COL_DATA_SIZE + get];
					d->colout_data[d->colout_put * UHARDDOOM_BLOCK_SIZE + xoff] = byte;
					get++;
					get %= UHARDDOOM_COL_DATA_SIZE;
					d->col_cols_state[xoff] &= ~UHARDDOOM_COL_COLS_STATE_DATA_GET_MASK;
					d->col_cols_state[xoff] |= get << UHARDDOOM_COL_COLS_STATE_DATA_GET_SHIFT;
				}
				xoff++;
				any = true;
				d->col_state &= ~UHARDDOOM_COL_STATE_XOFF_MASK;
				if (xoff == UHARDDOOM_BLOCK_SIZE) {
					xoff = 0;
					uint64_t mask = 0;
					for (int i = 0; i < UHARDDOOM_BLOCK_SIZE; i++)
						if (d->col_cols_state[i] & UHARDDOOM_COL_COLS_STATE_COL_EN)
							mask |= 1ull << i;
					d->colout_mask[d->colout_put] = mask;
					uharddoom_colout_write(d);
					d->col_state--;
					break;
				}
				d->col_state |= xoff << UHARDDOOM_COL_STATE_XOFF_SHIFT;
			}
		} else {
			if (uharddoom_colcmd_empty(d))
				return any;
			uint32_t cmd = d->colcmd_cmd[d->colcmd_get];
			uint32_t data = d->colcmd_data[d->colcmd_get];
			uharddoom_colcmd_read(d);
			//printf("COLCMD %08x %08x\n", cmd, data);
			any = true;
			switch (cmd) {
				case UHARDDOOM_COLCMD_TYPE_COL_CMAP_B_PTR:
					d->col_stage_cmap_b_ptr = data;
					break;
				case UHARDDOOM_COLCMD_TYPE_COL_SRC_PTR:
					d->col_stage_src_ptr = data;
					break;
				case UHARDDOOM_COLCMD_TYPE_COL_SRC_PITCH:
					d->col_stage_src_pitch = data;
					break;
				case UHARDDOOM_COLCMD_TYPE_COL_USTART:
					d->col_stage_ustart = data;
					break;
				case UHARDDOOM_COLCMD_TYPE_COL_USTEP:
					d->col_stage_ustep = data;
					break;
				case UHARDDOOM_COLCMD_TYPE_COL_SETUP:
					{
					int which = UHARDDOOM_COLCMD_DATA_EXTR_COL_SETUP_X(data);
					d->col_cols_state[which] = 0;
					if (UHARDDOOM_COLCMD_DATA_EXTR_COL_SETUP_COL_EN(data))
						d->col_cols_state[which] |= UHARDDOOM_COL_COLS_STATE_COL_EN;
					if (UHARDDOOM_COLCMD_DATA_EXTR_COL_SETUP_CMAP_B_EN(data))
						d->col_cols_state[which] |= UHARDDOOM_COL_COLS_STATE_CMAP_B_EN;
					d->col_cols_src_height[which] = UHARDDOOM_COLCMD_DATA_EXTR_COL_SETUP_SRC_HEIGHT(data);
					d->col_cols_cmap_b_ptr[which] = d->col_stage_cmap_b_ptr;
					d->col_cols_src_ptr[which] = d->col_stage_src_ptr;
					d->col_cols_src_pitch[which] = d->col_stage_src_pitch;
					d->col_cols_ustart[which] = d->col_stage_ustart;
					d->col_cols_ustep[which] = d->col_stage_ustep;
					}
					break;
				case UHARDDOOM_COLCMD_TYPE_LOAD_CMAP_A:
					d->col_state |= UHARDDOOM_COL_STATE_LOAD_CMAP_A;
					d->col_state &= ~0xc0000000;
					break;
				case UHARDDOOM_COLCMD_TYPE_DRAW:
					d->col_state = data & (UHARDDOOM_COL_STATE_DRAW_LENGTH_MASK | UHARDDOOM_COL_STATE_CMAP_A_EN);
					break;
				case UHARDDOOM_COLCMD_TYPE_COLSEM:
					d->col_state |= UHARDDOOM_COL_STATE_COLSEM;
					break;
			}
		}
	}
}

/* Runs FX for some time.  Returns true if anything has been done.  */
static bool uharddoom_run_fx(UltimateHardDoomState *d) {
	if (!(d->enable & UHARDDOOM_ENABLE_FX))
		return false;
	bool any = false;
	while (true) {
		if (d->fx_state & UHARDDOOM_FX_STATE_LOAD_MODE_MASK) {
			if (uharddoom_fxin_empty(d))
				return any;
			any = true;
			uint32_t pos = (d->fx_state & UHARDDOOM_FX_STATE_LOAD_CNT_MASK) >> UHARDDOOM_FX_STATE_LOAD_CNT_SHIFT;
			d->fx_state &= ~UHARDDOOM_FX_STATE_LOAD_CNT_MASK;
			switch (d->fx_state & UHARDDOOM_FX_STATE_LOAD_MODE_MASK) {
				case UHARDDOOM_FX_STATE_LOAD_MODE_CMAP:
					memcpy(&d->fx_cmap[pos * UHARDDOOM_BLOCK_SIZE], &d->fxin_data[d->fxin_get * UHARDDOOM_BLOCK_SIZE], UHARDDOOM_BLOCK_SIZE);
					pos++;
					if (pos == 4)
						d->fx_state &= ~UHARDDOOM_FX_STATE_LOAD_MODE_MASK;
					else
						d->fx_state |= pos << UHARDDOOM_FX_STATE_LOAD_CNT_SHIFT;
					break;
				case UHARDDOOM_FX_STATE_LOAD_MODE_BLOCK:
					memcpy(&d->fx_buf[pos * UHARDDOOM_BLOCK_SIZE], &d->fxin_data[d->fxin_get * UHARDDOOM_BLOCK_SIZE], UHARDDOOM_BLOCK_SIZE);
					d->fx_state &= ~UHARDDOOM_FX_STATE_LOAD_MODE_MASK;
					break;
				case UHARDDOOM_FX_STATE_LOAD_MODE_FUZZ:
					memcpy(&d->fx_buf[pos * UHARDDOOM_BLOCK_SIZE], &d->fxin_data[d->fxin_get * UHARDDOOM_BLOCK_SIZE], UHARDDOOM_BLOCK_SIZE);
					pos++;
					if (pos >= 2)
						d->fx_state &= ~UHARDDOOM_FX_STATE_LOAD_MODE_MASK;
					else
						d->fx_state |= pos << UHARDDOOM_FX_STATE_LOAD_CNT_SHIFT;
					break;
			}
			uharddoom_fxin_read(d);
		} else if (d->fx_state & UHARDDOOM_FX_STATE_DRAW_LENGTH_MASK) {
			int pos[3] = {0};
			if (d->fx_state & UHARDDOOM_FX_STATE_DRAW_FUZZ_EN) {
				pos[0] = (d->fx_state & UHARDDOOM_FX_STATE_DRAW_FUZZ_POS_MASK) >> UHARDDOOM_FX_STATE_DRAW_FUZZ_POS_SHIFT;
				pos[1] = (pos[0] + 1) % 4;
				pos[2] = (pos[0] + 2) % 4;
			}
			if (!(d->fx_state & UHARDDOOM_FX_STATE_DRAW_FETCH_DONE)) {
				if (d->fx_state & UHARDDOOM_FX_STATE_DRAW_FETCH_SRD) {
					if (uharddoom_fxin_empty(d))
						return any;
					memcpy(&d->fx_buf[pos[2] * UHARDDOOM_BLOCK_SIZE], &d->fxin_data[d->fxin_get * UHARDDOOM_BLOCK_SIZE], UHARDDOOM_BLOCK_SIZE);
					uharddoom_fxin_read(d);
				} else if (d->fx_state & UHARDDOOM_FX_STATE_DRAW_FETCH_SPAN) {
					if (uharddoom_spanout_empty(d))
						return any;
					memcpy(&d->fx_buf[pos[2] * UHARDDOOM_BLOCK_SIZE], &d->spanout_data[d->spanout_get * UHARDDOOM_BLOCK_SIZE], UHARDDOOM_BLOCK_SIZE);
					uharddoom_spanout_read(d);
				}
				if (d->fx_state & UHARDDOOM_FX_STATE_DRAW_CMAP_EN)
					for (int i = 0; i < UHARDDOOM_BLOCK_SIZE; i++)
						d->fx_buf[i] = d->fx_cmap[d->fx_buf[i]];
				if (d->fx_state & UHARDDOOM_FX_STATE_DRAW_FUZZ_EN) {
					for (int i = 0; i < UHARDDOOM_BLOCK_SIZE; i++) {
						if (!(d->fx_col[i] & UHARDDOOM_FX_COL_ENABLE))
							continue;
						uint32_t fuzzpos = d->fx_col[i] & UHARDDOOM_FX_COL_FUZZPOS_MASK;
						bool m = 0x121e650de224aull >> fuzzpos & 1;
						fuzzpos++;
						fuzzpos %= 50;
						d->fx_col[i] = UHARDDOOM_FX_COL_ENABLE | fuzzpos;
						d->fx_buf[pos[1] * UHARDDOOM_BLOCK_SIZE + i] =
							d->fx_cmap[d->fx_buf[pos[m ? 0 : 2] * UHARDDOOM_BLOCK_SIZE + i]];
					}
				}
				d->fx_state |= UHARDDOOM_FX_STATE_DRAW_FETCH_DONE;
				any = true;
			}
			if (uharddoom_fxout_full(d))
				return any;
			memcpy(&d->fxout_data[d->fxout_put * UHARDDOOM_BLOCK_SIZE], &d->fx_buf[pos[1] * UHARDDOOM_BLOCK_SIZE], UHARDDOOM_BLOCK_SIZE);
			d->fx_state--;
			uint64_t mask = 0xffffffffffffffffull;
			if (d->fx_skip & UHARDDOOM_FX_SKIP_ALWAYS || !(d->fx_state & UHARDDOOM_FX_STATE_DRAW_NON_FIRST)) {
				mask &= 0xffffffffffffffffull << UHARDDOOM_FXCMD_DATA_EXTR_SKIP_BEGIN(d->fx_skip);
			}
			if (d->fx_skip & UHARDDOOM_FX_SKIP_ALWAYS || !(d->fx_state & UHARDDOOM_FX_STATE_DRAW_LENGTH_MASK)) {
				mask &= 0xffffffffffffffffull >> UHARDDOOM_FXCMD_DATA_EXTR_SKIP_END(d->fx_skip);
			}
			d->fxout_mask[d->fxout_put] = mask;
			uharddoom_fxout_write(d);
			if (d->fx_state & UHARDDOOM_FX_STATE_DRAW_FUZZ_EN) {
				pos[0]++;
				pos[0] %= 4;
				d->fx_state &= ~UHARDDOOM_FX_STATE_DRAW_FUZZ_POS_MASK;
				d->fx_state |= pos[0] << UHARDDOOM_FX_STATE_DRAW_FUZZ_POS_SHIFT;
			}
			d->fx_state |= UHARDDOOM_FX_STATE_DRAW_NON_FIRST;
			d->fx_state &= ~UHARDDOOM_FX_STATE_DRAW_FETCH_DONE;
			any = true;
		} else {
			if (uharddoom_fxcmd_empty(d))
				return any;
			uint32_t cmd = d->fxcmd_cmd[d->fxcmd_get];
			uint32_t data = d->fxcmd_data[d->fxcmd_get];
			uharddoom_fxcmd_read(d);
			//printf("FXCMD %08x %08x\n", cmd, data);
			any = true;
			switch (cmd) {
				case UHARDDOOM_FXCMD_TYPE_LOAD_CMAP:
					d->fx_state &= ~(UHARDDOOM_FX_STATE_LOAD_MODE_MASK | UHARDDOOM_FX_STATE_LOAD_CNT_MASK);
					d->fx_state |= UHARDDOOM_FX_STATE_LOAD_MODE_CMAP;
					break;
				case UHARDDOOM_FXCMD_TYPE_LOAD_BLOCK:
					d->fx_state &= ~(UHARDDOOM_FX_STATE_LOAD_MODE_MASK | UHARDDOOM_FX_STATE_LOAD_CNT_MASK);
					d->fx_state |= UHARDDOOM_FX_STATE_LOAD_MODE_BLOCK;
					break;
				case UHARDDOOM_FXCMD_TYPE_LOAD_FUZZ:
					d->fx_state &= ~(UHARDDOOM_FX_STATE_LOAD_MODE_MASK | UHARDDOOM_FX_STATE_LOAD_CNT_MASK | UHARDDOOM_FX_STATE_DRAW_FUZZ_POS_MASK);
					d->fx_state |= UHARDDOOM_FX_STATE_LOAD_MODE_FUZZ;
					break;
				case UHARDDOOM_FXCMD_TYPE_FILL_COLOR:
					memset(d->fx_buf, (uint8_t)data, UHARDDOOM_FX_BUF_SIZE);
					break;
				case UHARDDOOM_FXCMD_TYPE_COL_SETUP:
					d->fx_col[UHARDDOOM_FXCMD_DATA_EXTR_COL_SETUP_X(data)] =
						UHARDDOOM_FXCMD_DATA_EXTR_COL_SETUP_FUZZPOS(data) |
						(UHARDDOOM_FXCMD_DATA_EXTR_COL_SETUP_ENABLE(data) ? UHARDDOOM_FX_COL_ENABLE : 0);
					break;
				case UHARDDOOM_FXCMD_TYPE_SKIP:
					d->fx_skip = data & UHARDDOOM_FX_SKIP_MASK;
					break;
				case UHARDDOOM_FXCMD_TYPE_DRAW:
					if (data & UHARDDOOM_FX_STATE_DRAW_FUZZ_EN)
						d->fx_state &= UHARDDOOM_FX_STATE_DRAW_FUZZ_POS_MASK;
					else
						d->fx_state = 0;
					d->fx_state |= data & (UHARDDOOM_FX_STATE_DRAW_LENGTH_MASK | UHARDDOOM_FX_STATE_DRAW_CMAP_EN | UHARDDOOM_FX_STATE_DRAW_FUZZ_EN | UHARDDOOM_FX_STATE_DRAW_FETCH_SRD | UHARDDOOM_FX_STATE_DRAW_FETCH_SPAN);
					break;
			}
		}
	}
}

/* Runs SWR for some time.  Returns true if anything has been done.  */
static bool uharddoom_run_swr(UltimateHardDoomState *d) {
	if (!(d->enable & UHARDDOOM_ENABLE_SWR))
		return false;
	bool any = false;
	while (true) {
		if (d->swr_state & UHARDDOOM_SWR_STATE_SRDSEM) {
			if (d->srdsem)
				return any;
			d->srdsem = 1;
			d->swr_state &= ~UHARDDOOM_SWR_STATE_SRDSEM;
			any = true;
		} else if (d->swr_state & UHARDDOOM_SWR_STATE_COLSEM) {
			if (d->colsem)
				return any;
			d->colsem = 1;
			d->swr_state &= ~UHARDDOOM_SWR_STATE_COLSEM;
			any = true;
		} else if (d->swr_state & UHARDDOOM_SWR_STATE_SPANSEM) {
			if (d->spansem)
				return any;
			d->spansem = 1;
			d->swr_state &= ~UHARDDOOM_SWR_STATE_SPANSEM;
			any = true;
		} else if (d->swr_state & UHARDDOOM_SWR_STATE_DRAW_LENGTH_MASK) {
			if (!(d->swr_state & UHARDDOOM_SWR_STATE_SRC_BUF_FULL)) {
				if (d->swr_state & UHARDDOOM_SWR_STATE_COL_EN) {
					if (uharddoom_colout_empty(d))
						return any;
					memcpy(d->swr_src_buf, &d->colout_data[d->colout_get * UHARDDOOM_BLOCK_SIZE], UHARDDOOM_BLOCK_SIZE);
					d->swr_block_mask = d->colout_mask[d->colout_get];
					uharddoom_colout_read(d);
				} else {
					if (uharddoom_fxout_empty(d))
						return any;
					memcpy(d->swr_src_buf, &d->fxout_data[d->fxout_get * UHARDDOOM_BLOCK_SIZE], UHARDDOOM_BLOCK_SIZE);
					d->swr_block_mask = d->fxout_mask[d->fxout_get];
					uharddoom_fxout_read(d);
				}
				d->swr_state |= UHARDDOOM_SWR_STATE_SRC_BUF_FULL;
				any = true;
			}
			if (!(d->swr_state & UHARDDOOM_SWR_STATE_DST_BUF_FULL)) {
				if (d->swr_block_mask != 0xffffffffffffffffull || d->swr_state & UHARDDOOM_SWR_STATE_TRANS_EN) {
					uint64_t pa;
					if (!uharddoom_translate_addr(d, UHARDDOOM_TLB_CLIENT_SWR_DST, d->swr_dst_ptr, &pa, false)) {
						d->enable &= ~UHARDDOOM_ENABLE_SWR;
						return any;
					}
					pci_dma_read(&d->dev, pa, &d->swr_dst_buf, UHARDDOOM_BLOCK_SIZE);
				}
				d->swr_state |= UHARDDOOM_SWR_STATE_DST_BUF_FULL;
				any = true;
			}
			int pos = (d->swr_state & UHARDDOOM_SWR_STATE_TRANS_POS_MASK) >> UHARDDOOM_SWR_STATE_TRANS_POS_SHIFT;
			while (pos < UHARDDOOM_BLOCK_SIZE) {
				if (d->swr_block_mask & 1ull << pos) {
					if (d->swr_state & UHARDDOOM_SWR_STATE_TRANS_EN) {
						uint32_t idx = d->swr_dst_buf[pos] << 8 | d->swr_src_buf[pos];
						uint32_t addr = idx + d->swr_transmap_ptr;
						if (!uharddoom_cache_read(d, UHARDDOOM_CACHE_CLIENT_SWR_TRANSMAP, addr, &d->swr_trans_buf[pos], false)) {
							d->enable &= ~UHARDDOOM_ENABLE_SWR;
							return any;
						}
					} else {
						d->swr_trans_buf[pos] = d->swr_src_buf[pos];
					}
				} else {
					d->swr_trans_buf[pos] = d->swr_dst_buf[pos];
				}
				pos++;
				d->swr_state &= ~UHARDDOOM_SWR_STATE_TRANS_POS_MASK;
				d->swr_state |= pos << UHARDDOOM_SWR_STATE_TRANS_POS_SHIFT;
				any = true;
			}
			uint64_t pa;
			if (!uharddoom_translate_addr(d, UHARDDOOM_TLB_CLIENT_SWR_DST, d->swr_dst_ptr, &pa, true)) {
				d->enable &= ~UHARDDOOM_ENABLE_SWR;
				return any;
			}
			pci_dma_write(&d->dev, pa, d->swr_trans_buf, UHARDDOOM_BLOCK_SIZE);
			d->swr_state--;
			d->swr_state &= ~(UHARDDOOM_SWR_STATE_SRC_BUF_FULL | UHARDDOOM_SWR_STATE_DST_BUF_FULL | UHARDDOOM_SWR_STATE_TRANS_POS_MASK);
			d->swr_dst_ptr += d->swr_dst_pitch;
			any = true;
		} else {
			if (uharddoom_swrcmd_empty(d))
				return any;
			uint32_t cmd = d->swrcmd_cmd[d->swrcmd_get];
			uint32_t data = d->swrcmd_data[d->swrcmd_get];
			uharddoom_swrcmd_read(d);
			// printf("SWRCMD %08x %08x\n", cmd, data);
			any = true;
			switch (cmd) {
				case UHARDDOOM_SWRCMD_TYPE_TRANSMAP_PTR:
					d->swr_transmap_ptr = data;
					break;
				case UHARDDOOM_SWRCMD_TYPE_DST_PTR:
					d->swr_dst_ptr = data & UHARDDOOM_BLOCK_PTR_MASK;
					break;
				case UHARDDOOM_SWRCMD_TYPE_DST_PITCH:
					d->swr_dst_pitch = data & UHARDDOOM_BLOCK_PITCH_MASK;
					break;
				case UHARDDOOM_SWRCMD_TYPE_DRAW:
					d->swr_state = data & (UHARDDOOM_SWR_STATE_DRAW_LENGTH_MASK | UHARDDOOM_SWR_STATE_COL_EN | UHARDDOOM_SWR_STATE_TRANS_EN);
					break;
				case UHARDDOOM_SWRCMD_TYPE_SRDSEM:
					d->swr_state |= UHARDDOOM_SWR_STATE_SRDSEM;
					break;
				case UHARDDOOM_SWRCMD_TYPE_COLSEM:
					d->swr_state |= UHARDDOOM_SWR_STATE_COLSEM;
					break;
				case UHARDDOOM_SWRCMD_TYPE_SPANSEM:
					d->swr_state |= UHARDDOOM_SWR_STATE_SPANSEM;
					break;
			}
		}
	}
}

static void *uharddoom_thread(void *opaque)
{
	UltimateHardDoomState *d = opaque;
	qemu_mutex_lock(&d->mutex);
	while (1) {
		if (d->stopping)
			break;
		qemu_mutex_unlock(&d->mutex);
		qemu_mutex_lock_iothread();
		qemu_mutex_lock(&d->mutex);
		bool idle = true;
		if (uharddoom_run_batch(d))
			idle = false;
		if (uharddoom_run_cmd(d))
			idle = false;
		if (uharddoom_run_fe(d))
			idle = false;
		if (uharddoom_run_srd(d))
			idle = false;
		if (uharddoom_run_span(d))
			idle = false;
		if (uharddoom_run_col(d))
			idle = false;
		if (uharddoom_run_fx(d))
			idle = false;
		if (uharddoom_run_swr(d))
			idle = false;
		uharddoom_status_update(d);
		qemu_mutex_unlock_iothread();
		if (idle) {
			qemu_cond_wait(&d->cond, &d->mutex);
		}
	}
	qemu_mutex_unlock(&d->mutex);
	return 0;
}

static void uharddoom_realize(PCIDevice *pci_dev, Error **errp)
{
	UltimateHardDoomState *d = UHARDDOOM_DEV(pci_dev);
	uint8_t *pci_conf = d->dev.config;

	pci_config_set_interrupt_pin(pci_conf, 1);

	memory_region_init_io(&d->mmio, OBJECT(d), &uharddoom_mmio_ops, d, "uharddoom", UHARDDOOM_BAR_SIZE);
	pci_register_bar(&d->dev, 0, PCI_BASE_ADDRESS_SPACE_MEMORY, &d->mmio);

	qemu_mutex_init(&d->mutex);
	qemu_cond_init(&d->cond);

	uharddoom_power_reset(&pci_dev->qdev);

	d->stopping = false;
	qemu_thread_create(&d->thread, "uharddoom", uharddoom_thread,
			d, QEMU_THREAD_JOINABLE);
}

static void uharddoom_exit(PCIDevice *pci_dev)
{
	UltimateHardDoomState *d = UHARDDOOM_DEV(pci_dev);
	qemu_mutex_lock(&d->mutex);
	d->stopping = true;
	qemu_mutex_unlock(&d->mutex);
	qemu_cond_signal(&d->cond);
	qemu_thread_join(&d->thread);
	qemu_cond_destroy(&d->cond);
	qemu_mutex_destroy(&d->mutex);
}

static void uharddoom_class_init(ObjectClass *klass, void *data)
{
	DeviceClass *dc = DEVICE_CLASS(klass);
	PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);

	k->realize = uharddoom_realize;
	k->exit = uharddoom_exit;
	k->vendor_id = UHARDDOOM_VENDOR_ID;
	k->device_id = UHARDDOOM_DEVICE_ID;
	k->class_id = PCI_CLASS_PROCESSOR_CO;
	dc->reset = uharddoom_power_reset;
}

static InterfaceInfo uharddoom_interfaces[] = {
	{ INTERFACE_CONVENTIONAL_PCI_DEVICE },
	{ },
};

static TypeInfo uharddoom_info = {
	.name          = TYPE_UHARDDOOM,
	.parent        = TYPE_PCI_DEVICE,
	.instance_size = sizeof(UltimateHardDoomState),
	.class_init    = uharddoom_class_init,
	.interfaces    = uharddoom_interfaces,
};

static void uharddoom_register_types(void)
{
	type_register_static(&uharddoom_info);
}

type_init(uharddoom_register_types)
