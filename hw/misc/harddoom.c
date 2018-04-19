/*
 * HardDoom™ device
 *
 * Copyright (C) 2013-2018 Marcin Kościelnicki
 *
 * This work is licensed under the terms of the GNU GPL, version 2.  See
 * the COPYING file in the top-level directory.
 */

#include "harddoom.h"
#include "qemu/osdep.h"
#include "qemu/main-loop.h"
#include "hw/hw.h"
#include "hw/pci/pci.h"

typedef struct {
	PCIDevice dev;
	MemoryRegion mmio;
	QemuThread thread;
	QemuMutex mutex;
	QemuCond cond;
	bool stopping;
	/* These correspond directly to the registers listed in harddoom.h.  */
	uint32_t enable;
	uint32_t status;
	uint32_t intr;
	uint32_t intr_enable;
	uint32_t fence_last;
	uint32_t fence_wait;
	uint32_t cmd_read_ptr;
	uint32_t cmd_write_ptr;
	uint32_t fe_code_addr;
	uint32_t fe_code[HARDDOOM_FE_CODE_SIZE];
	uint32_t fe_error_code;
	uint32_t fe_error_cmd;
	uint32_t fe_state;
	uint32_t fifo_state;
	uint32_t fifo_data[HARDDOOM_FIFO_SIZE];
	uint32_t fe_data_addr;
	uint32_t fe_data[HARDDOOM_FE_DATA_SIZE];
	uint32_t fe2xy_state;
	uint32_t fe2xy_data[HARDDOOM_FE2XY_SIZE];
	uint32_t fe2tex_state;
	uint32_t fe2tex_data[HARDDOOM_FE2TEX_SIZE];
	uint32_t fe2flat_state;
	uint32_t fe2flat_data[HARDDOOM_FE2FLAT_SIZE];
	uint32_t fe2fuzz_state;
	uint32_t fe2fuzz_data[HARDDOOM_FE2FUZZ_SIZE];
	uint32_t fe2og_state;
	uint32_t fe2og_data[HARDDOOM_FE2OG_SIZE];
	uint32_t fe_reg[HARDDOOM_FE_REG_NUM];
	uint32_t stats[HARDDOOM_STATS_NUM];
	uint32_t xy_surf_dims;
	uint32_t xy_cmd;
	uint32_t xy_dst_cmd;
	uint32_t xy_src_cmd;
	uint32_t xy2sw_state;
	uint32_t xy2sw_data[HARDDOOM_XY2SW_SIZE];
	uint32_t xy2sr_state;
	uint32_t xy2sr_data[HARDDOOM_XY2SR_SIZE];
	uint32_t sr2og_state;
	uint8_t sr2og_data[HARDDOOM_SR2OG_SIZE * HARDDOOM_BLOCK_SIZE];
	uint32_t tex_dims;
	uint32_t tex_ustart;
	uint32_t tex_ustep;
	uint32_t tex_cmd;
	uint32_t tex_cache_state;
	uint64_t tex_mask;
	uint8_t tex_cache[HARDDOOM_TEX_CACHE_SIZE];
	uint32_t tex_column_state[HARDDOOM_BLOCK_SIZE];
	uint32_t tex_column_step[HARDDOOM_BLOCK_SIZE];
	uint32_t tex_column_offset[HARDDOOM_BLOCK_SIZE];
	uint8_t tex_column_spec_data[HARDDOOM_BLOCK_SIZE * HARDDOOM_TEX_COLUMN_SPEC_DATA_SIZE];
	uint32_t tex2og_state;
	uint8_t tex2og_data[HARDDOOM_TEX2OG_SIZE * HARDDOOM_BLOCK_SIZE];
	uint64_t tex2og_mask[HARDDOOM_TEX2OG_SIZE];
	uint32_t flat_ucoord;
	uint32_t flat_vcoord;
	uint32_t flat_ustep;
	uint32_t flat_vstep;
	uint32_t flat_addr;
	uint32_t flat_cmd;
	uint32_t flat_cache_state;
	uint8_t flat_cache[HARDDOOM_FLAT_CACHE_SIZE];
	uint32_t flat2og_state;
	uint8_t flat2og_data[HARDDOOM_FLAT2OG_SIZE * HARDDOOM_BLOCK_SIZE];
	uint32_t fuzz_cmd;
	uint8_t fuzz_position[HARDDOOM_BLOCK_SIZE];
	uint32_t fuzz2og_state;
	uint64_t fuzz2og_mask[HARDDOOM_FUZZ2OG_SIZE];
	uint32_t og_cmd;
	uint32_t og_misc;
	uint64_t og_mask;
	uint64_t og_fuzz_mask;
	uint8_t og_buf[HARDDOOM_OG_BUF_SIZE];
	uint8_t og_colormap[HARDDOOM_COLORMAP_SIZE];
	uint8_t og_translation[HARDDOOM_COLORMAP_SIZE];
	uint32_t og2sw_state;
	uint8_t og2sw_data[HARDDOOM_OG2SW_SIZE * HARDDOOM_BLOCK_SIZE];
	uint64_t og2sw_mask[HARDDOOM_OG2SW_SIZE];
	uint32_t og2sw_c_state;
	uint32_t og2sw_c_data[HARDDOOM_OG2SW_C_SIZE];
	uint32_t sw_cmd;
	uint32_t sw2xy_state;
	uint32_t tlb_pt[3];
	uint32_t tlb_entry[3];
	uint32_t tlb_vaddr[3];
} HardDoomState;

static const VMStateDescription vmstate_harddoom = {
	.name = "harddoom",
	.version_id = 3,
	.minimum_version_id = 3,
	.minimum_version_id_old = 3,
	.fields = (VMStateField[]) {
		VMSTATE_PCI_DEVICE(dev, HardDoomState),
		VMSTATE_UINT32(enable, HardDoomState),
		VMSTATE_UINT32(status, HardDoomState),
		VMSTATE_UINT32(intr, HardDoomState),
		VMSTATE_UINT32(intr_enable, HardDoomState),
		VMSTATE_UINT32(fence_last, HardDoomState),
		VMSTATE_UINT32(fence_wait, HardDoomState),
		VMSTATE_UINT32(cmd_read_ptr, HardDoomState),
		VMSTATE_UINT32(cmd_write_ptr, HardDoomState),
		VMSTATE_UINT32(fe_code_addr, HardDoomState),
		VMSTATE_UINT32_ARRAY(fe_code, HardDoomState, HARDDOOM_FE_CODE_SIZE),
		VMSTATE_UINT32(fe_error_code, HardDoomState),
		VMSTATE_UINT32(fe_error_cmd, HardDoomState),
		VMSTATE_UINT32(fe_state, HardDoomState),
		VMSTATE_UINT32(fifo_state, HardDoomState),
		VMSTATE_UINT32_ARRAY(fifo_data, HardDoomState, HARDDOOM_FIFO_SIZE),
		VMSTATE_UINT32(fe_data_addr, HardDoomState),
		VMSTATE_UINT32_ARRAY(fe_data, HardDoomState, HARDDOOM_FE_DATA_SIZE),
		VMSTATE_UINT32(fe2xy_state, HardDoomState),
		VMSTATE_UINT32_ARRAY(fe2xy_data, HardDoomState, HARDDOOM_FE2XY_SIZE),
		VMSTATE_UINT32(fe2tex_state, HardDoomState),
		VMSTATE_UINT32_ARRAY(fe2tex_data, HardDoomState, HARDDOOM_FE2TEX_SIZE),
		VMSTATE_UINT32(fe2flat_state, HardDoomState),
		VMSTATE_UINT32_ARRAY(fe2flat_data, HardDoomState, HARDDOOM_FE2FLAT_SIZE),
		VMSTATE_UINT32(fe2fuzz_state, HardDoomState),
		VMSTATE_UINT32_ARRAY(fe2fuzz_data, HardDoomState, HARDDOOM_FE2FUZZ_SIZE),
		VMSTATE_UINT32(fe2og_state, HardDoomState),
		VMSTATE_UINT32_ARRAY(fe2og_data, HardDoomState, HARDDOOM_FE2OG_SIZE),
		VMSTATE_UINT32_ARRAY(fe_reg, HardDoomState, HARDDOOM_FE_REG_NUM),
		VMSTATE_UINT32_ARRAY(stats, HardDoomState, HARDDOOM_STATS_NUM),
		VMSTATE_UINT32(xy_surf_dims, HardDoomState),
		VMSTATE_UINT32(xy_cmd, HardDoomState),
		VMSTATE_UINT32(xy_dst_cmd, HardDoomState),
		VMSTATE_UINT32(xy_src_cmd, HardDoomState),
		VMSTATE_UINT32(xy2sw_state, HardDoomState),
		VMSTATE_UINT32_ARRAY(xy2sw_data, HardDoomState, HARDDOOM_XY2SW_SIZE),
		VMSTATE_UINT32(xy2sr_state, HardDoomState),
		VMSTATE_UINT32_ARRAY(xy2sr_data, HardDoomState, HARDDOOM_XY2SR_SIZE),
		VMSTATE_UINT32(sr2og_state, HardDoomState),
		VMSTATE_UINT8_ARRAY(sr2og_data, HardDoomState, HARDDOOM_SR2OG_SIZE * HARDDOOM_BLOCK_SIZE),
		VMSTATE_UINT32(tex_dims, HardDoomState),
		VMSTATE_UINT32(tex_ustart, HardDoomState),
		VMSTATE_UINT32(tex_ustep, HardDoomState),
		VMSTATE_UINT32(tex_cmd, HardDoomState),
		VMSTATE_UINT32(tex_cache_state, HardDoomState),
		VMSTATE_UINT64(tex_mask, HardDoomState),
		VMSTATE_UINT8_ARRAY(tex_cache, HardDoomState, HARDDOOM_TEX_CACHE_SIZE),
		VMSTATE_UINT32_ARRAY(tex_column_state, HardDoomState, HARDDOOM_BLOCK_SIZE),
		VMSTATE_UINT32_ARRAY(tex_column_step, HardDoomState, HARDDOOM_BLOCK_SIZE),
		VMSTATE_UINT32_ARRAY(tex_column_offset, HardDoomState, HARDDOOM_BLOCK_SIZE),
		VMSTATE_UINT8_ARRAY(tex_column_spec_data, HardDoomState, HARDDOOM_BLOCK_SIZE * HARDDOOM_TEX_COLUMN_SPEC_DATA_SIZE),
		VMSTATE_UINT32(tex2og_state, HardDoomState),
		VMSTATE_UINT8_ARRAY(tex2og_data, HardDoomState, HARDDOOM_TEX2OG_SIZE * HARDDOOM_BLOCK_SIZE),
		VMSTATE_UINT64_ARRAY(tex2og_mask, HardDoomState, HARDDOOM_TEX2OG_SIZE),
		VMSTATE_UINT32(flat_ucoord, HardDoomState),
		VMSTATE_UINT32(flat_vcoord, HardDoomState),
		VMSTATE_UINT32(flat_ustep, HardDoomState),
		VMSTATE_UINT32(flat_vstep, HardDoomState),
		VMSTATE_UINT32(flat_addr, HardDoomState),
		VMSTATE_UINT32(flat_cmd, HardDoomState),
		VMSTATE_UINT32(flat_cache_state, HardDoomState),
		VMSTATE_UINT8_ARRAY(flat_cache, HardDoomState, HARDDOOM_FLAT_CACHE_SIZE),
		VMSTATE_UINT32(flat2og_state, HardDoomState),
		VMSTATE_UINT8_ARRAY(flat2og_data, HardDoomState, HARDDOOM_FLAT2OG_SIZE * HARDDOOM_BLOCK_SIZE),
		VMSTATE_UINT32(fuzz_cmd, HardDoomState),
		VMSTATE_UINT8_ARRAY(fuzz_position, HardDoomState, HARDDOOM_BLOCK_SIZE),
		VMSTATE_UINT32(fuzz2og_state, HardDoomState),
		VMSTATE_UINT64_ARRAY(fuzz2og_mask, HardDoomState, HARDDOOM_FUZZ2OG_SIZE),
		VMSTATE_UINT32(og_cmd, HardDoomState),
		VMSTATE_UINT32(og_misc, HardDoomState),
		VMSTATE_UINT64(og_mask, HardDoomState),
		VMSTATE_UINT64(og_fuzz_mask, HardDoomState),
		VMSTATE_UINT8_ARRAY(og_buf, HardDoomState, HARDDOOM_OG_BUF_SIZE),
		VMSTATE_UINT8_ARRAY(og_colormap, HardDoomState, HARDDOOM_COLORMAP_SIZE),
		VMSTATE_UINT8_ARRAY(og_translation, HardDoomState, HARDDOOM_COLORMAP_SIZE),
		VMSTATE_UINT32(og2sw_state, HardDoomState),
		VMSTATE_UINT8_ARRAY(og2sw_data, HardDoomState, HARDDOOM_OG2SW_SIZE * HARDDOOM_BLOCK_SIZE),
		VMSTATE_UINT64_ARRAY(og2sw_mask, HardDoomState, HARDDOOM_OG2SW_SIZE),
		VMSTATE_UINT32(og2sw_c_state, HardDoomState),
		VMSTATE_UINT32_ARRAY(og2sw_c_data, HardDoomState, HARDDOOM_OG2SW_C_SIZE),
		VMSTATE_UINT32(sw_cmd, HardDoomState),
		VMSTATE_UINT32(sw2xy_state, HardDoomState),
		VMSTATE_UINT32_ARRAY(tlb_pt, HardDoomState, 3),
		VMSTATE_UINT32_ARRAY(tlb_entry, HardDoomState, 3),
		VMSTATE_UINT32_ARRAY(tlb_vaddr, HardDoomState, 3),
		VMSTATE_END_OF_LIST()
	}
};

enum {
	TLB_SURF_DST = 0,
	TLB_SURF_SRC = 1,
	TLB_TEXTURE = 2,
};

#define TLB(u, n) {HARDDOOM_ENABLE_ ## u, HARDDOOM_STAT_TLB_REBIND_ ## n, HARDDOOM_STAT_TLB_ ## n ## _HIT, HARDDOOM_STAT_TLB_ ## n ##_MISS}
struct harddoom_tlb {
	uint32_t enable;
	int rebind_stats_idx;
	int hit_stats_idx;
	int miss_stats_idx;
} harddoom_tlbs[] = {
	[TLB_SURF_DST] = TLB(XY, SURF_DST),
	[TLB_SURF_SRC] = TLB(XY, SURF_SRC),
	[TLB_TEXTURE] = TLB(TEX, TEXTURE),
};

enum {
	REG_TYPE_SIMPLE_32,
	REG_TYPE_WINDOW_32,
	REG_TYPE_SIMPLE_8,
	REG_TYPE_MASK,
};

#define REG(a, b, c) {a, offsetof(HardDoomState, b), 0, c, 1, REG_TYPE_SIMPLE_32, #a}
#define ARRAY(a, b, c, n) {a, offsetof(HardDoomState, b), 0, c, n, REG_TYPE_SIMPLE_32, #a}
#define BYTE_ARRAY(a, b, c, n) {a, offsetof(HardDoomState, b), 0, c, n, REG_TYPE_SIMPLE_8, #a}
#define WINDOW(a, b, i, c, n) {a, offsetof(HardDoomState, b), offsetof(HardDoomState, i), c, n, REG_TYPE_WINDOW_32, #a}
#define MASK(a, b) {a, offsetof(HardDoomState, b), 0, 0, 1, REG_TYPE_MASK, #a}
static struct harddoom_register {
	uint32_t bar_off;
	ptrdiff_t vm_off;
	ptrdiff_t vm_off_idx;
	uint32_t mask;
	int num;
	int type;
	const char *name;
} harddoom_registers[] = {
	REG(HARDDOOM_ENABLE, enable, HARDDOOM_ENABLE_ALL),
	REG(HARDDOOM_INTR_ENABLE, intr_enable, HARDDOOM_INTR_MASK),
	REG(HARDDOOM_FENCE_LAST, fence_last, HARDDOOM_FENCE_MASK),
	REG(HARDDOOM_FENCE_WAIT, fence_wait, HARDDOOM_FENCE_MASK),
	REG(HARDDOOM_CMD_READ_PTR, cmd_read_ptr, ~3),
	REG(HARDDOOM_CMD_WRITE_PTR, cmd_write_ptr, ~3),
	REG(HARDDOOM_FE_CODE_ADDR, fe_code_addr, HARDDOOM_FE_CODE_SIZE - 1),
	WINDOW(HARDDOOM_FE_CODE_WINDOW, fe_code, fe_code_addr, 0x3fffffff, HARDDOOM_FE_CODE_SIZE),
	REG(HARDDOOM_FE_ERROR_CODE, fe_error_code, HARDDOOM_FE_ERROR_CODE_MASK),
	REG(HARDDOOM_FE_ERROR_CMD, fe_error_cmd, ~0),
	REG(HARDDOOM_FE_STATE, fe_state, HARDDOOM_FE_STATE_MASK),
	REG(HARDDOOM_FE_DATA_ADDR, fe_data_addr, HARDDOOM_FE_DATA_SIZE - 1),
	WINDOW(HARDDOOM_FE_DATA_WINDOW, fe_data, fe_data_addr, ~0, HARDDOOM_FE_DATA_SIZE),
	ARRAY(HARDDOOM_FE_REG(0), fe_reg, ~0, HARDDOOM_FE_REG_NUM),
	ARRAY(HARDDOOM_STATS(0), stats, ~0, HARDDOOM_STATS_NUM),
	REG(HARDDOOM_XY_SURF_DIMS, xy_surf_dims, HARDDOOM_XY_SURF_DIMS_MASK),
	REG(HARDDOOM_XY_CMD, xy_cmd, ~0),
	REG(HARDDOOM_XY_DST_CMD, xy_dst_cmd, ~0),
	REG(HARDDOOM_XY_SRC_CMD, xy_src_cmd, ~0),
	REG(HARDDOOM_TLB_PT_SURF_DST, tlb_pt[TLB_SURF_DST], HARDDOOM_TLB_PT_MASK),
	REG(HARDDOOM_TLB_PT_SURF_SRC, tlb_pt[TLB_SURF_SRC], HARDDOOM_TLB_PT_MASK),
	REG(HARDDOOM_TLB_PT_TEXTURE, tlb_pt[TLB_TEXTURE], HARDDOOM_TLB_PT_MASK),
	REG(HARDDOOM_TLB_ENTRY_SURF_DST, tlb_entry[TLB_SURF_DST], HARDDOOM_TLB_ENTRY_MASK),
	REG(HARDDOOM_TLB_ENTRY_SURF_SRC, tlb_entry[TLB_SURF_SRC], HARDDOOM_TLB_ENTRY_MASK),
	REG(HARDDOOM_TLB_ENTRY_TEXTURE, tlb_entry[TLB_TEXTURE], HARDDOOM_TLB_ENTRY_MASK),
	REG(HARDDOOM_TLB_VADDR_SURF_DST, tlb_vaddr[TLB_SURF_DST], HARDDOOM_TLB_VADDR_MASK),
	REG(HARDDOOM_TLB_VADDR_SURF_SRC, tlb_vaddr[TLB_SURF_SRC], HARDDOOM_TLB_VADDR_MASK),
	REG(HARDDOOM_TLB_VADDR_TEXTURE, tlb_vaddr[TLB_TEXTURE], HARDDOOM_TLB_VADDR_MASK),
	REG(HARDDOOM_TEX_DIMS, tex_dims, HARDDOOM_TEX_DIMS_MASK),
	REG(HARDDOOM_TEX_USTART, tex_ustart, HARDDOOM_TEX_COORD_MASK),
	REG(HARDDOOM_TEX_USTEP, tex_ustep, HARDDOOM_TEX_COORD_MASK),
	REG(HARDDOOM_TEX_CMD, tex_cmd, ~0),
	REG(HARDDOOM_TEX_CACHE_STATE, tex_cache_state, HARDDOOM_TEX_CACHE_STATE_MASK),
	MASK(HARDDOOM_TEX_MASK, tex_mask),
	BYTE_ARRAY(HARDDOOM_TEX_CACHE, tex_cache, 0xff, HARDDOOM_TEX_CACHE_SIZE),
	ARRAY(HARDDOOM_TEX_COLUMN_STATE(0), tex_column_state, HARDDOOM_TEX_COLUMN_STATE_MASK, HARDDOOM_BLOCK_SIZE),
	ARRAY(HARDDOOM_TEX_COLUMN_STEP(0), tex_column_step, HARDDOOM_TEX_COORD_MASK, HARDDOOM_BLOCK_SIZE),
	ARRAY(HARDDOOM_TEX_COLUMN_OFFSET(0), tex_column_offset, HARDDOOM_TEX_OFFSET_MASK, HARDDOOM_BLOCK_SIZE),
	BYTE_ARRAY(HARDDOOM_TEX_COLUMN_SPEC_DATA(0), tex_column_spec_data, 0xff, HARDDOOM_BLOCK_SIZE * HARDDOOM_TEX_COLUMN_SPEC_DATA_SIZE),
	REG(HARDDOOM_FLAT_UCOORD, flat_ucoord, HARDDOOM_FLAT_COORD_MASK),
	REG(HARDDOOM_FLAT_VCOORD, flat_vcoord, HARDDOOM_FLAT_COORD_MASK),
	REG(HARDDOOM_FLAT_USTEP, flat_ustep, HARDDOOM_FLAT_COORD_MASK),
	REG(HARDDOOM_FLAT_VSTEP, flat_vstep, HARDDOOM_FLAT_COORD_MASK),
	REG(HARDDOOM_FLAT_ADDR, flat_addr, HARDDOOM_FLAT_ADDR_MASK),
	REG(HARDDOOM_FLAT_CMD, flat_cmd, ~0),
	REG(HARDDOOM_FLAT_CACHE_STATE, flat_cache_state, HARDDOOM_FLAT_CACHE_STATE_MASK),
	BYTE_ARRAY(HARDDOOM_FLAT_CACHE, flat_cache, 0xff, HARDDOOM_FLAT_CACHE_SIZE),
	REG(HARDDOOM_FUZZ_CMD, fuzz_cmd, ~0),
	BYTE_ARRAY(HARDDOOM_FUZZ_POSITION, fuzz_position, 0x3f, HARDDOOM_BLOCK_SIZE),
	REG(HARDDOOM_OG_CMD, og_cmd, ~0),
	REG(HARDDOOM_OG_MISC, og_misc, HARDDOOM_OG_MISC_MASK),
	MASK(HARDDOOM_OG_MASK, og_mask),
	MASK(HARDDOOM_OG_FUZZ_MASK, og_fuzz_mask),
	BYTE_ARRAY(HARDDOOM_OG_BUF, og_buf, 0xff, HARDDOOM_OG_BUF_SIZE),
	BYTE_ARRAY(HARDDOOM_OG_COLORMAP, og_colormap, 0xff, HARDDOOM_COLORMAP_SIZE),
	BYTE_ARRAY(HARDDOOM_OG_TRANSLATION, og_translation, 0xff, HARDDOOM_COLORMAP_SIZE),
	REG(HARDDOOM_SW_CMD, sw_cmd, ~0),
	REG(HARDDOOM_SW2XY_STATE, sw2xy_state, HARDDOOM_SW2XY_STATE_MASK),
};

enum {
	FIFO_MAIN,
	FIFO_FE2XY,
	FIFO_FE2TEX,
	FIFO_FE2FLAT,
	FIFO_FE2FUZZ,
	FIFO_FE2OG,
	FIFO_XY2SR,
	FIFO_XY2SW,
	FIFO_SR2OG,
	FIFO_TEX2OG,
	FIFO_FLAT2OG,
	FIFO_FUZZ2OG,
	FIFO_OG2SW,
	FIFO_OG2SW_C,
};

enum {
	FIFO_TYPE_32,
	FIFO_TYPE_BLOCK,
	FIFO_TYPE_BLOCK_MASK,
	FIFO_TYPE_MASK,
};

#define FIFO_32(a, b, s) {HARDDOOM_ ## a ## _STATE, HARDDOOM_ ## a ##_WINDOW, 0, offsetof(HardDoomState, b ## _state), offsetof(HardDoomState, b ## _data), 0, FIFO_TYPE_32, HARDDOOM_ ## a ## _SIZE, #a, HARDDOOM_RESET_ ## a, HARDDOOM_STATUS_ ## a, s}
#define FIFO_BLOCK(a, b, s) {HARDDOOM_ ## a ## _STATE, HARDDOOM_ ## a ##_WINDOW, 0, offsetof(HardDoomState, b ## _state), offsetof(HardDoomState, b ## _data), 0, FIFO_TYPE_BLOCK, HARDDOOM_ ## a ## _SIZE, #a, HARDDOOM_RESET_ ## a, HARDDOOM_STATUS_ ## a, s}
#define FIFO_BLOCK_MASK(a, b, s) {HARDDOOM_ ## a ## _STATE, HARDDOOM_ ## a ##_D_WINDOW, HARDDOOM_ ## a ## _M_WINDOW, offsetof(HardDoomState, b ## _state), offsetof(HardDoomState, b ## _data), offsetof(HardDoomState, b ## _mask), FIFO_TYPE_BLOCK_MASK, HARDDOOM_ ## a ## _SIZE, #a, HARDDOOM_RESET_ ## a, HARDDOOM_STATUS_ ## a, s}
#define FIFO_MASK(a, b, s) {HARDDOOM_ ## a ## _STATE, 0, HARDDOOM_ ## a ## _WINDOW, offsetof(HardDoomState, b ## _state), 0, offsetof(HardDoomState, b ## _mask), FIFO_TYPE_MASK, HARDDOOM_ ## a ## _SIZE, #a, HARDDOOM_RESET_ ## a, HARDDOOM_STATUS_ ## a, s}
static struct harddoom_fifo {
	uint32_t bar_off_state;
	uint32_t bar_off_window;
	uint32_t bar_off_mask;
	ptrdiff_t vm_off_state;
	ptrdiff_t vm_off_data;
	ptrdiff_t vm_off_mask;
	int type;
	uint32_t size;
	const char *name;
	uint32_t reset;
	uint32_t status;
	int stats_idx;
} harddoom_fifos[] = {
	[FIFO_MAIN] = FIFO_32(FIFO, fifo, HARDDOOM_STAT_FE_CMD),
	[FIFO_FE2XY] = FIFO_32(FE2XY, fe2xy, HARDDOOM_STAT_XY_CMD),
	[FIFO_FE2TEX] = FIFO_32(FE2TEX, fe2tex, HARDDOOM_STAT_TEX_CMD),
	[FIFO_FE2FLAT] = FIFO_32(FE2FLAT, fe2flat, HARDDOOM_STAT_FLAT_CMD),
	[FIFO_FE2FUZZ] = FIFO_32(FE2FUZZ, fe2fuzz, HARDDOOM_STAT_FUZZ_CMD),
	[FIFO_FE2OG] = FIFO_32(FE2OG, fe2og, HARDDOOM_STAT_OG_CMD),
	[FIFO_XY2SW] = FIFO_32(XY2SW, xy2sw, -1),
	[FIFO_XY2SR] = FIFO_32(XY2SR, xy2sr, -1),
	[FIFO_SR2OG] = FIFO_BLOCK(SR2OG, sr2og, HARDDOOM_STAT_SR_BLOCK),
	[FIFO_TEX2OG] = FIFO_BLOCK_MASK(TEX2OG, tex2og, HARDDOOM_STAT_TEX_BLOCK),
	[FIFO_FLAT2OG] = FIFO_BLOCK(FLAT2OG, flat2og, HARDDOOM_STAT_FLAT_BLOCK),
	[FIFO_FUZZ2OG] = FIFO_MASK(FUZZ2OG, fuzz2og, HARDDOOM_STAT_FUZZ_BLOCK),
	[FIFO_OG2SW] = FIFO_BLOCK_MASK(OG2SW, og2sw, HARDDOOM_STAT_SW_BLOCK),
	[FIFO_OG2SW_C] = FIFO_32(OG2SW_C, og2sw_c, HARDDOOM_STAT_SW_CMD),
};

static uint32_t le32_read(uint8_t *ptr) {
	return ptr[0] | ptr[1] << 8 | ptr[2] << 16 | ptr[3] << 24;
}

static int harddoom_fifo_rptr(HardDoomState *d, int which) {
	struct harddoom_fifo *desc = &harddoom_fifos[which];
	uint32_t *state = (void *)((char *)d + desc->vm_off_state);
	return *state & (desc->size - 1);
}

static int harddoom_fifo_wptr(HardDoomState *d, int which) {
	struct harddoom_fifo *desc = &harddoom_fifos[which];
	uint32_t *state = (void *)((char *)d + desc->vm_off_state);
	return *state >> 16 & (desc->size - 1);
}

/* Returns number of free slots in the FIFO.  */
static int harddoom_fifo_free(HardDoomState *d, int which) {
	struct harddoom_fifo *desc = &harddoom_fifos[which];
	uint32_t *state = (void *)((char *)d + desc->vm_off_state);
	int rptr = *state & 0xffff;
	int wptr = *state >> 16 & 0xffff;
	int used = (wptr - rptr) & (2 * desc->size - 1);
	/* This considers overfull FIFO to have free slots.
	 * It's part of the fun.  */
	return (desc->size - used) & (2 * desc->size - 1);
}

/* Bumps the FIFO read pointer, returns previous value.  */
static int harddoom_fifo_read_manual(HardDoomState *d, int which) {
	struct harddoom_fifo *desc = &harddoom_fifos[which];
	uint32_t *state = (void *)((char *)d + desc->vm_off_state);
	int rptr = *state & 0xffff;
	int wptr = *state >> 16 & 0xffff;
	int res = rptr & (desc->size - 1);
	rptr++;
	rptr &= (2 * desc->size - 1);
	*state = wptr << 16 | rptr;
	return res;
}

/* Reads from a FIFO.  */
static uint32_t harddoom_fifo32_read(HardDoomState *d, int which) {
	struct harddoom_fifo *desc = &harddoom_fifos[which];
	uint32_t *data = (void *)((char *)d + desc->vm_off_data);
	return data[harddoom_fifo_read_manual(d, which)];
}

/* Bumps the FIFO write pointer, returns previous value.  */
static int harddoom_fifo_write_manual(HardDoomState *d, int which) {
	struct harddoom_fifo *desc = &harddoom_fifos[which];
	uint32_t *state = (void *)((char *)d + desc->vm_off_state);
	if (desc->stats_idx != -1)
		d->stats[desc->stats_idx]++;
	int rptr = *state & 0xffff;
	int wptr = *state >> 16 & 0xffff;
	int res = wptr & (desc->size - 1);
	wptr++;
	wptr &= (2 * desc->size - 1);
	*state = wptr << 16 | rptr;
	return res;
}

/* Writes to a FIFO.  */
static void harddoom_fifo32_write(HardDoomState *d, int which, uint32_t value) {
	struct harddoom_fifo *desc = &harddoom_fifos[which];
	uint32_t *data = (void *)((char *)d + desc->vm_off_data);
	data[harddoom_fifo_write_manual(d, which)] = value;
	if (which == FIFO_MAIN) {
		d->fe_state &= ~HARDDOOM_FE_STATE_WAIT_FIFO;
	}
}

static int harddoom_fifo_can_read(HardDoomState *d, int which) {
	struct harddoom_fifo *desc = &harddoom_fifos[which];
	uint32_t *state = (void *)((char *)d + desc->vm_off_state);
	int rptr = *state & 0xffff;
	int wptr = *state >> 16 & 0xffff;
	return rptr != wptr;
}

/* Handles FIFO_SEND - appends a command to FIFO, or triggers FE_ERROR
 * or FIFO_OVERFLOW.  */
static void harddoom_fifo_send(HardDoomState *d, uint32_t val) {
	int free = harddoom_fifo_free(d, FIFO_MAIN);
	if (!free || !(d->enable & HARDDOOM_ENABLE_FIFO)) {
		d->intr |= HARDDOOM_INTR_FIFO_OVERFLOW;
	} else {
		harddoom_fifo32_write(d, FIFO_MAIN, val);
	}
}

/* Recomputes status register and PCI interrupt line.  */
static void harddoom_status_update(HardDoomState *d) {
	int i;
	d->status = 0;
	/* FETCH_CMD busy iff read != write.  */
	if (d->cmd_read_ptr != d->cmd_write_ptr)
		d->status |= HARDDOOM_STATUS_FETCH_CMD;
	/* FE busy iff not waiting for FIFO.  */
	if (!(d->fe_state & HARDDOOM_FE_STATE_WAIT_FIFO))
		d->status |= HARDDOOM_STATUS_FE;
	/* XY busy iff command pending in any of the three regs.  */
	if (HARDDOOM_XYCMD_EXTR_TYPE(d->xy_cmd))
		d->status |= HARDDOOM_STATUS_XY;
	if (HARDDOOM_XYCMD_EXTR_TYPE(d->xy_dst_cmd))
		d->status |= HARDDOOM_STATUS_XY;
	if (HARDDOOM_XYCMD_EXTR_TYPE(d->xy_src_cmd))
		d->status |= HARDDOOM_STATUS_XY;
	/* TEX busy iff command pending.  */
	if (HARDDOOM_TEXCMD_EXTR_TYPE(d->tex_cmd))
		d->status |= HARDDOOM_STATUS_TEX;
	/* FLAT busy iff command pending.  */
	if (HARDDOOM_FLCMD_EXTR_TYPE(d->flat_cmd))
		d->status |= HARDDOOM_STATUS_FLAT;
	/* FUZZ busy iff command pending.  */
	if (HARDDOOM_FZCMD_EXTR_TYPE(d->fuzz_cmd))
		d->status |= HARDDOOM_STATUS_FUZZ;
	/* OG busy iff command pending.  */
	if (HARDDOOM_OGCMD_EXTR_TYPE(d->og_cmd))
		d->status |= HARDDOOM_STATUS_OG;
	/* SW busy iff command pending.  */
	if (HARDDOOM_SWCMD_EXTR_TYPE(d->sw_cmd))
		d->status |= HARDDOOM_STATUS_SW;
	/* FIFOs busy iff read != write.  */
	for (i = 0; i < ARRAY_SIZE(harddoom_fifos); i++) {
		struct harddoom_fifo *desc = &harddoom_fifos[i];
		if (harddoom_fifo_can_read(d, i))
			d->status |= desc->status;
	}
	if (d->sw2xy_state)
		d->status |= HARDDOOM_STATUS_SW2XY;
	/* determine and set PCI interrupt status */
	pci_set_irq(PCI_DEVICE(d), !!(d->intr & d->intr_enable));
}

/* Resets TLBs, forcing a reread of PT.  */
static void harddoom_reset_tlb(HardDoomState *d) {
	/* Kill ENTRY valid bits.  */
	for (int i = 0; i < 3; i++)
		d->tlb_entry[i] &= ~HARDDOOM_TLB_ENTRY_VALID;
}

static void harddoom_reset(HardDoomState *d, uint32_t val) {
	int i;
	if (val & HARDDOOM_RESET_FE)
		d->fe_state = 0;
	if (val & HARDDOOM_RESET_XY) {
		d->xy_cmd = 0;
		d->xy_dst_cmd = 0;
		d->xy_src_cmd = 0;
	}
	if (val & HARDDOOM_RESET_TEX) {
		d->tex_cmd = 0;
	}
	if (val & HARDDOOM_RESET_FLAT) {
		d->flat_cmd = 0;
	}
	if (val & HARDDOOM_RESET_FUZZ) {
		d->fuzz_cmd = 0;
	}
	if (val & HARDDOOM_RESET_OG) {
		d->og_cmd = 0;
		d->og_misc &= ~HARDDOOM_OG_MISC_STATE_MASK;
	}
	if (val & HARDDOOM_RESET_SW) {
		d->sw_cmd = 0;
	}
	for (i = 0; i < ARRAY_SIZE(harddoom_fifos); i++) {
		struct harddoom_fifo *desc = &harddoom_fifos[i];
		uint32_t *state = (void *)((char *)d + desc->vm_off_state);
		if (val & desc->reset) {
			*state = 0;
		}
	}
	if (val & HARDDOOM_RESET_SW2XY)
		d->sw2xy_state = 0;
	if (val & HARDDOOM_RESET_STATS) {
		int i;
		for (i = 0; i < HARDDOOM_STATS_NUM; i++)
			d->stats[i] = 0;
	}
	if (val & HARDDOOM_RESET_TLB) {
		harddoom_reset_tlb(d);
	}
	if (val & HARDDOOM_RESET_FLAT_CACHE) {
		d->flat_cache_state &= ~HARDDOOM_FLAT_CACHE_STATE_VALID;
	}
	if (val & HARDDOOM_RESET_TEX_CACHE) {
		d->tex_cache_state &= ~HARDDOOM_TEX_CACHE_STATE_VALID;
	}
}

/* Sets the given PT and flushes corresponding TLB.  */
static void harddoom_set_pt(HardDoomState *d, int which, uint32_t val) {
	d->tlb_pt[which] = val;
	d->tlb_entry[which] &= ~HARDDOOM_TLB_ENTRY_VALID;
	d->stats[harddoom_tlbs[which].rebind_stats_idx]++;
}

/* Converts virtual offset to a physical address -- handles PT lookup.
 * If something goes wrong, disables the enable bit and fires an interrupt.
 * Returns true if succeeded.  */
static bool harddoom_translate_addr(HardDoomState *d, int which, uint32_t offset, uint32_t *res) {
	uint32_t vidx = offset >> HARDDOOM_PAGE_SHIFT & HARDDOOM_TLB_IDX_MASK;
	uint32_t entry = d->tlb_entry[which];
	uint32_t cur_vidx = entry >> HARDDOOM_TLB_ENTRY_IDX_SHIFT & HARDDOOM_TLB_IDX_MASK;
	uint32_t pte_addr = d->tlb_pt[which] + (vidx << 2);
	d->tlb_vaddr[which] = offset;
	if (vidx != cur_vidx || !(entry & HARDDOOM_TLB_ENTRY_VALID)) {
		/* Mismatched or invalid tag -- fetch a new one.  */
		uint8_t pteb[4];
		pci_dma_read(&d->dev, pte_addr, &pteb, sizeof pteb);
		d->tlb_entry[which] = entry = (le32_read(pteb) & 0xfffff001) | vidx << HARDDOOM_TLB_ENTRY_IDX_SHIFT | HARDDOOM_TLB_ENTRY_VALID;
		d->stats[harddoom_tlbs[which].miss_stats_idx]++;
	} else {
		d->stats[harddoom_tlbs[which].hit_stats_idx]++;
	}
	if (!(entry & HARDDOOM_PTE_VALID)) {
		d->enable &= ~harddoom_tlbs[which].enable;
		d->intr |= HARDDOOM_INTR_PAGE_FAULT_SURF_DST << which;
		return false;
	}
	*res = (entry & HARDDOOM_PTE_PHYS_MASK) | (offset & (HARDDOOM_PAGE_SIZE - 1));
	return true;
}

/* Converts x block, y position to physical address.  If something goes wrong,
 * triggers an interrupt and disables the enable bit.  Returns true if succeeded.  */
static bool harddoom_translate_surf_xy(HardDoomState *d, int which, uint16_t x, uint16_t y, uint32_t *res) {
	int w = HARDDOOM_CMD_EXTR_SURF_DIMS_WIDTH(d->xy_surf_dims);
	int h = HARDDOOM_CMD_EXTR_SURF_DIMS_HEIGHT(d->xy_surf_dims);
	uint32_t offset = y * w + (x << 6);
	offset &= HARDDOOM_TLB_VADDR_MASK;
	if (x >= w || y >= h) {
		/* Fire an interrupt and disable ourselves.  */
		d->enable &= ~HARDDOOM_ENABLE_XY;
		d->intr |= HARDDOOM_INTR_SURF_DST_OVERFLOW << which;
		return false;
	}
	return harddoom_translate_addr(d, which, y * w + (x << 6), res);
}

/* MMIO write handlers.  */
static void harddoom_mmio_writeb(void *opaque, hwaddr addr, uint32_t val)
{
	int i, idx;
	HardDoomState *d = opaque;
	qemu_mutex_lock(&d->mutex);
	bool found = false;
	for (i = 0; i < ARRAY_SIZE(harddoom_registers); i++) {
		struct harddoom_register *desc = &harddoom_registers[i];
		if (addr >= desc->bar_off && addr < desc->bar_off + desc->num && desc->type == REG_TYPE_SIMPLE_8) {
			uint8_t *reg = (void *)((char *)opaque + desc->vm_off);
			idx = addr - desc->bar_off;
			reg[idx] = val & desc->mask;
			if (val & ~desc->mask) {
				fprintf(stderr, "harddoom error: invalid %s value %08x (valid mask is %08x)\n", desc->name, val, desc->mask);
			}
			found = true;
		}
	}
	for (i = 0; i < ARRAY_SIZE(harddoom_fifos); i++) {
		struct harddoom_fifo *desc = &harddoom_fifos[i];
		if ((desc->type == FIFO_TYPE_BLOCK || desc->type == FIFO_TYPE_BLOCK_MASK) && (addr & ~0x3f) == desc->bar_off_window) {
			idx = addr & 0x3f;
			uint8_t *data = (void *)((char *)d + desc->vm_off_data);
			data[harddoom_fifo_wptr(d, i) * HARDDOOM_BLOCK_SIZE + idx] = val;
			found = true;
			break;
		}
	}
	if (!found) {
		fprintf(stderr, "harddoom error: byte-sized write at %03x, value %02x\n", (int)addr, val);
	}
	qemu_mutex_unlock(&d->mutex);
}

static void harddoom_mmio_writew(void *opaque, hwaddr addr, uint32_t val)
{
	fprintf(stderr, "harddoom error: word-sized write at %03x, value %04x\n", (int)addr, val);
}

static void harddoom_mmio_writel(void *opaque, hwaddr addr, uint32_t val)
{
	HardDoomState *d = opaque;
	qemu_mutex_lock(&d->mutex);
	int i, idx;
	bool found = false;
	for (i = 0; i < ARRAY_SIZE(harddoom_registers); i++) {
		struct harddoom_register *desc = &harddoom_registers[i];
		if (addr >= desc->bar_off && addr < desc->bar_off + desc->num * 4 && !(addr & 3) && desc->type == REG_TYPE_SIMPLE_32) {
			uint32_t *reg = (void *)((char *)opaque + desc->vm_off);
			idx = (addr - desc->bar_off) >> 2;
			reg[idx] = val & desc->mask;
			if (val & ~desc->mask) {
				fprintf(stderr, "harddoom error: invalid %s value %08x (valid mask is %08x)\n", desc->name, val, desc->mask);
			}
			found = true;
		}
		if (addr == desc->bar_off && desc->type == REG_TYPE_WINDOW_32) {
			uint32_t *reg = (void *)((char *)opaque + desc->vm_off);
			uint32_t *idx = (void *)((char *)opaque + desc->vm_off_idx);
			reg[*idx] = val & desc->mask;
			(*idx)++;
			(*idx) &= desc->num - 1;
			if (val & ~desc->mask) {
				fprintf(stderr, "harddoom error: invalid %s value %08x (valid mask is %08x)\n", desc->name, val, desc->mask);
			}
			found = true;
		}
		if (addr == desc->bar_off && desc->type == REG_TYPE_MASK) {
			uint64_t *reg = (void *)((char *)opaque + desc->vm_off);
			*reg &= ~0xffffffffull;
			*reg |= val;
			found = true;
		}
		if (addr == desc->bar_off + 4 && desc->type == REG_TYPE_MASK) {
			uint64_t *reg = (void *)((char *)opaque + desc->vm_off);
			*reg &= 0xffffffffull;
			*reg |= (uint64_t)val << 32;
			found = true;
		}
	}
	for (i = 0; i < ARRAY_SIZE(harddoom_fifos); i++) {
		struct harddoom_fifo *desc = &harddoom_fifos[i];
		if (addr == desc->bar_off_state) {
			uint32_t mask = (2 * desc->size - 1) * 0x10001;
			uint32_t *reg = (void *)((char *)opaque + desc->vm_off_state);
			*reg = val & mask;
			if (val & ~mask) {
				fprintf(stderr, "harddoom error: invalid %s_STATE value %08x (valid mask is %08x)\n", desc->name, val, mask);
			}
			found = true;
			break;
		}
		if (desc->type == FIFO_TYPE_32 && addr == desc->bar_off_window) {
			harddoom_fifo32_write(d, i, val);
			found = true;
			break;
		}
		if ((desc->type == FIFO_TYPE_MASK || desc->type == FIFO_TYPE_BLOCK_MASK) && addr == desc->bar_off_mask) {
			uint64_t *data = (void *)((char *)d + desc->vm_off_mask);
			int wptr = harddoom_fifo_wptr(d, i);
			data[wptr] &= ~0xffffffffull;
			data[wptr] |= val;
			found = true;
		}
		if ((desc->type == FIFO_TYPE_MASK || desc->type == FIFO_TYPE_BLOCK_MASK) && addr == desc->bar_off_mask + 4) {
			uint64_t *data = (void *)((char *)d + desc->vm_off_mask);
			int wptr = harddoom_fifo_wptr(d, i);
			data[wptr] &= 0xffffffffull;
			data[wptr] |= (uint64_t)val << 32;
			found = true;
		}
	}
	if (addr == HARDDOOM_RESET) {
		harddoom_reset(d, val);
		if (val & ~HARDDOOM_RESET_ALL)
			fprintf(stderr, "harddoom error: invalid RESET value %08x\n", val);
		found = true;
	}
	if (addr == HARDDOOM_INTR) {
		d->intr &= ~val;
		if (val & ~HARDDOOM_INTR_MASK)
			fprintf(stderr, "harddoom error: invalid INTR value %08x\n", val);
		found = true;
	}
	if (addr == HARDDOOM_FIFO_SEND) {
		harddoom_fifo_send(d, val);
		found = true;
	}
	if (!found)
		fprintf(stderr, "harddoom error: invalid register write at %03x, value %08x\n", (int)addr, val);
	harddoom_status_update(d);
	qemu_cond_signal(&d->cond);
	qemu_mutex_unlock(&d->mutex);
}

static uint32_t harddoom_mmio_readb(void *opaque, hwaddr addr)
{
	int i, idx;
	uint8_t res = 0xff;
	bool found = false;
	HardDoomState *d = opaque;
	qemu_mutex_lock(&d->mutex);
	for (i = 0; i < ARRAY_SIZE(harddoom_registers); i++) {
		struct harddoom_register *desc = &harddoom_registers[i];
		if (addr >= desc->bar_off && addr < desc->bar_off + desc->num && desc->type == REG_TYPE_SIMPLE_8) {
			uint8_t *reg = (void *)((char *)opaque + desc->vm_off);
			idx = addr - desc->bar_off;
			res = reg[idx];
			found = true;
		}
	}
	for (i = 0; i < ARRAY_SIZE(harddoom_fifos); i++) {
		struct harddoom_fifo *desc = &harddoom_fifos[i];
		if ((desc->type == FIFO_TYPE_BLOCK || desc->type == FIFO_TYPE_BLOCK_MASK) && (addr & ~0x3f) == desc->bar_off_window) {
			idx = addr & 0x3f;
			uint8_t *data = (void *)((char *)d + desc->vm_off_data);
			res = data[harddoom_fifo_rptr(d, i) * HARDDOOM_BLOCK_SIZE + idx];
			found = true;
			break;
		}
	}
	if (!found) {
		fprintf(stderr, "harddoom error: byte-sized read at %03x\n", (int)addr);
		res = 0xff;
	}
	qemu_mutex_unlock(&d->mutex);
	return res;
}

static uint32_t harddoom_mmio_readw(void *opaque, hwaddr addr)
{
	fprintf(stderr, "harddoom error: word-sized read at %03x\n", (int)addr);
	return 0xffff;
}

static uint32_t harddoom_mmio_readl(void *opaque, hwaddr addr)
{
	HardDoomState *d = opaque;
	uint32_t res = 0xffffffff;
	qemu_mutex_lock(&d->mutex);
	bool found = false;
	int i, idx;
	for (i = 0; i < ARRAY_SIZE(harddoom_registers); i++) {
		struct harddoom_register *desc = &harddoom_registers[i];
		if (addr >= desc->bar_off && addr < desc->bar_off + desc->num * 4 && !(addr & 3) && desc->type == REG_TYPE_SIMPLE_32) {
			uint32_t *reg = (void *)((char *)opaque + desc->vm_off);
			idx = (addr - desc->bar_off) >> 2;
			res = reg[idx];
			found = true;
			break;
		}
		if (addr == desc->bar_off && desc->type == REG_TYPE_WINDOW_32) {
			uint32_t *reg = (void *)((char *)opaque + desc->vm_off);
			uint32_t *idx = (void *)((char *)opaque + desc->vm_off_idx);
			res = reg[*idx];
			(*idx)++;
			(*idx) &= desc->num - 1;
			found = true;
			break;
		}
		if (addr == desc->bar_off && desc->type == REG_TYPE_MASK) {
			uint64_t *reg = (void *)((char *)opaque + desc->vm_off);
			res = *reg;
			found = true;
		}
		if (addr == desc->bar_off + 4 && desc->type == REG_TYPE_MASK) {
			uint64_t *reg = (void *)((char *)opaque + desc->vm_off);
			res = *reg >> 32;
			found = true;
		}
	}
	for (i = 0; i < ARRAY_SIZE(harddoom_fifos); i++) {
		struct harddoom_fifo *desc = &harddoom_fifos[i];
		if (addr == desc->bar_off_state) {
			uint32_t *reg = (void *)((char *)opaque + desc->vm_off_state);
			res = *reg;
			found = true;
			break;
		}
		if (desc->type == FIFO_TYPE_32 && addr == desc->bar_off_window) {
			res = harddoom_fifo32_read(d, i);
			found = true;
			break;
		}
		if ((desc->type == FIFO_TYPE_MASK || desc->type == FIFO_TYPE_BLOCK_MASK) && addr == desc->bar_off_mask) {
			uint64_t *data = (void *)((char *)d + desc->vm_off_mask);
			int wptr = harddoom_fifo_rptr(d, i);
			res = data[wptr];
			found = true;
		}
		if ((desc->type == FIFO_TYPE_MASK || desc->type == FIFO_TYPE_BLOCK_MASK) && addr == desc->bar_off_mask + 4) {
			uint64_t *data = (void *)((char *)d + desc->vm_off_mask);
			int wptr = harddoom_fifo_rptr(d, i);
			res = data[wptr] >> 32;
			found = true;
		}
	}
	if (addr == HARDDOOM_STATUS) {
		res = d->status;
		found = true;
	}
	if (addr == HARDDOOM_INTR) {
		res = d->intr;
		found = true;
	}
	if (addr == HARDDOOM_FIFO_FREE) {
		res = harddoom_fifo_free(d, FIFO_MAIN);
		found = true;
	}
	if (!found) {
		fprintf(stderr, "harddoom error: invalid register read at %03x\n", (int)addr);
		res = 0xffffffff;
	}
	harddoom_status_update(d);
	qemu_cond_signal(&d->cond);
	qemu_mutex_unlock(&d->mutex);
	return res;
}

static const MemoryRegionOps harddoom_mmio_ops = {
	.old_mmio = {
		.read = {
			harddoom_mmio_readb,
			harddoom_mmio_readw,
			harddoom_mmio_readl,
		},
		.write = {
			harddoom_mmio_writeb,
			harddoom_mmio_writew,
			harddoom_mmio_writel,
		},
	},
	.endianness = DEVICE_LITTLE_ENDIAN,
};

/* Power-up reset of the device.  */
static void harddoom_power_reset(DeviceState *ds)
{
	HardDoomState *s = container_of(ds, HardDoomState, dev.qdev);
	int i, j;
	qemu_mutex_lock(&s->mutex);
	for (i = 0; i < ARRAY_SIZE(harddoom_registers); i++) {
		struct harddoom_register *desc = &harddoom_registers[i];
		if (desc->type == REG_TYPE_SIMPLE_32 || desc->type == REG_TYPE_WINDOW_32) {
			uint32_t *reg = (void *)((char *)s + desc->vm_off);
			for (j = 0; j < desc->num; j++)
				reg[j] = mrand48() & desc->mask;
		} else if (desc->type == REG_TYPE_SIMPLE_8) {
			uint8_t *reg = (void *)((char *)s + desc->vm_off);
			for (j = 0; j < desc->num; j++)
				reg[j] = mrand48() & desc->mask;
		} else if (desc->type == REG_TYPE_MASK) {
			uint64_t *reg = (void *)((char *)s + desc->vm_off);
			*reg = (uint64_t)mrand48() << 32;
			*reg |= (uint32_t)mrand48();
		}
	}
	for (i = 0; i < ARRAY_SIZE(harddoom_fifos); i++) {
		struct harddoom_fifo *desc = &harddoom_fifos[i];
		uint32_t mask = (2 * desc->size - 1) * 0x10001;
		uint32_t *state = (void *)((char *)s + desc->vm_off_state);
		*state = mrand48() & mask;
		if (desc->type == FIFO_TYPE_32) {
			uint32_t *data = (void *)((char *)s + desc->vm_off_data);
			for (j = 0; j < desc->size; j++)
				data[j] = mrand48();
		}
		if (desc->type == FIFO_TYPE_BLOCK || desc->type == FIFO_TYPE_BLOCK_MASK) {
			uint8_t *data = (void *)((char *)s + desc->vm_off_data);
			for (j = 0; j < desc->size * HARDDOOM_BLOCK_SIZE; j++)
				data[j] = mrand48();
		}
		if (desc->type == FIFO_TYPE_MASK || desc->type == FIFO_TYPE_BLOCK_MASK) {
			uint64_t *mask = (void *)((char *)s + desc->vm_off_mask);
			for (j = 0; j < desc->size; j++) {
				mask[j] = (uint64_t)mrand48() << 32;
				mask[j] |= (uint32_t)mrand48();
			}
		}
	}
	s->intr = mrand48() & HARDDOOM_INTR_MASK;
	/* These registers play fair. */
	s->enable = 0;
	s->intr_enable = 0;
	harddoom_status_update(s);
	qemu_mutex_unlock(&s->mutex);
}

/* Runs FETCH_CMD for some time.  Returns true if anything has been done.  */
static bool harddoom_run_fetch_cmd(HardDoomState *d) {
	bool any = false;
	if (!(d->enable & HARDDOOM_ENABLE_FETCH_CMD))
		return any;
	while (d->cmd_read_ptr != d->cmd_write_ptr) {
		/* Now, check if there's some place to put commands.  */
		if (!harddoom_fifo_free(d, FIFO_MAIN))
			break;
		/* There are commands to read, and there's somewhere to put them. Do it.  */
		uint8_t cmdb[4];
		pci_dma_read(&d->dev, d->cmd_read_ptr, &cmdb, sizeof cmdb);
		uint32_t cmd = le32_read(cmdb);
		d->cmd_read_ptr += sizeof cmdb;
		if (HARDDOOM_CMD_EXTR_TYPE_HI(cmd) == HARDDOOM_CMD_TYPE_HI_JUMP) {
			d->cmd_read_ptr = HARDDOOM_CMD_EXTR_JUMP_ADDR(cmd);
		} else {
			/* This could cause an INVALID_CMD.  We don't care.  */
			harddoom_fifo_send(d, cmd);
		}
		any = true;
	}
	return any;
}

/* Runs the FE for some time.  Returns true if anything has been done.  */
static bool harddoom_run_fe(HardDoomState *d) {
	bool any = false;
	if (!(d->enable & HARDDOOM_ENABLE_FE))
		return any;
	if (d->fe_state & HARDDOOM_FE_STATE_WAIT_FIFO)
		return any;
	int num_insns = 0x1000;
	while (num_insns--) {
		int pc = d->fe_state & HARDDOOM_FE_STATE_PC_MASK;
		uint32_t insn = d->fe_code[pc++];
		pc &= HARDDOOM_FE_STATE_PC_MASK;
		int op1 = insn >> 26 & 0x3f;
		int op2;
		int r1, r2, r3;
		uint32_t imm1, imm2, imm3;
		uint32_t addr;
		uint32_t cmd;
		uint32_t tmp, mask;
		//printf("FE %03x %08x\n", pc, insn);
		switch (op1) {
		case 0x00:
			/* rri12 format.  */
			op2 = insn >> 22 & 0xf;
			r1 = insn >> 17 & 0x1f;
			r2 = insn >> 12 & 0x1f;
			imm1 = insn & 0xfff;
			switch (op2) {
			case 0x0:
				/* r1 is third-level opcode.  */
				switch (r1) {
				case 0x00:
					/* rcmd -- read command from FIFO.  r2 is destination register.  */
					if (harddoom_fifo_can_read(d, FIFO_MAIN)) {
						d->fe_reg[r2] = harddoom_fifo32_read(d, FIFO_MAIN);
						//printf("CMD %08x\n", d->fe_reg[r2]);
					} else {
						d->fe_state |= HARDDOOM_FE_STATE_WAIT_FIFO;
						return true;
					}
					break;
				case 0x01:
					/* error -- trigger FE_ERROR and stop FE.  r2 is register with error command, imm1 is error code.  */
					d->fe_error_code = imm1;
					d->fe_error_cmd = d->fe_reg[r2];
					d->enable &= ~HARDDOOM_ENABLE_FE;
					d->intr |= HARDDOOM_INTR_FE_ERROR;
					d->fe_state = pc;
					return true;
				case 0x02:
					/* pong -- trigger PONG_ASYNC.  No arguments.  */
					d->intr |= HARDDOOM_INTR_PONG_ASYNC;
					break;
				case 0x03:
					/* xycmd -- send a command to XY unit.  r2 is register with command params, imm1 is command type.  */
					cmd = d->fe_reg[r2] & HARDDOOM_XYCMD_PARAMS_MASK;
					cmd |= imm1 << HARDDOOM_XYCMD_TYPE_SHIFT;
					if (!harddoom_fifo_free(d, FIFO_FE2XY)) {
						return any;
					} else {
						//printf("XYCMD %08x\n", cmd);
						harddoom_fifo32_write(d, FIFO_FE2XY, cmd);
					}
					break;
				case 0x04:
					/* texcmd -- send a command to TEX unit.  r2 is register with command params, imm1 is command type.  */
					cmd = d->fe_reg[r2] & HARDDOOM_TEXCMD_PARAMS_MASK;
					cmd |= imm1 << HARDDOOM_TEXCMD_TYPE_SHIFT;
					if (!harddoom_fifo_free(d, FIFO_FE2TEX)) {
						return any;
					} else {
						//printf("TEXCMD %08x\n", cmd);
						harddoom_fifo32_write(d, FIFO_FE2TEX, cmd);
					}
					break;
				case 0x05:
					/* flcmd -- send a command to FLAT unit.  r2 is register with command params, imm1 is command type.  */
					cmd = d->fe_reg[r2] & HARDDOOM_FLCMD_PARAMS_MASK;
					cmd |= imm1 << HARDDOOM_FLCMD_TYPE_SHIFT;
					if (!harddoom_fifo_free(d, FIFO_FE2FLAT)) {
						return any;
					} else {
						//printf("FLCMD %08x\n", cmd);
						harddoom_fifo32_write(d, FIFO_FE2FLAT, cmd);
					}
					break;
				case 0x06:
					/* fzcmd -- send a command to FUZZ unit.  r2 is register with command params, imm1 is command type.  */
					cmd = d->fe_reg[r2] & HARDDOOM_FZCMD_PARAMS_MASK;
					cmd |= imm1 << HARDDOOM_FZCMD_TYPE_SHIFT;
					if (!harddoom_fifo_free(d, FIFO_FE2FUZZ)) {
						return any;
					} else {
						//printf("FZCMD %08x\n", cmd);
						harddoom_fifo32_write(d, FIFO_FE2FUZZ, cmd);
					}
					break;
				case 0x07:
					/* ogcmd -- send a command to OG unit.  r2 is register with command params, imm1 is command type.  */
					cmd = d->fe_reg[r2] & HARDDOOM_OGCMD_PARAMS_MASK;
					cmd |= imm1 << HARDDOOM_OGCMD_TYPE_SHIFT;
					if (!harddoom_fifo_free(d, FIFO_FE2OG)) {
						return any;
					} else {
						//printf("OGCMD %08x\n", cmd);
						harddoom_fifo32_write(d, FIFO_FE2OG, cmd);
					}
					break;
				case 0x08:
					/* b -- unconditional branch to imm1.  */
					pc = imm1;
					break;
				case 0x09:
					/* bl -- unconditional branch to imm1, saving return address in r2 register.  */
					d->fe_reg[r2] = pc;
					pc = imm1;
					break;
				case 0x0a:
					/* bi -- branch indirect to imm1 + register r2.  */
					pc = imm1 + d->fe_reg[r2];
					pc &= HARDDOOM_FE_STATE_PC_MASK;
					break;
				case 0x0b:
					/* stat -- bump statistics counter #imm1.  */
					d->stats[imm1 & 0xf]++;
					break;
				}
				break;
			case 0x2:
				/* bbs -- branch if bit r2 set in register r1.  r2 is treated as an immediate here.  */
				if (d->fe_reg[r1] >> r2 & 1)
					pc = imm1;
				break;
			case 0x3:
				/* bbc -- branch if bit clear, like above.  */
				if (!(d->fe_reg[r1] >> r2 & 1))
					pc = imm1;
				break;
			case 0x4:
				/* be -- branch if registers equal.  */
				if (d->fe_reg[r1] == d->fe_reg[r2])
					pc = imm1;
				break;
			case 0x5:
				/* bne -- branch if registers not equal.  */
				if (d->fe_reg[r1] != d->fe_reg[r2])
					pc = imm1;
				break;
			case 0x6:
				/* bg -- branch if register r1 is greater than register r2.  */
				if (d->fe_reg[r1] > d->fe_reg[r2])
					pc = imm1;
				break;
			case 0x7:
				/* ble -- branch if register r1 is <= register r2.  */
				if (d->fe_reg[r1] <= d->fe_reg[r2])
					pc = imm1;
				break;
			case 0x8:
				/* st -- store register r1 to memory at register r2 + imm1. */
				addr = d->fe_reg[r2] + imm1;
				addr &= HARDDOOM_FE_DATA_SIZE - 1;
				d->fe_data[addr] = d->fe_reg[r1];
				break;
			case 0x9:
				/* ld -- load register r1 from memory at register r2 + imm1. */
				addr = d->fe_reg[r2] + imm1;
				addr &= HARDDOOM_FE_DATA_SIZE - 1;
				d->fe_reg[r1] = d->fe_data[addr];
				break;
			}
			break;
		case 0x01:
			/* ri7i12 format.  */
			op2 = insn >> 24 & 3;
			r1 = insn >> 19 & 0x1f;
			imm1 = insn >> 12 & 0x7f;
			imm2 = insn & 0xfff;
			switch (op2) {
			case 0x0:
				/* bei -- branch if register r1 equal to imm1.  */
				if (d->fe_reg[r1] == imm1)
					pc = imm2;
				break;
			case 0x1:
				/* bnei -- branch if register r1 not equal to imm1.  */
				if (d->fe_reg[r1] != imm1)
					pc = imm2;
				break;
			case 0x2:
				/* bgi -- branch if register r1 greater than imm1.  */
				if (d->fe_reg[r1] > imm1)
					pc = imm2;
				break;
			case 0x3:
				/* blei -- branch if register r1 <= imm1.  */
				if (d->fe_reg[r1] <= imm1)
					pc = imm2;
				break;
			}
			break;
		case 0x02:
			/* rrrrr format.  */
			/* mb or mbc -- move bits or move bits and clear others.  r1 is destination register,
			 * imm1 is destination bit position, r2 is source register, imm2 is source bit position,
			 * imm3 is size in bits - 1.  */
			op2 = insn >> 25 & 1;
			r1 = insn >> 20 & 0x1f;
			imm1 = insn >> 15 & 0x1f;
			r2 = insn >> 10 & 0x1f;
			imm2 = insn >> 5 & 0x1f;
			imm3 = insn >> 0 & 0x1f;
			mask = (((uint32_t)2 << imm3) - 1) << imm1;
			tmp = d->fe_reg[r2] >> imm2 << imm1 & mask;
			if (op2 == 0) {
				tmp |= d->fe_reg[r1] & ~mask;
			}
			d->fe_reg[r1] = tmp;
			break;
		case 0x03:
			/* mbi -- move bits immediate.  Inserts (imm3+1) bits of imm2 (zero-extended) to register r1 at position imm1.  */
			r1 = insn >> 21 & 0x1f;
			imm1 = insn >> 16 & 0x1f;
			imm2 = insn >> 5 & 0x7ff;
			imm3 = insn & 0x1f;
			mask = (((uint32_t)2 << imm3) - 1) << imm1;
			tmp = imm2 << imm1 & mask;
			tmp |= d->fe_reg[r1] & ~mask;
			d->fe_reg[r1] = tmp;
			break;
		case 0x04:
			/* li -- load immediate.  */
			r1 = insn >> 21 & 0x1f;
			imm1 = insn & 0xffff;
			if (imm1 & 0x8000)
				imm1 |= 0xffff0000;
			d->fe_reg[r1] = imm1;
			break;
		case 0x05:
			/* ai -- add immediate.  */
			r1 = insn >> 21 & 0x1f;
			r2 = insn >> 16 & 0x1f;
			imm1 = insn & 0xffff;
			if (imm1 & 0x8000)
				imm1 |= 0xffff0000;
			d->fe_reg[r1] = d->fe_reg[r2] + imm1;
			break;
		case 0x06:
			/* a -- add reg r2 + imm1 + reg r3, store to r1.  */
			r1 = insn >> 21 & 0x1f;
			r2 = insn >> 16 & 0x1f;
			imm1 = insn >> 5 & 0x7ff;
			if (imm1 & 0x400)
				imm1 |= 0xfffff800;
			r3 = insn & 0x1f;
			d->fe_reg[r1] = d->fe_reg[r2] + imm1 + d->fe_reg[r3];
			break;
		case 0x07:
			/* s -- add reg r2 + imm1, subtract reg r3, store to r1.  */
			r1 = insn >> 21 & 0x1f;
			r2 = insn >> 16 & 0x1f;
			imm1 = insn >> 5 & 0x7ff;
			if (imm1 & 0x400)
				imm1 |= 0xfffff800;
			r3 = insn & 0x1f;
			d->fe_reg[r1] = d->fe_reg[r2] + imm1 - d->fe_reg[r3];
			break;
		case 0x08:
			/* arm -- add reg r2 + reg r3, store sum modulo imm1 to r1.  */
			r1 = insn >> 21 & 0x1f;
			r2 = insn >> 16 & 0x1f;
			imm1 = insn >> 5 & 0x7ff;
			r3 = insn & 0x1f;
			d->fe_reg[r1] = d->fe_reg[r2] + d->fe_reg[r3];
			if (imm1)
				d->fe_reg[r1] %= imm1;
			break;
		case 0x09:
			/* sign --  store sign of r2-r3 to r1.  */
			r1 = insn >> 21 & 0x1f;
			r2 = insn >> 16 & 0x1f;
			r3 = insn & 0x1f;
			d->fe_reg[r1] = (d->fe_reg[r2] > d->fe_reg[r3] ? 1 : -1);
			break;
		}
		d->fe_state = pc;
		any = true;
	}
	return any;
}

/* Runs the XY for some time.  Returns true if anything has been done.  */
static bool harddoom_run_xy(HardDoomState *d) {
	bool any = false;
	int iterations = 0x80;
	while (iterations--) {
		bool any_this = false;
		bool dst_busy = !!HARDDOOM_XYCMD_EXTR_TYPE(d->xy_dst_cmd);
		bool src_busy = !!HARDDOOM_XYCMD_EXTR_TYPE(d->xy_src_cmd);
		if (!(d->enable & HARDDOOM_ENABLE_XY))
			return any;
		/* If we don't currently have a pending command, fetch one from FE.  */
		if (!HARDDOOM_XYCMD_EXTR_TYPE(d->xy_cmd) && harddoom_fifo_can_read(d, FIFO_FE2XY)) {
			d->xy_cmd = harddoom_fifo32_read(d, FIFO_FE2XY);
			any_this = true;
		}
		/* Try to execute the pending command.  */
		switch (HARDDOOM_XYCMD_EXTR_TYPE(d->xy_cmd)) {
			case HARDDOOM_XYCMD_TYPE_SURF_DST_PT:
				/* Can change destination PT iff destination not busy.  */
				if (dst_busy)
					break;
				harddoom_set_pt(d, TLB_SURF_DST, d->xy_cmd << 6);
				d->xy_cmd = 0;
				any_this = true;
				break;
			case HARDDOOM_XYCMD_TYPE_SURF_SRC_PT:
				/* Can change source PT iff destination not busy.  */
				if (src_busy)
					break;
				harddoom_set_pt(d, TLB_SURF_SRC, d->xy_cmd << 6);
				d->xy_cmd = 0;
				any_this = true;
				break;
			case HARDDOOM_XYCMD_TYPE_SURF_DIMS:
				/* Can change surf dimensions iff both src and dst are idle.  */
				if (dst_busy || src_busy)
					break;
				d->xy_surf_dims = d->xy_cmd & HARDDOOM_XY_SURF_DIMS_MASK;
				d->xy_cmd = 0;
				any_this = true;
				break;
			case HARDDOOM_XYCMD_TYPE_WRITE_DST_H:
			case HARDDOOM_XYCMD_TYPE_WRITE_DST_V:
			case HARDDOOM_XYCMD_TYPE_READ_DST_V:
			case HARDDOOM_XYCMD_TYPE_RMW_DST_V:
				/* Pass these to dst subunit if not busy.  */
				if (dst_busy)
					break;
				d->xy_dst_cmd = d->xy_cmd;
				d->xy_cmd = 0;
				any_this = true;
				break;
			case HARDDOOM_XYCMD_TYPE_READ_SRC_H:
			case HARDDOOM_XYCMD_TYPE_READ_SRC_V:
				/* Pass these to src subunit if not busy.  */
				if (src_busy)
					break;
				d->xy_src_cmd = d->xy_cmd;
				d->xy_cmd = 0;
				any_this = true;
				break;
			case HARDDOOM_XYCMD_TYPE_INTERLOCK:
				/* If interlock signal from SW ready, consume it and the command.  Otherwise, wait.  */
				if (!d->sw2xy_state)
					break;
				d->stats[HARDDOOM_STAT_XY_INTERLOCK]++;
				d->sw2xy_state--;
				d->xy_cmd = 0;
				any_this = true;
				break;
		}
		/* Try to step the dst subunit.  */
		if (HARDDOOM_XYCMD_EXTR_TYPE(d->xy_dst_cmd)) {
			int x = HARDDOOM_XYCMD_EXTR_X(d->xy_dst_cmd);
			int y = HARDDOOM_XYCMD_EXTR_Y(d->xy_dst_cmd);
			uint32_t addr;
			int count;
			switch (HARDDOOM_XYCMD_EXTR_TYPE(d->xy_dst_cmd)) {
				case HARDDOOM_XYCMD_TYPE_WRITE_DST_H:
					count = HARDDOOM_XYCMD_EXTR_WIDTH(d->xy_dst_cmd);
					if (!count) {
						d->xy_dst_cmd = 0;
						any_this = true;
						break;
					}
					if (!harddoom_fifo_free(d, FIFO_XY2SW))
						break;
					if (harddoom_translate_surf_xy(d, TLB_SURF_DST, x, y, &addr)) {
						harddoom_fifo32_write(d, FIFO_XY2SW, addr);
						count--;
						x++;
						d->xy_dst_cmd = HARDDOOM_XYCMD_WRITE_DST_H(x, y, count);
						any_this = true;
					}
					break;
				case HARDDOOM_XYCMD_TYPE_WRITE_DST_V:
					count = HARDDOOM_XYCMD_EXTR_HEIGHT(d->xy_dst_cmd);
					if (!count) {
						d->xy_dst_cmd = 0;
						any_this = true;
						break;
					}
					if (!harddoom_fifo_free(d, FIFO_XY2SW))
						break;
					if (harddoom_translate_surf_xy(d, TLB_SURF_DST, x, y, &addr)) {
						harddoom_fifo32_write(d, FIFO_XY2SW, addr);
						count--;
						y++;
						d->xy_dst_cmd = HARDDOOM_XYCMD_WRITE_DST_V(x, y, count);
						any_this = true;
					}
					break;
				case HARDDOOM_XYCMD_TYPE_READ_DST_V:
					count = HARDDOOM_XYCMD_EXTR_HEIGHT(d->xy_dst_cmd);
					if (!count) {
						d->xy_dst_cmd = 0;
						any_this = true;
						break;
					}
					if (!harddoom_fifo_free(d, FIFO_XY2SR))
						break;
					if (harddoom_translate_surf_xy(d, TLB_SURF_DST, x, y, &addr)) {
						harddoom_fifo32_write(d, FIFO_XY2SR, addr);
						count--;
						y++;
						d->xy_dst_cmd = HARDDOOM_XYCMD_READ_DST_V(x, y, count);
						any_this = true;
					}
					break;
				case HARDDOOM_XYCMD_TYPE_RMW_DST_V:
					count = HARDDOOM_XYCMD_EXTR_HEIGHT(d->xy_dst_cmd);
					if (!count) {
						d->xy_dst_cmd = 0;
						any_this = true;
						break;
					}
					if (!harddoom_fifo_free(d, FIFO_XY2SR))
						break;
					if (!harddoom_fifo_free(d, FIFO_XY2SW))
						break;
					if (harddoom_translate_surf_xy(d, TLB_SURF_DST, x, y, &addr)) {
						harddoom_fifo32_write(d, FIFO_XY2SR, addr);
						harddoom_fifo32_write(d, FIFO_XY2SW, addr);
						count--;
						y++;
						d->xy_dst_cmd = HARDDOOM_XYCMD_RMW_DST_V(x, y, count);
						any_this = true;
					}
					break;
			}
		}
		/* Try to step the src subunit.  */
		if (HARDDOOM_XYCMD_EXTR_TYPE(d->xy_src_cmd)) {
			int x = HARDDOOM_XYCMD_EXTR_X(d->xy_src_cmd);
			int y = HARDDOOM_XYCMD_EXTR_Y(d->xy_src_cmd);
			uint32_t addr;
			int count;
			switch (HARDDOOM_XYCMD_EXTR_TYPE(d->xy_src_cmd)) {
				case HARDDOOM_XYCMD_TYPE_READ_SRC_H:
					count = HARDDOOM_XYCMD_EXTR_WIDTH(d->xy_src_cmd);
					if (!count) {
						d->xy_src_cmd = 0;
						any_this = true;
						break;
					}
					if (!harddoom_fifo_free(d, FIFO_XY2SR))
						break;
					if (harddoom_translate_surf_xy(d, TLB_SURF_SRC, x, y, &addr)) {
						harddoom_fifo32_write(d, FIFO_XY2SR, addr);
						count--;
						x++;
						d->xy_src_cmd = HARDDOOM_XYCMD_READ_SRC_H(x, y, count);
						any_this = true;
					}
					break;
				case HARDDOOM_XYCMD_TYPE_READ_SRC_V:
					count = HARDDOOM_XYCMD_EXTR_HEIGHT(d->xy_src_cmd);
					if (!count) {
						d->xy_src_cmd = 0;
						any_this = true;
						break;
					}
					if (!harddoom_fifo_free(d, FIFO_XY2SR))
						break;
					if (harddoom_translate_surf_xy(d, TLB_SURF_SRC, x, y, &addr)) {
						harddoom_fifo32_write(d, FIFO_XY2SR, addr);
						count--;
						y++;
						d->xy_src_cmd = HARDDOOM_XYCMD_READ_SRC_V(x, y, count);
						any_this = true;
					}
					break;
			}
		}
		any |= any_this;
		if (!any_this)
			return any;
	}
	return any;
}

/* Runs the SR for some time.  Returns true if anything has been done.  */
static bool harddoom_run_sr(HardDoomState *d) {
	bool any = false;
	if (!(d->enable & HARDDOOM_ENABLE_SR))
		return false;
	while (1) {
		uint32_t addr;
		if (!harddoom_fifo_can_read(d, FIFO_XY2SR))
			return any;
		if (!harddoom_fifo_free(d, FIFO_SR2OG))
			return any;
		addr = harddoom_fifo32_read(d, FIFO_XY2SR);
		int wptr = harddoom_fifo_write_manual(d, FIFO_SR2OG);
		pci_dma_read(&d->dev, addr, &d->sr2og_data[wptr * HARDDOOM_BLOCK_SIZE], HARDDOOM_BLOCK_SIZE);
		any = true;
	}
	return any;
}

/* Ensures the flat cache is filled with the given line.  */
static void harddoom_flat_fill_cache(HardDoomState *d, int y) {
	if ((d->flat_cache_state & HARDDOOM_FLAT_CACHE_STATE_VALID) &&
		((d->flat_cache_state & HARDDOOM_FLAT_CACHE_STATE_TAG_MASK) == y)) {
		d->stats[HARDDOOM_STAT_FLAT_CACHE_HIT]++;
		return;
	}
	d->stats[HARDDOOM_STAT_FLAT_CACHE_MISS]++;
	uint32_t phys = d->flat_addr | y << 6;
	pci_dma_read(&d->dev, phys, d->flat_cache, HARDDOOM_FLAT_CACHE_SIZE);
	d->flat_cache_state = y | HARDDOOM_FLAT_CACHE_STATE_VALID;
}

static bool harddoom_tex_cached(HardDoomState *d, uint32_t offset) {
	int tag = offset >> 6;
	return (d->tex_cache_state & HARDDOOM_TEX_CACHE_STATE_VALID) &&
		(d->tex_cache_state & HARDDOOM_TEX_CACHE_STATE_TAG_MASK) == tag;
}

/* Ensures the texture cache is filled with the line containing the given offset.
 * May cause a page fault and disable DRAW.  */
static bool harddoom_tex_fill_cache(HardDoomState *d, uint32_t offset) {
	if (harddoom_tex_cached(d, offset)) {
		d->stats[HARDDOOM_STAT_TEX_CACHE_HIT]++;
		return true;
	}
	d->stats[HARDDOOM_STAT_TEX_CACHE_MISS]++;
	if (offset < HARDDOOM_CMD_EXTR_TEXTURE_SIZE(d->tex_dims)) {
		uint32_t phys;
		if (!harddoom_translate_addr(d, TLB_TEXTURE, offset & ~0x3f, &phys))
			return false;
		pci_dma_read(&d->dev, phys, d->tex_cache, HARDDOOM_TEX_CACHE_SIZE);
	} else {
		memset(d->tex_cache, 0, sizeof d->tex_cache);
	}
	d->tex_cache_state &= ~HARDDOOM_TEX_CACHE_STATE_TAG_MASK;
	d->tex_cache_state |= offset >> 6;
	d->tex_cache_state |= HARDDOOM_TEX_CACHE_STATE_VALID;
	return true;
}

static uint32_t harddoom_tex_offset(HardDoomState *d, int x) {
	int height = HARDDOOM_CMD_EXTR_TEXTURE_HEIGHT(d->tex_dims);
	uint32_t coord = d->tex_column_state[x] & HARDDOOM_TEX_COORD_MASK;
	uint32_t offset = d->tex_column_offset[x] + (coord >> 16) % height;
	return offset & HARDDOOM_TEX_OFFSET_MASK;
}

static void harddoom_tex_step(HardDoomState *d, int x) {
	uint32_t coord = d->tex_column_state[x];
	coord += d->tex_column_step[x];
	coord &= HARDDOOM_TEX_COORD_MASK;
	d->tex_column_state[x] &= ~HARDDOOM_TEX_COORD_MASK;
	d->tex_column_state[x] |= coord;
}

/* Runs the TEX for some time.  Returns true if anything has been done.  */
static bool harddoom_run_tex(HardDoomState *d) {
	bool any = false;
	if (!(d->enable & HARDDOOM_ENABLE_TEX))
		return false;
	int x, h, pos;
	while (1) {
		if (!HARDDOOM_TEXCMD_EXTR_TYPE(d->tex_cmd)) {
			if (!harddoom_fifo_can_read(d, FIFO_FE2TEX))
				return any;
			d->tex_cmd = harddoom_fifo32_read(d, FIFO_FE2TEX);
			any = true;
		}
		switch (HARDDOOM_TEXCMD_EXTR_TYPE(d->tex_cmd)) {
		case HARDDOOM_TEXCMD_TYPE_TEXTURE_PT:
			harddoom_set_pt(d, TLB_TEXTURE, d->tex_cmd << 6);
			d->tex_cache_state &= ~HARDDOOM_TEX_CACHE_STATE_VALID;
			d->tex_cmd = 0;
			any = true;
			break;
		case HARDDOOM_TEXCMD_TYPE_TEXTURE_DIMS:
			d->tex_dims = d->tex_cmd & HARDDOOM_TEX_DIMS_MASK;
			d->tex_cmd = 0;
			any = true;
			break;
		case HARDDOOM_TEXCMD_TYPE_USTART:
			d->tex_ustart = HARDDOOM_CMD_EXTR_TEX_COORD(d->tex_cmd);
			d->tex_cmd = 0;
			any = true;
			break;
		case HARDDOOM_TEXCMD_TYPE_USTEP:
			d->tex_ustep = HARDDOOM_CMD_EXTR_TEX_COORD(d->tex_cmd);
			d->tex_cmd = 0;
			any = true;
			break;
		case HARDDOOM_TEXCMD_TYPE_START_COLUMN:
			x = HARDDOOM_TEXCMD_EXTR_START_X(d->tex_cmd);
			d->stats[HARDDOOM_STAT_TEX_COLUMN]++;
			d->tex_mask |= 1ull << x;
			d->tex_column_state[x] = d->tex_ustart;
			d->tex_column_step[x] = d->tex_ustep;
			d->tex_column_offset[x] = HARDDOOM_CMD_EXTR_COLUMN_OFFSET(d->tex_cmd);
			d->tex_cmd = 0;
			any = true;
			break;
		case HARDDOOM_TEXCMD_TYPE_END_COLUMN:
			x = HARDDOOM_TEXCMD_EXTR_END_X(d->tex_cmd);
			d->tex_mask &= ~(1ull << x);
			d->tex_cmd = 0;
			any = true;
			break;
		case HARDDOOM_TEXCMD_TYPE_DRAW_TEX:
			h = HARDDOOM_TEXCMD_EXTR_DRAW_HEIGHT(d->tex_cmd);
			x = HARDDOOM_TEXCMD_EXTR_DRAW_X(d->tex_cmd);
			while (h) {
				if (!harddoom_fifo_free(d, FIFO_TEX2OG))
					return any;
				int wptr = harddoom_fifo_wptr(d, FIFO_TEX2OG);
				if (d->tex_mask & 1ull << x) {
					int filled = d->tex_column_state[x] >> HARDDOOM_TEX_COLUMN_STATE_SPEC_SHIFT;
					uint8_t byte;
					pos = d->tex_cache_state >> HARDDOOM_TEX_CACHE_STATE_SPEC_POS_SHIFT;
					if (filled) {
						byte = d->tex_column_spec_data[x * HARDDOOM_TEX_COLUMN_SPEC_DATA_SIZE + pos];
						filled--;
					} else {
						uint32_t offset = harddoom_tex_offset(d, x);
						if (!harddoom_tex_fill_cache(d, offset))
							return any;
						byte = d->tex_cache[offset & 0x3f];
						harddoom_tex_step(d, x);
						while (filled < HARDDOOM_TEX_COLUMN_SPEC_DATA_SIZE) {
							pos++;
							pos &= 0xf;
							offset = harddoom_tex_offset(d, x);
							if (!harddoom_tex_cached(d, offset)) {
								d->stats[HARDDOOM_STAT_TEX_CACHE_SPEC_MISS]++;
								break;
							}
							d->stats[HARDDOOM_STAT_TEX_CACHE_SPEC_HIT]++;
							d->tex_column_spec_data[x * HARDDOOM_TEX_COLUMN_SPEC_DATA_SIZE + pos] = d->tex_cache[offset & 0x3f];
							filled++;
							harddoom_tex_step(d, x);
						}
					}
					d->tex_column_state[x] &= ~HARDDOOM_TEX_COLUMN_STATE_SPEC_MASK;
					d->tex_column_state[x] |= filled << HARDDOOM_TEX_COLUMN_STATE_SPEC_SHIFT;
					d->tex2og_data[wptr * HARDDOOM_BLOCK_SIZE + x] = byte;
					d->stats[HARDDOOM_STAT_TEX_PIXEL]++;
				}
				x++;
				if (x == HARDDOOM_BLOCK_SIZE) {
					d->tex2og_mask[wptr] = d->tex_mask;
					harddoom_fifo_write_manual(d, FIFO_TEX2OG);
					x = 0;
					h--;
					pos = d->tex_cache_state >> HARDDOOM_TEX_CACHE_STATE_SPEC_POS_SHIFT;
					pos++;
					d->tex_cache_state &= ~HARDDOOM_TEX_CACHE_STATE_SPEC_POS_MASK;
					d->tex_cache_state |= (pos << HARDDOOM_TEX_CACHE_STATE_SPEC_POS_SHIFT) & HARDDOOM_TEX_CACHE_STATE_SPEC_POS_MASK;
				}
				d->tex_cmd &= ~0x3ffff;
				d->tex_cmd |= x << 12 | h;
				any = true;
			}
			d->tex_cmd = 0;
			any = true;
			break;
		default:
			return any;
		}
	}
}

/* Runs the FLAT for some time.  Returns true if anything has been done.  */
static bool harddoom_run_flat(HardDoomState *d) {
	bool any = false;
	if (!(d->enable & HARDDOOM_ENABLE_FLAT))
		return false;
	int pos, num;
	while (1) {
		if (!HARDDOOM_FLCMD_EXTR_TYPE(d->flat_cmd)) {
			if (!harddoom_fifo_can_read(d, FIFO_FE2FLAT))
				return any;
			d->flat_cmd = harddoom_fifo32_read(d, FIFO_FE2FLAT);
			any = true;
		}
		switch (HARDDOOM_FLCMD_EXTR_TYPE(d->flat_cmd)) {
		case HARDDOOM_FLCMD_TYPE_FLAT_ADDR:
			d->stats[HARDDOOM_STAT_FLAT_REBIND]++;
			d->flat_addr = HARDDOOM_CMD_EXTR_FLAT_ADDR(d->flat_cmd);
			d->flat_cmd = 0;
			d->flat_cache_state &= ~HARDDOOM_FLAT_CACHE_STATE_VALID;
			any = true;
			break;
		case HARDDOOM_FLCMD_TYPE_USTART:
			d->flat_ucoord = HARDDOOM_FLCMD_EXTR_COORD(d->flat_cmd);
			d->flat_cmd = 0;
			any = true;
			break;
		case HARDDOOM_FLCMD_TYPE_VSTART:
			d->flat_vcoord = HARDDOOM_FLCMD_EXTR_COORD(d->flat_cmd);
			d->flat_cmd = 0;
			any = true;
			break;
		case HARDDOOM_FLCMD_TYPE_USTEP:
			d->flat_ustep = HARDDOOM_FLCMD_EXTR_COORD(d->flat_cmd);
			d->flat_cmd = 0;
			any = true;
			break;
		case HARDDOOM_FLCMD_TYPE_VSTEP:
			d->flat_vstep = HARDDOOM_FLCMD_EXTR_COORD(d->flat_cmd);
			d->flat_cmd = 0;
			any = true;
			break;
		case HARDDOOM_FLCMD_TYPE_READ_FLAT:
			num = HARDDOOM_FLCMD_EXTR_READ_HEIGHT(d->flat_cmd);
			pos = HARDDOOM_FLCMD_EXTR_READ_Y(d->flat_cmd);
			while (num) {
				if (!harddoom_fifo_free(d, FIFO_FLAT2OG))
					return any;
				int wptr = harddoom_fifo_write_manual(d, FIFO_FLAT2OG);
				harddoom_flat_fill_cache(d, pos);
				memcpy(&d->flat2og_data[wptr * HARDDOOM_BLOCK_SIZE], d->flat_cache, HARDDOOM_BLOCK_SIZE);
				num--;
				pos++;
				pos &= 0x3f;
				d->flat_cmd &= ~0x3ffff;
				d->flat_cmd |= pos << 12 | num;
				d->stats[HARDDOOM_STAT_FLAT_READ_BLOCK]++;
				any = true;
			}
			d->flat_cmd = 0;
			any = true;
			break;
		case HARDDOOM_FLCMD_TYPE_DRAW_SPAN:
			num = HARDDOOM_FLCMD_EXTR_DRAW_WIDTH(d->flat_cmd);
			pos = HARDDOOM_FLCMD_EXTR_DRAW_X(d->flat_cmd);
			while (num) {
				if (!harddoom_fifo_free(d, FIFO_FLAT2OG))
					return any;
				int wptr = harddoom_fifo_wptr(d, FIFO_FLAT2OG);
				int u = d->flat_ucoord >> 16;
				int v = d->flat_vcoord >> 16;
				d->flat_ucoord += d->flat_ustep;
				d->flat_vcoord += d->flat_vstep;
				d->flat_ucoord &= HARDDOOM_FLAT_COORD_MASK;
				d->flat_vcoord &= HARDDOOM_FLAT_COORD_MASK;
				harddoom_flat_fill_cache(d, v);
				d->flat2og_data[wptr * HARDDOOM_BLOCK_SIZE + pos] = d->flat_cache[u];
				num--;
				pos++;
				d->stats[HARDDOOM_STAT_FLAT_SPAN_PIXEL]++;
				if (pos == HARDDOOM_BLOCK_SIZE || num == 0) {
					harddoom_fifo_write_manual(d, FIFO_FLAT2OG);
					d->stats[HARDDOOM_STAT_FLAT_SPAN_BLOCK]++;
					pos = 0;
				}
				d->flat_cmd &= ~0x3ffff;
				d->flat_cmd |= pos << 12 | num;
				any = true;
			}
			d->flat_cmd = 0;
			any = true;
			break;
		default:
			return any;
		}
	}
}

/* Runs the FUZZ for some time.  Returns true if anything has been done.  */
static bool harddoom_run_fuzz(HardDoomState *d) {
	bool any = false;
	if (!(d->enable & HARDDOOM_ENABLE_FUZZ))
		return false;
	int x, pos, height;
	uint64_t mask;
	while (1) {
		if (!HARDDOOM_FZCMD_EXTR_TYPE(d->fuzz_cmd)) {
			if (!harddoom_fifo_can_read(d, FIFO_FE2FUZZ))
				return any;
			d->fuzz_cmd = harddoom_fifo32_read(d, FIFO_FE2FUZZ);
			any = true;
		}
		switch (HARDDOOM_FZCMD_EXTR_TYPE(d->fuzz_cmd)) {
		case HARDDOOM_FZCMD_TYPE_SET_COLUMN:
			x = HARDDOOM_FZCMD_EXTR_X(d->fuzz_cmd);
			pos = HARDDOOM_FZCMD_EXTR_POS(d->fuzz_cmd);
			d->fuzz_position[x] = pos;
			d->fuzz_cmd = 0;
			d->stats[HARDDOOM_STAT_FUZZ_COLUMN]++;
			any = true;
			break;
		case HARDDOOM_FZCMD_TYPE_DRAW_FUZZ:
			height = HARDDOOM_FZCMD_EXTR_HEIGHT(d->fuzz_cmd);
			while (height) {
				if (!harddoom_fifo_free(d, FIFO_FUZZ2OG))
					return any;
				mask = 0;
				int wptr = harddoom_fifo_write_manual(d, FIFO_FUZZ2OG);
				for (x = 0; x < HARDDOOM_BLOCK_SIZE; x++) {
					if (0x121e650de224aull >> d->fuzz_position[x]++ & 1)
						mask |= 1ull << x;
					d->fuzz_position[x] %= 50;
				}
				d->fuzz2og_mask[wptr] = mask;
				height--;
				d->fuzz_cmd &= ~0xfff;
				d->fuzz_cmd |= height;
				any = true;
			}
			d->fuzz_cmd = 0;
			any = true;
			break;
		default:
			return any;
		}
	}
}

static void harddoom_og_bump_pos(HardDoomState *d) {
	int pos = (d->og_misc & HARDDOOM_OG_MISC_BUF_POS_MASK) >> HARDDOOM_OG_MISC_BUF_POS_SHIFT;
	d->og_misc &= ~HARDDOOM_OG_MISC_BUF_POS_MASK;
	pos += HARDDOOM_BLOCK_SIZE;
	pos &= 0xff;
	d->og_misc |= pos << HARDDOOM_OG_MISC_BUF_POS_SHIFT;
}

static bool harddoom_og_buf_fill(HardDoomState *d) {
	int pos = (d->og_misc & HARDDOOM_OG_MISC_BUF_POS_MASK) >> HARDDOOM_OG_MISC_BUF_POS_SHIFT;
	uint8_t *src;
	int cmd = HARDDOOM_OGCMD_EXTR_TYPE(d->og_cmd);
	int flags = HARDDOOM_OGCMD_EXTR_FLAGS(d->og_cmd);
	int rptr;
	switch (cmd) {
		case HARDDOOM_OGCMD_TYPE_INIT_FUZZ:
		case HARDDOOM_OGCMD_TYPE_DRAW_FUZZ:
			pos += 0x40;
			pos &= 0xff;
			/* fallthru */
		case HARDDOOM_OGCMD_TYPE_COPY_H:
		case HARDDOOM_OGCMD_TYPE_COPY_V:
			if (!harddoom_fifo_can_read(d, FIFO_SR2OG))
				return false;
			rptr = harddoom_fifo_read_manual(d, FIFO_SR2OG);
			src = &d->sr2og_data[rptr * HARDDOOM_BLOCK_SIZE];
			break;
		case HARDDOOM_OGCMD_TYPE_DRAW_SPAN:
			if (!harddoom_fifo_can_read(d, FIFO_FLAT2OG))
				return false;
			rptr = harddoom_fifo_read_manual(d, FIFO_FLAT2OG);
			src = &d->flat2og_data[rptr * HARDDOOM_BLOCK_SIZE];
			break;
		case HARDDOOM_OGCMD_TYPE_DRAW_TEX:
			if (!harddoom_fifo_can_read(d, FIFO_TEX2OG))
				return false;
			rptr = harddoom_fifo_read_manual(d, FIFO_TEX2OG);
			src = &d->tex2og_data[rptr * HARDDOOM_BLOCK_SIZE];
			d->og_mask = d->tex2og_mask[rptr];
			break;
		default:
			return true;
	}
	if (flags) {
		int i, j;
		if (flags & HARDDOOM_OGCMD_FLAG_TRANSLATE)
			d->stats[HARDDOOM_STAT_OG_TRANSLATE_BLOCK]++;
		if (flags & HARDDOOM_OGCMD_FLAG_COLORMAP)
			d->stats[HARDDOOM_STAT_OG_COLORMAP_BLOCK]++;
		for (i = pos, j = 0; j < HARDDOOM_BLOCK_SIZE; i++, j++, i &= 0xff) {
			uint8_t byte = src[j];
			if (flags & HARDDOOM_OGCMD_FLAG_TRANSLATE)
				byte = d->og_translation[byte];
			if (flags & HARDDOOM_OGCMD_FLAG_COLORMAP)
				byte = d->og_colormap[byte];
			d->og_buf[i] = byte;
		}
	} else {
		if (pos + HARDDOOM_BLOCK_SIZE > sizeof d->og_buf) {
			int mid = sizeof d->og_buf - pos - HARDDOOM_BLOCK_SIZE;
			memcpy(d->og_buf + pos, src, mid);
			memcpy(d->og_buf, src + mid, HARDDOOM_BLOCK_SIZE - mid);
		} else {
			memcpy(d->og_buf + pos, src, HARDDOOM_BLOCK_SIZE);
		}
	}
	return true;
}

static bool harddoom_og_send(HardDoomState *d) {
	int cmd = HARDDOOM_OGCMD_EXTR_TYPE(d->og_cmd);
	int pos = (d->og_misc & HARDDOOM_OG_MISC_BUF_POS_MASK) >> HARDDOOM_OG_MISC_BUF_POS_SHIFT;
	pos &= 0xc0;
	if (!harddoom_fifo_free(d, FIFO_OG2SW))
		return false;
	int wptr = harddoom_fifo_write_manual(d, FIFO_OG2SW);
	memcpy(&d->og2sw_data[wptr * HARDDOOM_BLOCK_SIZE], d->og_buf + pos, HARDDOOM_BLOCK_SIZE);
	d->og2sw_mask[wptr] = d->og_mask;
	switch (cmd) {
	case HARDDOOM_OGCMD_TYPE_DRAW_BUF_H:
	case HARDDOOM_OGCMD_TYPE_DRAW_BUF_V:
		d->stats[HARDDOOM_STAT_OG_DRAW_BUF_BLOCK]++;
		d->stats[HARDDOOM_STAT_OG_DRAW_BUF_PIXEL] += ctpop64(d->og_mask);
		break;
	case HARDDOOM_OGCMD_TYPE_COPY_H:
	case HARDDOOM_OGCMD_TYPE_COPY_V:
		d->stats[HARDDOOM_STAT_OG_COPY_BLOCK]++;
		d->stats[HARDDOOM_STAT_OG_COPY_PIXEL] += ctpop64(d->og_mask);
		break;
	case HARDDOOM_OGCMD_TYPE_DRAW_FUZZ:
		d->stats[HARDDOOM_STAT_OG_FUZZ_PIXEL] += ctpop64(d->og_mask);
		break;
	}
	return true;
}

/* Runs the OG for some time.  Returns true if anything has been done.  */
static bool harddoom_run_og(HardDoomState *d) {
	bool any = false;
	if (!(d->enable & HARDDOOM_ENABLE_OG))
		return false;
	int x, w, h;
	int pos;
	int cmd;
	int state;
	int i;
	uint8_t *src;
	uint64_t mask;
	while (1) {
		if (!HARDDOOM_OGCMD_EXTR_TYPE(d->og_cmd)) {
			if (!harddoom_fifo_can_read(d, FIFO_FE2OG))
				return any;
			d->og_cmd = harddoom_fifo32_read(d, FIFO_FE2OG);
			any = true;
		}
		cmd = HARDDOOM_OGCMD_EXTR_TYPE(d->og_cmd);
		switch (cmd) {
		case HARDDOOM_OGCMD_TYPE_INTERLOCK:
			if (!harddoom_fifo_free(d, FIFO_OG2SW_C))
				return any;
			harddoom_fifo32_write(d, FIFO_OG2SW_C, HARDDOOM_SWCMD_INTERLOCK);
			d->og_cmd = 0;
			any = true;
			break;
		case HARDDOOM_OGCMD_TYPE_FENCE:
			if (!harddoom_fifo_free(d, FIFO_OG2SW_C))
				return any;
			harddoom_fifo32_write(d, FIFO_OG2SW_C, HARDDOOM_SWCMD_FENCE(HARDDOOM_CMD_EXTR_FENCE(d->og_cmd)));
			d->og_cmd = 0;
			any = true;
			break;
		case HARDDOOM_OGCMD_TYPE_PING:
			if (!harddoom_fifo_free(d, FIFO_OG2SW_C))
				return any;
			harddoom_fifo32_write(d, FIFO_OG2SW_C, HARDDOOM_SWCMD_PING);
			d->og_cmd = 0;
			any = true;
			break;
		case HARDDOOM_OGCMD_TYPE_FILL_COLOR:
			memset(d->og_buf, HARDDOOM_CMD_EXTR_FILL_COLOR(d->og_cmd), sizeof d->og_buf);
			d->og_cmd = 0;
			any = true;
			break;
		case HARDDOOM_OGCMD_TYPE_COLORMAP_ADDR:
			d->stats[HARDDOOM_STAT_OG_COLORMAP_FETCH]++;
			pci_dma_read(&d->dev, HARDDOOM_CMD_EXTR_COLORMAP_ADDR(d->og_cmd), d->og_colormap, sizeof d->og_colormap);
			d->og_cmd = 0;
			any = true;
			break;
		case HARDDOOM_OGCMD_TYPE_TRANSLATION_ADDR:
			d->stats[HARDDOOM_STAT_OG_TRANSLATION_FETCH]++;
			pci_dma_read(&d->dev, HARDDOOM_CMD_EXTR_COLORMAP_ADDR(d->og_cmd), d->og_translation, sizeof d->og_translation);
			d->og_cmd = 0;
			any = true;
			break;
		case HARDDOOM_OGCMD_TYPE_DRAW_BUF_H:
		case HARDDOOM_OGCMD_TYPE_COPY_H:
		case HARDDOOM_OGCMD_TYPE_DRAW_SPAN:
			/* The horizontal draw commands.  */
			x = HARDDOOM_OGCMD_EXTR_X(d->og_cmd);
			w = HARDDOOM_OGCMD_EXTR_H_WIDTH(d->og_cmd);
			while (w) {
				state = d->og_misc & HARDDOOM_OG_MISC_STATE_MASK;
				if (state == HARDDOOM_OG_MISC_STATE_INIT) {
					/* If this is the first iteration, initialize some things and tell the SW unit about the draw.  */
					int num = (w + x + 0x3f) >> 6;
					if (!harddoom_fifo_free(d, FIFO_OG2SW_C))
						return any;
					harddoom_fifo32_write(d, FIFO_OG2SW_C, HARDDOOM_SWCMD_DRAW(num));
					state = HARDDOOM_OG_MISC_STATE_RUNNING;
					d->og_misc &= ~HARDDOOM_OG_MISC_BUF_POS_MASK;
					if (cmd != HARDDOOM_OGCMD_TYPE_DRAW_BUF_H) {
						pos = 0;
						if (cmd == HARDDOOM_OGCMD_TYPE_COPY_H)
							pos = d->og_misc & HARDDOOM_OG_MISC_SRC_OFFSET_MASK;
						pos = x - pos;
						if (pos < 0) {
							state = HARDDOOM_OG_MISC_STATE_PREFILL;
							pos += HARDDOOM_BLOCK_SIZE;
						}
						d->og_misc |= pos << HARDDOOM_OG_MISC_BUF_POS_SHIFT;
					}
					d->og_misc &= ~HARDDOOM_OG_MISC_STATE_MASK;
					d->og_misc |= state;
					any = true;
				} else if (state == HARDDOOM_OG_MISC_STATE_PREFILL) {
					/* For some source alignments, we will need to pre-fill an extra source block.  */
					if (!harddoom_og_buf_fill(d))
						return any;
					harddoom_og_bump_pos(d);
					d->og_misc &= ~HARDDOOM_OG_MISC_STATE_MASK;
					d->og_misc |= HARDDOOM_OG_MISC_STATE_RUNNING;
					any = true;
				} else if (state == HARDDOOM_OG_MISC_STATE_RUNNING) {
					/* Now, fill the buffer, if we need to.  */
					pos = (d->og_misc & HARDDOOM_OG_MISC_BUF_POS_MASK) >> HARDDOOM_OG_MISC_BUF_POS_SHIFT;
					if (cmd != HARDDOOM_OGCMD_TYPE_DRAW_BUF_H && (pos & 0x3f) < x + w) {
						if (!harddoom_og_buf_fill(d))
							return any;
					}
					mask = ~0ull << x;
					if (x + w < HARDDOOM_BLOCK_SIZE)
						mask &= (1ull << (x + w)) - 1;
					d->og_mask = mask;
					d->og_misc &= ~HARDDOOM_OG_MISC_STATE_MASK;
					d->og_misc |= HARDDOOM_OG_MISC_STATE_FILLED;
					any = true;
				} else if (state == HARDDOOM_OG_MISC_STATE_FILLED) {
					/* Everything ready, try to submit to SW.  */
					if (!harddoom_og_send(d))
						return any;
					w -= (HARDDOOM_BLOCK_SIZE - x);
					if (w < 0)
						w = 0;
					x = 0;
					d->og_cmd &= ~0x3ffff;
					d->og_cmd |= w << 6;
					harddoom_og_bump_pos(d);
					d->og_misc &= ~HARDDOOM_OG_MISC_STATE_MASK;
					d->og_misc |= HARDDOOM_OG_MISC_STATE_RUNNING;
					any = true;
				} else {
					return any;
				}
			}
			/* Nothing left to do.  */
			d->og_cmd = 0;
			d->og_misc &= ~HARDDOOM_OG_MISC_STATE_MASK;
			any = true;
			break;
		case HARDDOOM_OGCMD_TYPE_DRAW_BUF_V:
		case HARDDOOM_OGCMD_TYPE_COPY_V:
			/* The vertical draw commands.  */
			x = HARDDOOM_OGCMD_EXTR_X(d->og_cmd);
			w = HARDDOOM_OGCMD_EXTR_V_WIDTH(d->og_cmd);
			h = HARDDOOM_OGCMD_EXTR_V_HEIGHT(d->og_cmd);
			while (h) {
				state = d->og_misc & HARDDOOM_OG_MISC_STATE_MASK;
				if (state == HARDDOOM_OG_MISC_STATE_INIT) {
					/* If this is the first iteration, initialize some things and tell the SW unit about the draw.  */
					if (!harddoom_fifo_free(d, FIFO_OG2SW_C))
						return any;
					harddoom_fifo32_write(d, FIFO_OG2SW_C, HARDDOOM_SWCMD_DRAW(h));
					d->og_misc &= ~HARDDOOM_OG_MISC_BUF_POS_MASK;
					pos = 0;
					if (cmd == HARDDOOM_OGCMD_TYPE_COPY_V)
						pos = d->og_misc & HARDDOOM_OG_MISC_SRC_OFFSET_MASK;
					pos = x - pos;
					if (pos < 0)
						pos += HARDDOOM_BLOCK_SIZE;
					d->og_misc |= pos << HARDDOOM_OG_MISC_BUF_POS_SHIFT;
					d->og_misc &= ~HARDDOOM_OG_MISC_STATE_MASK;
					d->og_misc |= HARDDOOM_OG_MISC_STATE_RUNNING;
					any = true;
				} else if (state == HARDDOOM_OG_MISC_STATE_RUNNING) {
					/* Now, fill the buffer, if we need to.  */
					if (cmd != HARDDOOM_OGCMD_TYPE_DRAW_BUF_V) {
						if (!harddoom_og_buf_fill(d))
							return any;
					}
					mask = ~0ull << x;
					if (x + w < HARDDOOM_BLOCK_SIZE)
						mask &= (1ull << (x + w)) - 1;
					d->og_mask = mask;
					d->og_misc &= ~HARDDOOM_OG_MISC_STATE_MASK;
					d->og_misc |= HARDDOOM_OG_MISC_STATE_FILLED;
					any = true;
				} else if (state == HARDDOOM_OG_MISC_STATE_FILLED) {
					/* Everything ready, try to submit to SW.  */
					if (!harddoom_og_send(d))
						return any;
					h--;
					d->og_cmd &= ~0xfff000;
					d->og_cmd |= h << 12;
					d->og_misc &= ~HARDDOOM_OG_MISC_STATE_MASK;
					d->og_misc |= HARDDOOM_OG_MISC_STATE_RUNNING;
					any = true;
				} else {
					return any;
				}
			}
			/* Nothing left to do.  */
			d->og_cmd = 0;
			d->og_misc &= ~HARDDOOM_OG_MISC_STATE_MASK;
			any = true;
			break;
		case HARDDOOM_OGCMD_TYPE_SRC_OFFSET:
			d->og_misc &= ~HARDDOOM_OG_MISC_SRC_OFFSET_MASK;
			d->og_misc |= HARDDOOM_OGCMD_EXTR_X(d->og_cmd);
			d->og_cmd = 0;
			any = true;
			break;
		case HARDDOOM_OGCMD_TYPE_READ_FLAT:
			if (!harddoom_fifo_can_read(d, FIFO_FLAT2OG))
				return any;
			src = &d->flat2og_data[harddoom_fifo_read_manual(d, FIFO_FLAT2OG) * HARDDOOM_BLOCK_SIZE];
			for (i = 0; i < 4; i++)
				memcpy(d->og_buf + i * HARDDOOM_BLOCK_SIZE, src, HARDDOOM_BLOCK_SIZE);
			d->og_cmd = 0;
			any = true;
			break;
		case HARDDOOM_OGCMD_TYPE_DRAW_FUZZ:
			h = HARDDOOM_OGCMD_EXTR_TF_HEIGHT(d->og_cmd);
			while (h) {
				state = d->og_misc & HARDDOOM_OG_MISC_STATE_MASK;
				if (state == HARDDOOM_OG_MISC_STATE_INIT) {
					/* If this is the first iteration, tell the SW unit about the draw.  */
					if (!harddoom_fifo_free(d, FIFO_OG2SW_C))
						return any;
					harddoom_fifo32_write(d, FIFO_OG2SW_C, HARDDOOM_SWCMD_DRAW(h));
					d->og_misc &= ~HARDDOOM_OG_MISC_STATE_MASK;
					d->og_misc |= HARDDOOM_OG_MISC_STATE_PREFILL;
					any = true;
				} else if (state == HARDDOOM_OG_MISC_STATE_PREFILL) {
					/* Fetch the mask from FUZZ.  */
					if (!harddoom_fifo_can_read(d, FIFO_FUZZ2OG))
						return any;
					int rptr = harddoom_fifo_read_manual(d, FIFO_FUZZ2OG);
					d->og_fuzz_mask = d->fuzz2og_mask[rptr];
					d->og_misc &= ~HARDDOOM_OG_MISC_STATE_MASK;
					d->og_misc |= HARDDOOM_OG_MISC_STATE_RUNNING;
					any = true;
				} else if (state == HARDDOOM_OG_MISC_STATE_RUNNING) {
					/* Now, fill the buffer.  */
					if (!harddoom_og_buf_fill(d))
						return any;
					/* And perform the fuzz.  */
					int i;
					pos = (d->og_misc & HARDDOOM_OG_MISC_BUF_POS_MASK) >> HARDDOOM_OG_MISC_BUF_POS_SHIFT;
					pos &= 0xc0;
					int prev = (pos - 0x40) & 0xc0;
					int next = (pos + 0x40) & 0xc0;
					for (i = 0; i < HARDDOOM_BLOCK_SIZE; i++) {
						if (d->og_mask & 1ull << i) {
							uint8_t byte;
							if (d->og_fuzz_mask & 1ull << i) {
								byte = d->og_buf[prev+i];
							} else {
								byte = d->og_buf[next+i];
							}
							d->og_buf[pos+i] = d->og_colormap[byte];
						}
					}
					d->og_misc &= ~HARDDOOM_OG_MISC_STATE_MASK;
					d->og_misc |= HARDDOOM_OG_MISC_STATE_FILLED;
					any = true;
				} else if (state == HARDDOOM_OG_MISC_STATE_FILLED) {
					/* Everything ready, try to submit to SW.  */
					if (!harddoom_og_send(d))
						return any;
					harddoom_og_bump_pos(d);
					h--;
					d->og_cmd &= ~0xfff;
					d->og_cmd |= h;
					d->og_misc &= ~HARDDOOM_OG_MISC_STATE_MASK;
					d->og_misc |= HARDDOOM_OG_MISC_STATE_PREFILL;
					any = true;
				} else {
					return any;
				}
			}
			/* Nothing left to do.  */
			d->og_cmd = 0;
			d->og_misc &= ~HARDDOOM_OG_MISC_STATE_MASK;
			any = true;
			break;
		case HARDDOOM_OGCMD_TYPE_INIT_FUZZ:
			while (1) {
				state = d->og_misc & HARDDOOM_OG_MISC_STATE_MASK;
				if (state == HARDDOOM_OG_MISC_STATE_INIT) {
					d->og_mask = 0;
					d->og_misc &= ~HARDDOOM_OG_MISC_BUF_POS_MASK;
					d->og_misc &= ~HARDDOOM_OG_MISC_STATE_MASK;
					d->og_misc |= HARDDOOM_OG_MISC_STATE_PREFILL;
				} else if (state == HARDDOOM_OG_MISC_STATE_PREFILL) {
					if (!harddoom_og_buf_fill(d))
						return any;
					harddoom_og_bump_pos(d);
					d->og_misc &= ~HARDDOOM_OG_MISC_STATE_MASK;
					d->og_misc |= HARDDOOM_OG_MISC_STATE_RUNNING;
					any = true;
				} else if (state == HARDDOOM_OG_MISC_STATE_RUNNING) {
					if (!harddoom_og_buf_fill(d))
						return any;
					harddoom_og_bump_pos(d);
					d->og_misc &= ~HARDDOOM_OG_MISC_STATE_MASK;
					d->og_cmd = 0;
					any = true;
					break;
				}
			}
			break;
		case HARDDOOM_OGCMD_TYPE_FUZZ_COLUMN:
			d->og_mask ^= 1ull << HARDDOOM_OGCMD_EXTR_X(d->og_cmd);
			d->og_cmd = 0;
			any = true;
			break;
		case HARDDOOM_OGCMD_TYPE_DRAW_TEX:
			h = HARDDOOM_OGCMD_EXTR_TF_HEIGHT(d->og_cmd);
			while (h) {
				state = d->og_misc & HARDDOOM_OG_MISC_STATE_MASK;
				if (state == HARDDOOM_OG_MISC_STATE_INIT) {
					/* If this is the first iteration, initialize some things and tell the SW unit about the draw.  */
					if (!harddoom_fifo_free(d, FIFO_OG2SW_C))
						return any;
					harddoom_fifo32_write(d, FIFO_OG2SW_C, HARDDOOM_SWCMD_DRAW(h));
					d->og_misc &= ~HARDDOOM_OG_MISC_BUF_POS_MASK;
					d->og_misc &= ~HARDDOOM_OG_MISC_STATE_MASK;
					d->og_misc |= HARDDOOM_OG_MISC_STATE_RUNNING;
					any = true;
				} else if (state == HARDDOOM_OG_MISC_STATE_RUNNING) {
					/* Now, fill the buffer.  */
					if (!harddoom_og_buf_fill(d))
						return any;
					d->og_misc &= ~HARDDOOM_OG_MISC_STATE_MASK;
					d->og_misc |= HARDDOOM_OG_MISC_STATE_FILLED;
					any = true;
				} else if (state == HARDDOOM_OG_MISC_STATE_FILLED) {
					/* Everything ready, try to submit to SW.  */
					if (!harddoom_og_send(d))
						return any;
					h--;
					d->og_cmd &= ~0xfff;
					d->og_cmd |= h;
					d->og_misc &= ~HARDDOOM_OG_MISC_STATE_MASK;
					d->og_misc |= HARDDOOM_OG_MISC_STATE_RUNNING;
					any = true;
				} else {
					return any;
				}
			}
			/* Nothing left to do.  */
			d->og_cmd = 0;
			d->og_misc &= ~HARDDOOM_OG_MISC_STATE_MASK;
			any = true;
			break;
		default:
			return any;
		}
	}
}

/* Runs the SW for some time.  Returns true if anything has been done.  */
static bool harddoom_run_sw(HardDoomState *d) {
	bool any = false;
	if (!(d->enable & HARDDOOM_ENABLE_SW))
		return false;
	while (1) {
		if (!HARDDOOM_SWCMD_EXTR_TYPE(d->sw_cmd)) {
			if (!harddoom_fifo_can_read(d, FIFO_OG2SW_C))
				return any;
			d->sw_cmd = harddoom_fifo32_read(d, FIFO_OG2SW_C);
			any = true;
		}
		switch (HARDDOOM_SWCMD_EXTR_TYPE(d->sw_cmd)) {
		case HARDDOOM_SWCMD_TYPE_INTERLOCK:
			if (d->sw2xy_state == HARDDOOM_SW2XY_STATE_MASK)
				return any;
			d->sw2xy_state++;
			d->sw_cmd = 0;
			any = true;
			break;
		case HARDDOOM_SWCMD_TYPE_FENCE:
			d->fence_last = HARDDOOM_CMD_EXTR_FENCE(d->sw_cmd);
			d->stats[HARDDOOM_STAT_SW_FENCE]++;
			if (d->fence_last == d->fence_wait) {
				d->stats[HARDDOOM_STAT_SW_FENCE_INTR]++;
				d->intr |= HARDDOOM_INTR_FENCE;
			}
			d->sw_cmd = 0;
			any = true;
			break;
		case HARDDOOM_SWCMD_TYPE_PING:
			d->intr |= HARDDOOM_INTR_PONG_SYNC;
			d->sw_cmd = 0;
			any = true;
			break;
		case HARDDOOM_SWCMD_TYPE_DRAW:
			if (!HARDDOOM_SWCMD_EXTR_BLOCKS(d->sw_cmd)) {
				/* We're done.  */
				d->sw_cmd = 0;
				any = true;
				break;
			}
			/* Otherwise, see if we have both data and address.  */
			if (!harddoom_fifo_can_read(d, FIFO_XY2SW))
				return any;
			if (!harddoom_fifo_can_read(d, FIFO_OG2SW))
				return any;
			uint32_t addr = harddoom_fifo32_read(d, FIFO_XY2SW);
			int rptr = harddoom_fifo_read_manual(d, FIFO_OG2SW);
			uint64_t mask = d->og2sw_mask[rptr];
			uint8_t *data = d->og2sw_data + rptr * HARDDOOM_BLOCK_SIZE;
			int pos = 0;
			while (mask) {
				if (!(mask & 1)) {
					/* Run of 0.  */
					int len = ctz64(mask);
					if (pos+len > 64) {
						abort();
					}
					pos += len;
					if (len == 64)
						break;
					mask >>= len;
				} else {
					/* Run of 1 -- draw.  */
					int len = 64;
					if (~mask)
						len = ctz64(~mask);
					d->stats[HARDDOOM_STAT_SW_XFER]++;
					d->stats[HARDDOOM_STAT_SW_PIXEL] += len;
					pci_dma_write(&d->dev, addr+pos, data+pos, len);
					if (pos+len > 64) {
						abort();
					}
					pos += len;
					if (len == 64)
						break;
					mask >>= len;
				}
			}
			d->sw_cmd--;
			any = true;
			break;
		default:
			return any;
		}
	}
}

static void *harddoom_thread(void *opaque)
{
	HardDoomState *d = opaque;
	qemu_mutex_lock(&d->mutex);
	while (1) {
		if (d->stopping)
			break;
		qemu_mutex_unlock(&d->mutex);
		qemu_mutex_lock_iothread();
		qemu_mutex_lock(&d->mutex);
		bool idle = true;
		if (harddoom_run_fetch_cmd(d))
			idle = false;
		if (harddoom_run_fe(d))
			idle = false;
		if (harddoom_run_xy(d))
			idle = false;
		if (harddoom_run_sr(d))
			idle = false;
		if (harddoom_run_tex(d))
			idle = false;
		if (harddoom_run_flat(d))
			idle = false;
		if (harddoom_run_fuzz(d))
			idle = false;
		if (harddoom_run_og(d))
			idle = false;
		if (harddoom_run_sw(d))
			idle = false;
		harddoom_status_update(d);
		qemu_mutex_unlock_iothread();
		if (idle) {
			qemu_cond_wait(&d->cond, &d->mutex);
		}
	}
	qemu_mutex_unlock(&d->mutex);
	return 0;
}

static int harddoom_init(PCIDevice *pci_dev)
{
	HardDoomState *d = DO_UPCAST(HardDoomState, dev, pci_dev);
	uint8_t *pci_conf = d->dev.config;

	pci_config_set_interrupt_pin(pci_conf, 1);

	memory_region_init_io(&d->mmio, OBJECT(d), &harddoom_mmio_ops, d, "harddoom", 0x1000);
	pci_register_bar(&d->dev, 0, PCI_BASE_ADDRESS_SPACE_MEMORY, &d->mmio);

	qemu_mutex_init(&d->mutex);
	qemu_cond_init(&d->cond);

	harddoom_power_reset(&pci_dev->qdev);

	d->stopping = false;
	qemu_thread_create(&d->thread, "harddoom", harddoom_thread,
			d, QEMU_THREAD_JOINABLE);

	return 0;
}

static void harddoom_exit(PCIDevice *pci_dev)
{
	HardDoomState *d = DO_UPCAST(HardDoomState, dev, pci_dev);
	qemu_mutex_lock(&d->mutex);
	d->stopping = true;
	qemu_mutex_unlock(&d->mutex);
	qemu_cond_signal(&d->cond);
	qemu_thread_join(&d->thread);
	qemu_cond_destroy(&d->cond);
	qemu_mutex_destroy(&d->mutex);
}

static Property harddoom_properties[] = {
	DEFINE_PROP_END_OF_LIST(),
};

static void harddoom_class_init(ObjectClass *klass, void *data)
{
	DeviceClass *dc = DEVICE_CLASS(klass);
	PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);

	k->init = harddoom_init;
	k->exit = harddoom_exit;
	k->vendor_id = HARDDOOM_VENDOR_ID;
	k->device_id = HARDDOOM_DEVICE_ID;
	k->class_id = PCI_CLASS_PROCESSOR_CO;
	dc->reset = harddoom_power_reset;
	dc->vmsd = &vmstate_harddoom;
	dc->props = harddoom_properties;
}

static InterfaceInfo harddoom_interfaces[] = {
	{ INTERFACE_CONVENTIONAL_PCI_DEVICE },
	{ },
};

static TypeInfo harddoom_info = {
	.name          = "harddoom",
	.parent        = TYPE_PCI_DEVICE,
	.instance_size = sizeof(HardDoomState),
	.class_init    = harddoom_class_init,
	.interfaces    = harddoom_interfaces,
};

static void harddoom_register_types(void)
{
	type_register_static(&harddoom_info);
}

type_init(harddoom_register_types)
