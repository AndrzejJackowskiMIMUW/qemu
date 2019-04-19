/*
 * HardDoom ][™ device
 *
 * Copyright (C) 2013-2018 Marcin Kościelnicki
 *
 * This work is licensed under the terms of the GNU GPL, version 2.  See
 * the COPYING file in the top-level directory.
 */

#include "harddoom2.h"
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
	/* These correspond directly to the registers listed in harddoom2.h.  */
	/* Main.  */
	uint32_t enable;
	uint32_t status;
	uint32_t intr;
	uint32_t intr_enable;
	uint32_t fence_counter;
	uint32_t fence_wait;
	/* CMD.  */
	uint32_t cmd_send[HARDDOOM2_CMD_SEND_SIZE];
	uint32_t cmd_pt;
	uint32_t cmd_size;
	uint32_t cmd_read_idx;
	uint32_t cmd_write_idx;
	/* TLB.  */
	uint32_t tlb_pt[HARDDOOM2_TLB_NUM];
	uint32_t tlb_entry[HARDDOOM2_TLB_NUM];
	uint32_t tlb_vaddr[HARDDOOM2_TLB_NUM];
	/* FE.  */
	uint32_t fe_code_addr;
	uint32_t fe_code[HARDDOOM2_FE_CODE_SIZE];
	uint32_t fe_data_addr;
	uint32_t fe_data[HARDDOOM2_FE_DATA_SIZE];
	uint32_t fe_error_code;
	uint32_t fe_state;
	uint32_t fe_reg[HARDDOOM2_FE_REG_NUM];
	/* FIFOs.  */
	uint8_t fifo_srout_data[HARDDOOM2_FIFO_SROUT_SIZE * HARDDOOM2_BLOCK_SIZE];
	uint8_t fifo_flatout_data[HARDDOOM2_FIFO_FLATOUT_SIZE * HARDDOOM2_BLOCK_SIZE];
	uint8_t fifo_texout_data[HARDDOOM2_FIFO_TEXOUT_SIZE * HARDDOOM2_BLOCK_SIZE];
	uint8_t fifo_ogout_data[HARDDOOM2_FIFO_OGOUT_SIZE * HARDDOOM2_BLOCK_SIZE];
	uint64_t fifo_texout_mask[HARDDOOM2_FIFO_TEXOUT_SIZE];
	uint64_t fifo_fuzzout_mask[HARDDOOM2_FIFO_FUZZOUT_SIZE];
	uint64_t fifo_ogout_mask[HARDDOOM2_FIFO_OGOUT_SIZE];
	uint32_t fifo_srout_state;
	uint32_t fifo_texout_state;
	uint32_t fifo_flatout_state;
	uint32_t fifo_fuzzout_state;
	uint32_t fifo_ogout_state;
	uint64_t fifo_xyoutr_data[HARDDOOM2_FIFO_XYOUTR_SIZE];
	uint64_t fifo_xyoutw_data[HARDDOOM2_FIFO_XYOUTW_SIZE];
	uint32_t fifo_xyoutr_state;
	uint32_t fifo_xyoutw_state;
	uint32_t fifo_xysync_state;
	uint32_t fifo_fecmd_state;
	uint32_t fifo_fecmd_data[HARDDOOM2_FIFO_FECMD_SIZE * HARDDOOM2_CMD_SEND_SIZE];
	uint32_t fifo_xycmd_cmd[HARDDOOM2_FIFO_XYCMD_SIZE];
	uint32_t fifo_xycmd_data[HARDDOOM2_FIFO_XYCMD_SIZE];
	uint32_t fifo_xycmd_state;
	uint32_t fifo_texcmd_cmd[HARDDOOM2_FIFO_TEXCMD_SIZE];
	uint32_t fifo_texcmd_data[HARDDOOM2_FIFO_TEXCMD_SIZE];
	uint32_t fifo_texcmd_state;
	uint32_t fifo_flatcmd_cmd[HARDDOOM2_FIFO_FLATCMD_SIZE];
	uint32_t fifo_flatcmd_data[HARDDOOM2_FIFO_FLATCMD_SIZE];
	uint32_t fifo_flatcmd_state;
	uint32_t fifo_fuzzcmd_cmd[HARDDOOM2_FIFO_FUZZCMD_SIZE];
	uint32_t fifo_fuzzcmd_data[HARDDOOM2_FIFO_FUZZCMD_SIZE];
	uint32_t fifo_fuzzcmd_state;
	uint32_t fifo_ogcmd_cmd[HARDDOOM2_FIFO_OGCMD_SIZE];
	uint32_t fifo_ogcmd_data[HARDDOOM2_FIFO_OGCMD_SIZE];
	uint32_t fifo_ogcmd_state;
	uint32_t fifo_swcmd_cmd[HARDDOOM2_FIFO_SWCMD_SIZE];
	uint32_t fifo_swcmd_data[HARDDOOM2_FIFO_SWCMD_SIZE];
	uint32_t fifo_swcmd_state;
	/* STATS.  */
	uint32_t stats[HARDDOOM2_STATS_NUM];
	/* XY.  */
	uint32_t xy_state;
	uint32_t xy_pending_data;
	uint32_t xy_dst_data;
	uint32_t xy_src_data;
	/* TEX.  */
	uint32_t tex_dims;
	uint32_t tex_ustart;
	uint32_t tex_ustep;
	uint32_t tex_draw;
	uint64_t tex_mask;
	uint32_t tex_cache_state;
	uint8_t tex_cache[HARDDOOM2_TEX_CACHE_SIZE];
	uint32_t tex_column_state[HARDDOOM2_BLOCK_SIZE];
	uint32_t tex_column_coord[HARDDOOM2_BLOCK_SIZE];
	uint32_t tex_column_step[HARDDOOM2_BLOCK_SIZE];
	uint8_t tex_column_spec_data[HARDDOOM2_BLOCK_SIZE * HARDDOOM2_TEX_COLUMN_SPEC_DATA_SIZE];
	/* FLAT.  */
	uint32_t flat_ucoord;
	uint32_t flat_vcoord;
	uint32_t flat_ustep;
	uint32_t flat_vstep;
	uint32_t flat_addr;
	uint32_t flat_draw;
	uint32_t flat_read;
	uint32_t flat_cache_state;
	uint8_t flat_cache[HARDDOOM2_FLAT_CACHE_SIZE];
	/* FUZZ.  */
	uint32_t fuzz_draw;
	uint8_t fuzz_position[HARDDOOM2_BLOCK_SIZE];
	/* OG.  */
	uint64_t og_mask;
	uint64_t og_fuzz_mask;
	uint32_t og_data;
	uint32_t og_state;
	uint8_t og_buf[HARDDOOM2_OG_BUF_SIZE];
	uint8_t og_translation[HARDDOOM2_COLORMAP_SIZE];
	uint8_t og_colormap[HARDDOOM2_COLORMAP_SIZE];
	/* SW.  */
	uint32_t sw_state;
	uint64_t sw_mask;
	uint64_t sw_addr;
	uint32_t sw_cache_state[HARDDOOM2_SW_CACHE_LINES];
	uint8_t sw_buf[HARDDOOM2_SW_BUF_SIZE];
	uint8_t sw_old[HARDDOOM2_SW_OLD_SIZE];
	uint8_t sw_cache[HARDDOOM2_SW_CACHE_LINES * HARDDOOM2_SW_CACHE_LINE_SIZE];
} HardDoom2State;

static const VMStateDescription vmstate_harddoom2 = {
	.name = "harddoom2",
	.version_id = 3,
	.minimum_version_id = 3,
	.minimum_version_id_old = 3,
	.fields = (VMStateField[]) {
		VMSTATE_PCI_DEVICE(dev, HardDoom2State),
		/* Main.  */
		VMSTATE_UINT32(enable, HardDoom2State),
		VMSTATE_UINT32(status, HardDoom2State),
		VMSTATE_UINT32(intr, HardDoom2State),
		VMSTATE_UINT32(intr_enable, HardDoom2State),
		VMSTATE_UINT32(fence_counter, HardDoom2State),
		VMSTATE_UINT32(fence_wait, HardDoom2State),
		/* CMD.  */
		VMSTATE_UINT32_ARRAY(cmd_send, HardDoom2State, HARDDOOM2_CMD_SEND_SIZE),
		VMSTATE_UINT32(cmd_pt, HardDoom2State),
		VMSTATE_UINT32(cmd_size, HardDoom2State),
		VMSTATE_UINT32(cmd_read_idx, HardDoom2State),
		VMSTATE_UINT32(cmd_write_idx, HardDoom2State),
		/* TLB.  */
		VMSTATE_UINT32_ARRAY(tlb_pt, HardDoom2State, HARDDOOM2_TLB_NUM),
		VMSTATE_UINT32_ARRAY(tlb_entry, HardDoom2State, HARDDOOM2_TLB_NUM),
		VMSTATE_UINT32_ARRAY(tlb_vaddr, HardDoom2State, HARDDOOM2_TLB_NUM),
		/* FE.  */
		VMSTATE_UINT32(fe_code_addr, HardDoom2State),
		VMSTATE_UINT32_ARRAY(fe_code, HardDoom2State, HARDDOOM2_FE_CODE_SIZE),
		VMSTATE_UINT32(fe_data_addr, HardDoom2State),
		VMSTATE_UINT32_ARRAY(fe_data, HardDoom2State, HARDDOOM2_FE_DATA_SIZE),
		VMSTATE_UINT32(fe_error_code, HardDoom2State),
		VMSTATE_UINT32(fe_state, HardDoom2State),
		VMSTATE_UINT32_ARRAY(fe_reg, HardDoom2State, HARDDOOM2_FE_REG_NUM),
		/* FIFOs.  */
		VMSTATE_UINT8_ARRAY(fifo_srout_data, HardDoom2State, HARDDOOM2_FIFO_SROUT_SIZE * HARDDOOM2_BLOCK_SIZE),
		VMSTATE_UINT8_ARRAY(fifo_flatout_data, HardDoom2State, HARDDOOM2_FIFO_FLATOUT_SIZE * HARDDOOM2_BLOCK_SIZE),
		VMSTATE_UINT8_ARRAY(fifo_texout_data, HardDoom2State, HARDDOOM2_FIFO_TEXOUT_SIZE * HARDDOOM2_BLOCK_SIZE),
		VMSTATE_UINT8_ARRAY(fifo_ogout_data, HardDoom2State, HARDDOOM2_FIFO_OGOUT_SIZE * HARDDOOM2_BLOCK_SIZE),
		VMSTATE_UINT64_ARRAY(fifo_texout_mask, HardDoom2State, HARDDOOM2_FIFO_TEXOUT_SIZE),
		VMSTATE_UINT64_ARRAY(fifo_fuzzout_mask, HardDoom2State, HARDDOOM2_FIFO_FUZZOUT_SIZE),
		VMSTATE_UINT64_ARRAY(fifo_ogout_mask, HardDoom2State, HARDDOOM2_FIFO_OGOUT_SIZE),
		VMSTATE_UINT32(fifo_srout_state, HardDoom2State),
		VMSTATE_UINT32(fifo_texout_state, HardDoom2State),
		VMSTATE_UINT32(fifo_flatout_state, HardDoom2State),
		VMSTATE_UINT32(fifo_fuzzout_state, HardDoom2State),
		VMSTATE_UINT32(fifo_ogout_state, HardDoom2State),
		VMSTATE_UINT64_ARRAY(fifo_xyoutr_data, HardDoom2State, HARDDOOM2_FIFO_XYOUTR_SIZE),
		VMSTATE_UINT64_ARRAY(fifo_xyoutw_data, HardDoom2State, HARDDOOM2_FIFO_XYOUTW_SIZE),
		VMSTATE_UINT32(fifo_xyoutr_state, HardDoom2State),
		VMSTATE_UINT32(fifo_xyoutw_state, HardDoom2State),
		VMSTATE_UINT32(fifo_xysync_state, HardDoom2State),
		VMSTATE_UINT32(fifo_fecmd_state, HardDoom2State),
		VMSTATE_UINT32_ARRAY(fifo_fecmd_data, HardDoom2State, HARDDOOM2_FIFO_FECMD_SIZE * HARDDOOM2_CMD_SEND_SIZE),
		VMSTATE_UINT32_ARRAY(fifo_xycmd_cmd, HardDoom2State, HARDDOOM2_FIFO_XYCMD_SIZE),
		VMSTATE_UINT32_ARRAY(fifo_xycmd_data, HardDoom2State, HARDDOOM2_FIFO_XYCMD_SIZE),
		VMSTATE_UINT32(fifo_xycmd_state, HardDoom2State),
		VMSTATE_UINT32_ARRAY(fifo_texcmd_cmd, HardDoom2State, HARDDOOM2_FIFO_TEXCMD_SIZE),
		VMSTATE_UINT32_ARRAY(fifo_texcmd_data, HardDoom2State, HARDDOOM2_FIFO_TEXCMD_SIZE),
		VMSTATE_UINT32(fifo_texcmd_state, HardDoom2State),
		VMSTATE_UINT32_ARRAY(fifo_flatcmd_cmd, HardDoom2State, HARDDOOM2_FIFO_FLATCMD_SIZE),
		VMSTATE_UINT32_ARRAY(fifo_flatcmd_data, HardDoom2State, HARDDOOM2_FIFO_FLATCMD_SIZE),
		VMSTATE_UINT32(fifo_flatcmd_state, HardDoom2State),
		VMSTATE_UINT32_ARRAY(fifo_fuzzcmd_cmd, HardDoom2State, HARDDOOM2_FIFO_FUZZCMD_SIZE),
		VMSTATE_UINT32_ARRAY(fifo_fuzzcmd_data, HardDoom2State, HARDDOOM2_FIFO_FUZZCMD_SIZE),
		VMSTATE_UINT32(fifo_fuzzcmd_state, HardDoom2State),
		VMSTATE_UINT32_ARRAY(fifo_ogcmd_cmd, HardDoom2State, HARDDOOM2_FIFO_OGCMD_SIZE),
		VMSTATE_UINT32_ARRAY(fifo_ogcmd_data, HardDoom2State, HARDDOOM2_FIFO_OGCMD_SIZE),
		VMSTATE_UINT32(fifo_ogcmd_state, HardDoom2State),
		VMSTATE_UINT32_ARRAY(fifo_swcmd_cmd, HardDoom2State, HARDDOOM2_FIFO_SWCMD_SIZE),
		VMSTATE_UINT32_ARRAY(fifo_swcmd_data, HardDoom2State, HARDDOOM2_FIFO_SWCMD_SIZE),
		VMSTATE_UINT32(fifo_swcmd_state, HardDoom2State),
		/* STATS.  */
		VMSTATE_UINT32_ARRAY(stats, HardDoom2State, HARDDOOM2_STATS_NUM),
		/* XY.  */
		VMSTATE_UINT32(xy_state, HardDoom2State),
		VMSTATE_UINT32(xy_pending_data, HardDoom2State),
		VMSTATE_UINT32(xy_dst_data, HardDoom2State),
		VMSTATE_UINT32(xy_src_data, HardDoom2State),
		/* TEX.  */
		VMSTATE_UINT32(tex_dims, HardDoom2State),
		VMSTATE_UINT32(tex_ustart, HardDoom2State),
		VMSTATE_UINT32(tex_ustep, HardDoom2State),
		VMSTATE_UINT32(tex_draw, HardDoom2State),
		VMSTATE_UINT64(tex_mask, HardDoom2State),
		VMSTATE_UINT32(tex_cache_state, HardDoom2State),
		VMSTATE_UINT8_ARRAY(tex_cache, HardDoom2State, HARDDOOM2_TEX_CACHE_SIZE),
		VMSTATE_UINT32_ARRAY(tex_column_state, HardDoom2State, HARDDOOM2_BLOCK_SIZE),
		VMSTATE_UINT32_ARRAY(tex_column_coord, HardDoom2State, HARDDOOM2_BLOCK_SIZE),
		VMSTATE_UINT32_ARRAY(tex_column_step, HardDoom2State, HARDDOOM2_BLOCK_SIZE),
		VMSTATE_UINT8_ARRAY(tex_column_spec_data, HardDoom2State, HARDDOOM2_BLOCK_SIZE * HARDDOOM2_TEX_COLUMN_SPEC_DATA_SIZE),
		/* FLAT.  */
		VMSTATE_UINT32(flat_ucoord, HardDoom2State),
		VMSTATE_UINT32(flat_vcoord, HardDoom2State),
		VMSTATE_UINT32(flat_ustep, HardDoom2State),
		VMSTATE_UINT32(flat_vstep, HardDoom2State),
		VMSTATE_UINT32(flat_addr, HardDoom2State),
		VMSTATE_UINT32(flat_draw, HardDoom2State),
		VMSTATE_UINT32(flat_read, HardDoom2State),
		VMSTATE_UINT32(flat_cache_state, HardDoom2State),
		VMSTATE_UINT8_ARRAY(flat_cache, HardDoom2State, HARDDOOM2_FLAT_CACHE_SIZE),
		/* FUZZ.  */
		VMSTATE_UINT32(fuzz_draw, HardDoom2State),
		VMSTATE_UINT8_ARRAY(fuzz_position, HardDoom2State, HARDDOOM2_BLOCK_SIZE),
		/* OG.  */
		VMSTATE_UINT64(og_mask, HardDoom2State),
		VMSTATE_UINT64(og_fuzz_mask, HardDoom2State),
		VMSTATE_UINT32(og_data, HardDoom2State),
		VMSTATE_UINT32(og_state, HardDoom2State),
		VMSTATE_UINT8_ARRAY(og_buf, HardDoom2State, HARDDOOM2_OG_BUF_SIZE),
		VMSTATE_UINT8_ARRAY(og_translation, HardDoom2State, HARDDOOM2_COLORMAP_SIZE),
		VMSTATE_UINT8_ARRAY(og_colormap, HardDoom2State, HARDDOOM2_COLORMAP_SIZE),
		/* SW.  */
		VMSTATE_UINT32(sw_state, HardDoom2State),
		VMSTATE_UINT64(sw_mask, HardDoom2State),
		VMSTATE_UINT64(sw_addr, HardDoom2State),
		VMSTATE_UINT8_ARRAY(sw_buf, HardDoom2State, HARDDOOM2_SW_BUF_SIZE),
		VMSTATE_UINT8_ARRAY(sw_old, HardDoom2State, HARDDOOM2_SW_OLD_SIZE),
		VMSTATE_UINT32_ARRAY(sw_cache_state, HardDoom2State, HARDDOOM2_SW_CACHE_LINES),
		VMSTATE_UINT8_ARRAY(sw_cache, HardDoom2State, HARDDOOM2_SW_CACHE_LINES * HARDDOOM2_SW_CACHE_LINE_SIZE),
		VMSTATE_END_OF_LIST()
	}
};

enum {
	REG_TYPE_SIMPLE_32,
	REG_TYPE_WINDOW_32,
	REG_TYPE_SIMPLE_8,
	REG_TYPE_MASK,
};

#define REG(a, b, c) {a, offsetof(HardDoom2State, b), 0, c, 1, REG_TYPE_SIMPLE_32, #a}
#define ARRAY(a, b, c, n) {a, offsetof(HardDoom2State, b), 0, c, n, REG_TYPE_SIMPLE_32, #a}
#define BYTE_ARRAY(a, b, c, n) {a, offsetof(HardDoom2State, b), 0, c, n, REG_TYPE_SIMPLE_8, #a}
#define WINDOW(a, b, i, c, n) {a, offsetof(HardDoom2State, b), offsetof(HardDoom2State, i), c, n, REG_TYPE_WINDOW_32, #a}
#define MASK(a, b) {a, offsetof(HardDoom2State, b), 0, 0, 1, REG_TYPE_MASK, #a}
static struct harddoom2_register {
	uint32_t bar_off;
	ptrdiff_t vm_off;
	ptrdiff_t vm_off_idx;
	uint32_t mask;
	int num;
	int type;
	const char *name;
} harddoom2_registers[] = {
	/* Main.  */
	REG(HARDDOOM2_ENABLE, enable, HARDDOOM2_ENABLE_ALL),
	/* Special RO: STATUS.  */
	/* Special WO: RESET.  */
	/* Special RW-ish: INTR.  */
	REG(HARDDOOM2_INTR_ENABLE, intr_enable, HARDDOOM2_INTR_MASK),
	REG(HARDDOOM2_FENCE_COUNTER, fence_counter, ~0),
	REG(HARDDOOM2_FENCE_WAIT, fence_wait, ~0),
	/* CMD.  */
	/* Last index has special behavior on write, in addition to setting state.  */
	ARRAY(HARDDOOM2_CMD_SEND(0), cmd_send, ~0, HARDDOOM2_CMD_SEND_SIZE),
	/* Has special behavior on write, in addition to setting state.  */
	REG(HARDDOOM2_CMD_PT, cmd_pt, ~0),
	REG(HARDDOOM2_CMD_SIZE, cmd_size, HARDDOOM2_CMD_SIZE_MASK),
	REG(HARDDOOM2_CMD_READ_IDX, cmd_read_idx, HARDDOOM2_CMD_IDX_MASK),
	REG(HARDDOOM2_CMD_WRITE_IDX, cmd_write_idx, HARDDOOM2_CMD_IDX_MASK),
	/* Special RO: CMD_FREE.  */
	/* TLB.  */
	ARRAY(HARDDOOM2_TLB_PT(0), tlb_pt, ~0, HARDDOOM2_TLB_NUM),
	ARRAY(HARDDOOM2_TLB_ENTRY(0), tlb_entry, HARDDOOM2_TLB_ENTRY_MASK, HARDDOOM2_TLB_NUM),
	ARRAY(HARDDOOM2_TLB_VADDR(0), tlb_vaddr, HARDDOOM2_TLB_VADDR_MASK, HARDDOOM2_TLB_NUM),
	/* FE.  */
	REG(HARDDOOM2_FE_CODE_ADDR, fe_code_addr, HARDDOOM2_FE_CODE_SIZE - 1),
	WINDOW(HARDDOOM2_FE_CODE_WINDOW, fe_code, fe_code_addr, 0x3fffffff, HARDDOOM2_FE_CODE_SIZE),
	REG(HARDDOOM2_FE_DATA_ADDR, fe_data_addr, HARDDOOM2_FE_DATA_SIZE - 1),
	WINDOW(HARDDOOM2_FE_DATA_WINDOW, fe_data, fe_data_addr, ~0, HARDDOOM2_FE_DATA_SIZE),
	REG(HARDDOOM2_FE_ERROR_CODE, fe_error_code, HARDDOOM2_FE_ERROR_CODE_MASK),
	REG(HARDDOOM2_FE_STATE, fe_state, HARDDOOM2_FE_STATE_MASK),
	ARRAY(HARDDOOM2_FE_REG(0), fe_reg, ~0, HARDDOOM2_FE_REG_NUM),
	/* Most FIFOs are handled below.  */
	REG(HARDDOOM2_FIFO_XYSYNC_STATE, fifo_xysync_state, HARDDOOM2_FIFO_XYSYNC_STATE_MASK),
	/* STATS.  */
	ARRAY(HARDDOOM2_STATS(0), stats, ~0, HARDDOOM2_STATS_NUM),
	/* XY.  */
	REG(HARDDOOM2_XY_STATE, xy_state, HARDDOOM2_XY_STATE_MASK),
	REG(HARDDOOM2_XY_PENDING_DATA, xy_pending_data, ~0),
	REG(HARDDOOM2_XY_DST_DATA, xy_dst_data, ~0),
	REG(HARDDOOM2_XY_SRC_DATA, xy_src_data, ~0),
	/* TEX.  */
	REG(HARDDOOM2_TEX_DIMS, tex_dims, HARDDOOM2_TEX_DIMS_MASK),
	REG(HARDDOOM2_TEX_USTART, tex_ustart, ~0),
	REG(HARDDOOM2_TEX_USTEP, tex_ustep, ~0),
	REG(HARDDOOM2_TEX_DRAW, tex_draw, HARDDOOM2_TEX_DRAW_MASK),
	MASK(HARDDOOM2_TEX_MASK, tex_mask),
	REG(HARDDOOM2_TEX_CACHE_STATE, tex_cache_state, HARDDOOM2_TEX_CACHE_STATE_MASK),
	BYTE_ARRAY(HARDDOOM2_TEX_CACHE, tex_cache, 0xff, HARDDOOM2_TEX_CACHE_SIZE),
	ARRAY(HARDDOOM2_TEX_COLUMN_STATE(0), tex_column_state, HARDDOOM2_TEX_COLUMN_STATE_MASK, HARDDOOM2_BLOCK_SIZE),
	ARRAY(HARDDOOM2_TEX_COLUMN_COORD(0), tex_column_coord, ~0, HARDDOOM2_BLOCK_SIZE),
	ARRAY(HARDDOOM2_TEX_COLUMN_STEP(0), tex_column_step, ~0, HARDDOOM2_BLOCK_SIZE),
	BYTE_ARRAY(HARDDOOM2_TEX_COLUMN_SPEC_DATA(0), tex_column_spec_data, 0xff, HARDDOOM2_BLOCK_SIZE * HARDDOOM2_TEX_COLUMN_SPEC_DATA_SIZE),
	/* FLAT. */
	REG(HARDDOOM2_FLAT_UCOORD, flat_ucoord, HARDDOOM2_FLAT_COORD_MASK),
	REG(HARDDOOM2_FLAT_VCOORD, flat_vcoord, HARDDOOM2_FLAT_COORD_MASK),
	REG(HARDDOOM2_FLAT_USTEP, flat_ustep, HARDDOOM2_FLAT_COORD_MASK),
	REG(HARDDOOM2_FLAT_VSTEP, flat_vstep, HARDDOOM2_FLAT_COORD_MASK),
	REG(HARDDOOM2_FLAT_ADDR, flat_addr, HARDDOOM2_FLAT_ADDR_MASK),
	REG(HARDDOOM2_FLAT_DRAW, flat_draw, HARDDOOM2_FLAT_DRAW_MASK),
	REG(HARDDOOM2_FLAT_READ, flat_read, HARDDOOM2_FLAT_READ_MASK),
	REG(HARDDOOM2_FLAT_CACHE_STATE, flat_cache_state, HARDDOOM2_FLAT_CACHE_STATE_MASK),
	BYTE_ARRAY(HARDDOOM2_FLAT_CACHE, flat_cache, 0xff, HARDDOOM2_FLAT_CACHE_SIZE),
	/* FUZZ.  */
	REG(HARDDOOM2_FUZZ_DRAW, fuzz_draw, HARDDOOM2_FUZZ_DRAW_MASK),
	BYTE_ARRAY(HARDDOOM2_FUZZ_POSITION, fuzz_position, 0x3f, HARDDOOM2_BLOCK_SIZE),
	/* OG.  */
	MASK(HARDDOOM2_OG_MASK, og_mask),
	MASK(HARDDOOM2_OG_FUZZ_MASK, og_fuzz_mask),
	REG(HARDDOOM2_OG_DATA, og_data, ~0),
	REG(HARDDOOM2_OG_STATE, og_state, HARDDOOM2_OG_STATE_MASK),
	BYTE_ARRAY(HARDDOOM2_OG_BUF, og_buf, 0xff, HARDDOOM2_OG_BUF_SIZE),
	BYTE_ARRAY(HARDDOOM2_OG_TRANSLATION, og_translation, 0xff, HARDDOOM2_COLORMAP_SIZE),
	BYTE_ARRAY(HARDDOOM2_OG_COLORMAP, og_colormap, 0xff, HARDDOOM2_COLORMAP_SIZE),
	/* SW.  */
	REG(HARDDOOM2_SW_STATE, sw_state, HARDDOOM2_SW_STATE_MASK),
	MASK(HARDDOOM2_SW_MASK, sw_mask),
	MASK(HARDDOOM2_SW_ADDR, sw_addr),
	BYTE_ARRAY(HARDDOOM2_SW_BUF, sw_buf, 0xff, HARDDOOM2_SW_BUF_SIZE),
	BYTE_ARRAY(HARDDOOM2_SW_OLD, sw_old, 0xff, HARDDOOM2_SW_OLD_SIZE),
	ARRAY(HARDDOOM2_SW_CACHE_STATE(0), sw_cache_state, HARDDOOM2_SW_CACHE_STATE_MASK, HARDDOOM2_SW_CACHE_LINES),
	BYTE_ARRAY(HARDDOOM2_SW_CACHE(0), sw_cache, 0xff, HARDDOOM2_SW_CACHE_LINES * HARDDOOM2_SW_CACHE_LINE_SIZE),
};

enum {
	FIFO_FECMD,
	FIFO_XYCMD,
	FIFO_TEXCMD,
	FIFO_FLATCMD,
	FIFO_FUZZCMD,
	FIFO_OGCMD,
	FIFO_SWCMD,
	FIFO_XYOUTR,
	FIFO_XYOUTW,
	FIFO_SROUT,
	FIFO_TEXOUT,
	FIFO_FLATOUT,
	FIFO_FUZZOUT,
	FIFO_OGOUT,
};

enum {
	FIFO_TYPE_FECMD,
	FIFO_TYPE_CMD,
	FIFO_TYPE_ADDR,
	FIFO_TYPE_BLOCK,
	FIFO_TYPE_BLOCK_MASK,
	FIFO_TYPE_MASK,
};

#define FIFO_FECMD(a, b) { \
	HARDDOOM2_FIFO_ ## a ## _STATE, \
	HARDDOOM2_FIFO_ ## a ## _DATA_WINDOW, \
	0, \
	offsetof(HardDoom2State, fifo_ ## b ## _state), \
	offsetof(HardDoom2State, fifo_ ## b ## _data), \
	0, \
	FIFO_TYPE_FECMD, \
	HARDDOOM2_FIFO_ ## a ## _SIZE, \
	#a, \
	HARDDOOM2_RESET_FIFO_ ## a, \
	HARDDOOM2_STATUS_FIFO_ ## a, \
	HARDDOOM2_STAT_FIFO_ ## a, \
	-1, \
}
#define FIFO_CMD(a, b) { \
	HARDDOOM2_FIFO_ ## a ## _STATE, \
	HARDDOOM2_FIFO_ ## a ## _CMD_WINDOW, \
	HARDDOOM2_FIFO_ ## a ## _DATA_WINDOW, \
	offsetof(HardDoom2State, fifo_ ## b ## _state), \
	offsetof(HardDoom2State, fifo_ ## b ## _cmd), \
	offsetof(HardDoom2State, fifo_ ## b ## _data), \
	FIFO_TYPE_CMD, \
	HARDDOOM2_FIFO_ ## a ## _SIZE, \
	#a, \
	HARDDOOM2_RESET_FIFO_ ## a, \
	HARDDOOM2_STATUS_FIFO_ ## a, \
	HARDDOOM2_STAT_FIFO_ ## a, \
	-1, \
}
#define FIFO_ADDR(a, b) { \
	HARDDOOM2_FIFO_ ## a ## _STATE, \
	HARDDOOM2_FIFO_ ## a ## _DATA_WINDOW, \
	0, \
	offsetof(HardDoom2State, fifo_ ## b ## _state), \
	offsetof(HardDoom2State, fifo_ ## b ## _data), \
	0, \
	FIFO_TYPE_ADDR, \
	HARDDOOM2_FIFO_ ## a ## _SIZE, \
	#a, \
	HARDDOOM2_RESET_FIFO_ ## a, \
	HARDDOOM2_STATUS_FIFO_ ## a, \
	-1, \
	-1, \
}

#define FIFO_BLOCK(a, b) { \
	HARDDOOM2_FIFO_ ## a ## _STATE, \
	HARDDOOM2_FIFO_ ## a ## _DATA_WINDOW, \
	0, \
	offsetof(HardDoom2State, fifo_ ## b ## _state), \
	offsetof(HardDoom2State, fifo_ ## b ## _data), \
	0, \
	FIFO_TYPE_BLOCK, \
	HARDDOOM2_FIFO_ ## a ## _SIZE, \
	#a, \
	HARDDOOM2_RESET_FIFO_ ## a, \
	HARDDOOM2_STATUS_FIFO_ ## a, \
	HARDDOOM2_STAT_FIFO_ ## a, \
	-1, \
}
#define FIFO_BLOCK_MASK(a, b) { \
	HARDDOOM2_FIFO_ ## a ## _STATE, \
	HARDDOOM2_FIFO_ ## a ## _DATA_WINDOW, \
	HARDDOOM2_FIFO_ ## a ## _MASK_WINDOW, \
	offsetof(HardDoom2State, fifo_ ## b ## _state), \
	offsetof(HardDoom2State, fifo_ ## b ## _data), \
	offsetof(HardDoom2State, fifo_ ## b ## _mask), \
	FIFO_TYPE_BLOCK_MASK, \
	HARDDOOM2_FIFO_ ## a ## _SIZE, \
	#a, \
	HARDDOOM2_RESET_FIFO_ ## a, \
	HARDDOOM2_STATUS_FIFO_ ## a, \
	HARDDOOM2_STAT_FIFO_ ## a, \
	HARDDOOM2_STAT_FIFO_ ## a ## _PIXEL, \
}
#define FIFO_MASK(a, b) { \
	HARDDOOM2_FIFO_ ## a ## _STATE, \
	0, \
	HARDDOOM2_FIFO_ ## a ## _MASK_WINDOW, \
	offsetof(HardDoom2State, fifo_ ## b ## _state), \
	0, \
	offsetof(HardDoom2State, fifo_ ## b ## _mask), \
	FIFO_TYPE_MASK, \
	HARDDOOM2_FIFO_ ## a ## _SIZE, \
	#a, \
	HARDDOOM2_RESET_FIFO_ ## a, \
	HARDDOOM2_STATUS_FIFO_ ## a, \
	HARDDOOM2_STAT_FIFO_ ## a, \
	-1, \
}
static struct harddoom2_fifo {
	uint32_t bar_off_state;
	uint32_t bar_off_window_a;
	uint32_t bar_off_window_b;
	ptrdiff_t vm_off_state;
	ptrdiff_t vm_off_window_a;
	ptrdiff_t vm_off_window_b;
	int type;
	uint32_t size;
	const char *name;
	uint32_t reset;
	uint32_t status;
	int stats_idx;
	int stats_idx_pixel;
} harddoom2_fifos[] = {
	[FIFO_FECMD] = FIFO_FECMD(FECMD, fecmd),
	[FIFO_XYCMD] = FIFO_CMD(XYCMD, xycmd),
	[FIFO_TEXCMD] = FIFO_CMD(TEXCMD, texcmd),
	[FIFO_FLATCMD] = FIFO_CMD(FLATCMD, flatcmd),
	[FIFO_FUZZCMD] = FIFO_CMD(FUZZCMD, fuzzcmd),
	[FIFO_OGCMD] = FIFO_CMD(OGCMD, ogcmd),
	[FIFO_SWCMD] = FIFO_CMD(SWCMD, swcmd),
	[FIFO_XYOUTR] = FIFO_ADDR(XYOUTR, xyoutr),
	[FIFO_XYOUTW] = FIFO_ADDR(XYOUTW, xyoutw),
	[FIFO_SROUT] = FIFO_BLOCK(SROUT, srout),
	[FIFO_TEXOUT] = FIFO_BLOCK_MASK(TEXOUT, texout),
	[FIFO_FLATOUT] = FIFO_BLOCK(FLATOUT, flatout),
	[FIFO_FUZZOUT] = FIFO_MASK(FUZZOUT, fuzzout),
	[FIFO_OGOUT] = FIFO_BLOCK_MASK(OGOUT, ogout),
};

static uint32_t le32_read(uint8_t *ptr) {
	return ptr[0] | ptr[1] << 8 | ptr[2] << 16 | ptr[3] << 24;
}

static int harddoom2_fifo_rptr(HardDoom2State *d, int which) {
	struct harddoom2_fifo *desc = &harddoom2_fifos[which];
	uint32_t *state = (void *)((char *)d + desc->vm_off_state);
	return *state & (desc->size - 1);
}

static int harddoom2_fifo_wptr(HardDoom2State *d, int which) {
	struct harddoom2_fifo *desc = &harddoom2_fifos[which];
	uint32_t *state = (void *)((char *)d + desc->vm_off_state);
	return *state >> 16 & (desc->size - 1);
}

/* Returns number of free slots in the FIFO.  */
static int harddoom2_fifo_free(HardDoom2State *d, int which) {
	struct harddoom2_fifo *desc = &harddoom2_fifos[which];
	uint32_t *state = (void *)((char *)d + desc->vm_off_state);
	int rptr = *state & 0xffff;
	int wptr = *state >> 16 & 0xffff;
	int used = (wptr - rptr) & (2 * desc->size - 1);
	/* This considers overfull FIFO to have free slots.
	 * It's part of the fun.  */
	return (desc->size - used) & (2 * desc->size - 1);
}

/* Bumps the FIFO read pointer, returns previous value.  */
static int harddoom2_fifo_read(HardDoom2State *d, int which) {
	struct harddoom2_fifo *desc = &harddoom2_fifos[which];
	uint32_t *state = (void *)((char *)d + desc->vm_off_state);
	int rptr = *state & 0xffff;
	int wptr = *state >> 16 & 0xffff;
	int res = rptr & (desc->size - 1);
	rptr++;
	rptr &= (2 * desc->size - 1);
	*state = wptr << 16 | rptr;
	return res;
}

/* Bumps the FIFO write pointer, returns previous value.  */
static int harddoom2_fifo_write(HardDoom2State *d, int which) {
	struct harddoom2_fifo *desc = &harddoom2_fifos[which];
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

static int harddoom2_fifo_can_read(HardDoom2State *d, int which) {
	struct harddoom2_fifo *desc = &harddoom2_fifos[which];
	uint32_t *state = (void *)((char *)d + desc->vm_off_state);
	int rptr = *state & 0xffff;
	int wptr = *state >> 16 & 0xffff;
	return rptr != wptr;
}

static int harddoom2_fifo_can_write(HardDoom2State *d, int which) {
	return harddoom2_fifo_free(d, which) != 0;
}

/* Appends a command to FECMD FIFO.  */
static void harddoom2_fecmd_write(HardDoom2State *d, const uint32_t *cmd) {
	int wptr = harddoom2_fifo_wptr(d, FIFO_FECMD);
	for (int i = 0; i < 8; i++)
		d->fifo_fecmd_data[HARDDOOM2_CMD_SEND_SIZE * wptr + i] = cmd[i];
	harddoom2_fifo_write(d, FIFO_FECMD);
}

/* Gets a command from FECMD FIFO.  */
static void harddoom2_fecmd_read(HardDoom2State *d, uint32_t *cmd) {
	int rptr = harddoom2_fifo_rptr(d, FIFO_FECMD);
	for (int i = 0; i < 8; i++)
		cmd[i] = d->fifo_fecmd_data[HARDDOOM2_CMD_SEND_SIZE * rptr + i];
#if 0
	printf("FECMD");
	for (int i = 0; i < 8; i++) {
		printf(" %08x", cmd[i]);
	}
	printf("\n");
#endif
	harddoom2_fifo_read(d, FIFO_FECMD);
}

/* Appends a command to FECMD FIFO, or triggers CMD_OVERFLOW.  */
static void harddoom2_cmd_send(HardDoom2State *d) {
	if (!harddoom2_fifo_can_write(d, FIFO_FECMD) || !(d->enable & HARDDOOM2_ENABLE_CMD_SEND)) {
		d->intr |= HARDDOOM2_INTR_CMD_OVERFLOW;
	} else {
		harddoom2_fecmd_write(d, d->cmd_send);
	}
}

/* Appends a command to a command FIFO.  */
static void harddoom2_fifo_cmd_write(HardDoom2State *d, int which, uint32_t cmd, uint32_t data) {
	int wptr = harddoom2_fifo_wptr(d, which);
	struct harddoom2_fifo *desc = &harddoom2_fifos[which];
	uint32_t *fifo_cmd = (void *)((char *)d + desc->vm_off_window_a);
	uint32_t *fifo_data = (void *)((char *)d + desc->vm_off_window_b);
	fifo_cmd[wptr] = cmd;
	fifo_data[wptr] = data;
#if 0
	printf("%s %03x %08x\n", desc->name, cmd, data);
#endif
	harddoom2_fifo_write(d, which);
}

/* Gets a command from a command FIFO.  */
static uint32_t harddoom2_fifo_cmd_read(HardDoom2State *d, int which, uint32_t *data) {
	int rptr = harddoom2_fifo_rptr(d, which);
	struct harddoom2_fifo *desc = &harddoom2_fifos[which];
	uint32_t *fifo_cmd = (void *)((char *)d + desc->vm_off_window_a);
	uint32_t *fifo_data = (void *)((char *)d + desc->vm_off_window_b);
	*data = fifo_data[rptr];
	harddoom2_fifo_read(d, which);
	return fifo_cmd[rptr];
}

/* Appends a command to an address FIFO.  */
static void harddoom2_fifo_addr_write(HardDoom2State *d, int which, uint64_t addr) {
	int wptr = harddoom2_fifo_wptr(d, which);
	struct harddoom2_fifo *desc = &harddoom2_fifos[which];
	uint64_t *fifo = (void *)((char *)d + desc->vm_off_window_a);
	fifo[wptr] = addr;
	harddoom2_fifo_write(d, which);
}

/* Gets a command from an address FIFO.  */
static uint64_t harddoom2_fifo_addr_read(HardDoom2State *d, int which) {
	int rptr = harddoom2_fifo_rptr(d, which);
	struct harddoom2_fifo *desc = &harddoom2_fifos[which];
	uint64_t *fifo = (void *)((char *)d + desc->vm_off_window_a);
	harddoom2_fifo_read(d, which);
	return fifo[rptr];
}

/* Recomputes status register and PCI interrupt line.  */
static void harddoom2_status_update(HardDoom2State *d) {
	int i;
	d->status = 0;
	/* FETCH_CMD busy iff read != write.  */
	if (d->cmd_read_idx != d->cmd_write_idx)
		d->status |= HARDDOOM2_STATUS_CMD_FETCH;
	/* FE busy iff not waiting for FIFO.  */
	if (!(d->fe_state & HARDDOOM2_FE_STATE_WAIT_FIFO))
		d->status |= HARDDOOM2_STATUS_FE;
	/* XY busy iff command pending in any of the three regs.  */
	if (HARDDOOM2_XY_STATE_EXTR_PENDING_CMD_TYPE(d->xy_state))
		d->status |= HARDDOOM2_STATUS_XY;
	if (HARDDOOM2_XY_STATE_EXTR_DST_CMD_TYPE(d->xy_state))
		d->status |= HARDDOOM2_STATUS_XY;
	if (HARDDOOM2_XY_STATE_EXTR_SRC_CMD_TYPE(d->xy_state))
		d->status |= HARDDOOM2_STATUS_XY;
	/* TEX busy iff command pending.  */
	if (HARDDOOM2_TEX_DRAW_EXTR_LENGTH(d->tex_draw))
		d->status |= HARDDOOM2_STATUS_TEX;
	/* FLAT busy iff command pending.  */
	if (HARDDOOM2_FLAT_DRAW_EXTR_LENGTH(d->flat_draw))
		d->status |= HARDDOOM2_STATUS_FLAT;
	if (HARDDOOM2_FLAT_READ_EXTR_LENGTH(d->flat_read))
		d->status |= HARDDOOM2_STATUS_FLAT;
	/* FUZZ busy iff command pending.  */
	if (d->fuzz_draw)
		d->status |= HARDDOOM2_STATUS_FUZZ;
	/* OG busy iff command pending.  */
	if (HARDDOOM2_OG_STATE_EXTR_CMD(d->og_state))
		d->status |= HARDDOOM2_STATUS_OG;
	/* SW busy iff command pending.  */
	if (HARDDOOM2_SW_STATE_EXTR_CMD(d->sw_state))
		d->status |= HARDDOOM2_STATUS_SW;
	/* FIFOs busy iff read != write.  */
	for (i = 0; i < ARRAY_SIZE(harddoom2_fifos); i++) {
		struct harddoom2_fifo *desc = &harddoom2_fifos[i];
		if (harddoom2_fifo_can_read(d, i))
			d->status |= desc->status;
	}
	if (d->fifo_xysync_state)
		d->status |= HARDDOOM2_STATUS_FIFO_XYSYNC;
	/* determine and set PCI interrupt status */
	pci_set_irq(PCI_DEVICE(d), !!(d->intr & d->intr_enable));
}

/* Resets TLBs, forcing a reread of PT.  */
static void harddoom2_reset_tlb(HardDoom2State *d) {
	/* Kill ENTRY valid bits.  */
	for (int i = 0; i < HARDDOOM2_TLB_NUM; i++)
		d->tlb_entry[i] &= ~HARDDOOM2_TLB_ENTRY_VALID;
}

static void harddoom2_reset_sw_cache(HardDoom2State *d) {
	for (int i = 0; i < HARDDOOM2_SW_CACHE_LINES; i++)
		d->sw_cache_state[i] &= ~HARDDOOM2_SW_CACHE_STATE_VALID;
}

static void harddoom2_reset(HardDoom2State *d, uint32_t val) {
	int i;
	if (val & HARDDOOM2_RESET_FE)
		d->fe_state = 0;
	if (val & HARDDOOM2_RESET_XY) {
		d->xy_state &= HARDDOOM2_XY_STATE_SURF_DST_WIDTH_MASK | HARDDOOM2_XY_STATE_SURF_SRC_WIDTH_MASK;
	}
	if (val & HARDDOOM2_RESET_TEX) {
		d->tex_draw = 0;
		d->tex_mask = 0;
	}
	if (val & HARDDOOM2_RESET_FLAT) {
		d->flat_draw = 0;
		d->flat_read = 0;
	}
	if (val & HARDDOOM2_RESET_FUZZ) {
		d->fuzz_draw = 0;
	}
	if (val & HARDDOOM2_RESET_OG) {
		d->og_state = 0;
	}
	if (val & HARDDOOM2_RESET_SW) {
		d->sw_state = 0;
	}
	for (i = 0; i < ARRAY_SIZE(harddoom2_fifos); i++) {
		struct harddoom2_fifo *desc = &harddoom2_fifos[i];
		uint32_t *state = (void *)((char *)d + desc->vm_off_state);
		if (val & desc->reset) {
			*state = 0;
		}
	}
	if (val & HARDDOOM2_RESET_FIFO_XYSYNC)
		d->fifo_xysync_state = 0;
	if (val & HARDDOOM2_RESET_STATS) {
		int i;
		for (i = 0; i < HARDDOOM2_STATS_NUM; i++)
			d->stats[i] = 0;
	}
	if (val & HARDDOOM2_RESET_TLB) {
		harddoom2_reset_tlb(d);
	}
	if (val & HARDDOOM2_RESET_FLAT_CACHE) {
		d->flat_cache_state &= ~HARDDOOM2_FLAT_CACHE_STATE_VALID;
	}
	if (val & HARDDOOM2_RESET_TEX_CACHE) {
		d->tex_cache_state &= ~HARDDOOM2_TEX_CACHE_STATE_VALID;
	}
	if (val & HARDDOOM2_RESET_SW_CACHE) {
		harddoom2_reset_sw_cache(d);
	}
}

/* Sets the given PT and flushes corresponding TLB.  */
static void harddoom2_set_pt(HardDoom2State *d, int which, uint32_t val) {
	d->tlb_pt[which] = val;
	d->tlb_entry[which] &= ~HARDDOOM2_TLB_ENTRY_VALID;
	d->stats[HARDDOOM2_STAT_TLB_CHANGE(which)]++;
}

/* Converts virtual offset to a physical address -- handles PT lookup.
 * If something goes wrong, disables the enable bit and fires an interrupt.
 * Returns true if succeeded.  */
static bool harddoom2_translate_addr(HardDoom2State *d, int which, uint32_t offset, uint64_t *res, bool need_write) {
	uint32_t vidx = offset >> HARDDOOM2_PAGE_SHIFT & HARDDOOM2_VIDX_MASK;
	uint32_t entry = d->tlb_entry[which];
	uint32_t cur_vidx = d->tlb_vaddr[which] >> HARDDOOM2_PAGE_SHIFT & HARDDOOM2_VIDX_MASK;
	uint64_t pte_addr = ((uint64_t)d->tlb_pt[which] << 8) + (vidx << 2);
	d->tlb_vaddr[which] = offset;
	if (vidx != cur_vidx || !(entry & HARDDOOM2_TLB_ENTRY_VALID)) {
		/* Mismatched or invalid tag -- fetch a new one.  */
		uint8_t pteb[4];
		pci_dma_read(&d->dev, pte_addr, &pteb, sizeof pteb);
		d->tlb_entry[which] = entry = (le32_read(pteb) & 0xfffffff3) | HARDDOOM2_TLB_ENTRY_VALID;
		d->stats[HARDDOOM2_STAT_TLB_MISS(which)]++;
	} else {
		d->stats[HARDDOOM2_STAT_TLB_HIT(which)]++;
	}
	if (!(entry & HARDDOOM2_PTE_VALID) || (!(entry & HARDDOOM2_PTE_WRITABLE) && need_write)) {
		d->intr |= HARDDOOM2_INTR_PAGE_FAULT(which);
		return false;
	}
	*res = (uint64_t)(entry & HARDDOOM2_PTE_PHYS_MASK) >> HARDDOOM2_PTE_PHYS_SHIFT << HARDDOOM2_PAGE_SHIFT | (offset & (HARDDOOM2_PAGE_SIZE - 1));
	return true;
}

/* Converts x block, y position to physical address.  If something goes wrong,
 * triggers an interrupt and disables the enable bit.  Returns true if succeeded.  */
static bool harddoom2_translate_surf_xy(HardDoom2State *d, bool is_src, bool need_write, uint16_t x, uint16_t y, uint64_t *res) {
	int w;
	if (is_src)
		w = HARDDOOM2_XY_STATE_EXTR_SURF_SRC_WIDTH(d->xy_state);
	else
		w = HARDDOOM2_XY_STATE_EXTR_SURF_DST_WIDTH(d->xy_state);
	uint32_t offset = (y * w + x) << 6;
	offset &= HARDDOOM2_TLB_VADDR_MASK;
	if (x >= w) {
		/* Fire an interrupt and disable ourselves.  */
#if 0
		printf("OVF %08x %08x %08x %08x %d %d %d\n", d->xy_state, d->xy_pending_data, d->xy_dst_data, d->xy_src_data, x, y, w);
#endif
		d->enable &= ~HARDDOOM2_ENABLE_XY;
		if (is_src)
			d->intr |= HARDDOOM2_INTR_SURF_SRC_OVERFLOW;
		else
			d->intr |= HARDDOOM2_INTR_SURF_DST_OVERFLOW;
		return false;
	}
	if (!harddoom2_translate_addr(d, is_src ? HARDDOOM2_TLB_IDX_SURF_SRC : HARDDOOM2_TLB_IDX_SURF_DST, offset, res, need_write)) {
#if 0
		printf("PF %08x %08x %08x %08x %d %d %d %08x\n", d->xy_state, d->xy_pending_data, d->xy_dst_data, d->xy_src_data, x, y, w, offset);
#endif
		d->enable &= ~HARDDOOM2_ENABLE_XY;
		return false;
	}
#if 0
	printf("OK %08x %08x %08x %08x %d %d %d %08x %016lx\n", d->xy_state, d->xy_pending_data, d->xy_dst_data, d->xy_src_data, x, y, w, offset, *res);
#endif
	return true;
}

/* MMIO write handlers.  */
static void harddoom2_mmio_writeb(void *opaque, hwaddr addr, uint32_t val)
{
	int i, idx;
	HardDoom2State *d = opaque;
	qemu_mutex_lock(&d->mutex);
	bool found = false;
	for (i = 0; i < ARRAY_SIZE(harddoom2_registers); i++) {
		struct harddoom2_register *desc = &harddoom2_registers[i];
		if (addr >= desc->bar_off && addr < desc->bar_off + desc->num && desc->type == REG_TYPE_SIMPLE_8) {
			uint8_t *reg = (void *)((char *)opaque + desc->vm_off);
			idx = addr - desc->bar_off;
			reg[idx] = val & desc->mask;
			if (val & ~desc->mask) {
				fprintf(stderr, "harddoom2 error: invalid %s value %08x (valid mask is %08x)\n", desc->name, val, desc->mask);
			}
			found = true;
		}
	}
	for (i = 0; i < ARRAY_SIZE(harddoom2_fifos); i++) {
		struct harddoom2_fifo *desc = &harddoom2_fifos[i];
		if ((desc->type == FIFO_TYPE_BLOCK || desc->type == FIFO_TYPE_BLOCK_MASK) && (addr & ~0x3f) == desc->bar_off_window_a) {
			idx = addr & 0x3f;
			uint8_t *data = (void *)((char *)d + desc->vm_off_window_a);
			data[harddoom2_fifo_wptr(d, i) * HARDDOOM2_BLOCK_SIZE + idx] = val;
			found = true;
			break;
		}
	}
	if (!found) {
		fprintf(stderr, "harddoom2 error: byte-sized write at %03x, value %02x\n", (int)addr, val);
	}
	qemu_mutex_unlock(&d->mutex);
}

static void harddoom2_mmio_writel(void *opaque, hwaddr addr, uint32_t val)
{
	HardDoom2State *d = opaque;
	qemu_mutex_lock(&d->mutex);
	int i, idx;
	bool found = false;
	for (i = 0; i < ARRAY_SIZE(harddoom2_registers); i++) {
		struct harddoom2_register *desc = &harddoom2_registers[i];
		if (addr >= desc->bar_off && addr < desc->bar_off + desc->num * 4 && !(addr & 3) && desc->type == REG_TYPE_SIMPLE_32) {
			uint32_t *reg = (void *)((char *)opaque + desc->vm_off);
			idx = (addr - desc->bar_off) >> 2;
			reg[idx] = val & desc->mask;
			if (val & ~desc->mask) {
				fprintf(stderr, "harddoom2 error: invalid %s value %08x (valid mask is %08x)\n", desc->name, val, desc->mask);
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
				fprintf(stderr, "harddoom2 error: invalid %s value %08x (valid mask is %08x)\n", desc->name, val, desc->mask);
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
	for (i = 0; i < ARRAY_SIZE(harddoom2_fifos); i++) {
		struct harddoom2_fifo *desc = &harddoom2_fifos[i];
		if (addr == desc->bar_off_state) {
			uint32_t mask = (2 * desc->size - 1) * 0x10001;
			uint32_t *reg = (void *)((char *)opaque + desc->vm_off_state);
			*reg = val & mask;
			if (val & ~mask) {
				fprintf(stderr, "harddoom2 error: invalid %s_STATE value %08x (valid mask is %08x)\n", desc->name, val, mask);
			}
			found = true;
			break;
		}
		if ((desc->type == FIFO_TYPE_FECMD) && (addr & ~0x1f) == desc->bar_off_window_a) {
			idx = addr >> 2 & 7;
			uint32_t *data = (void *)((char *)d + desc->vm_off_window_a);
			data[harddoom2_fifo_wptr(d, i) * HARDDOOM2_CMD_SEND_SIZE + idx] = val;
			found = true;
			break;
		}
		if ((desc->type == FIFO_TYPE_CMD) && addr == desc->bar_off_window_a) {
			uint32_t *data = (void *)((char *)d + desc->vm_off_window_a);
			data[harddoom2_fifo_wptr(d, i)] = val;
			found = true;
			break;
		}
		if ((desc->type == FIFO_TYPE_CMD) && addr == desc->bar_off_window_b) {
			uint32_t *data = (void *)((char *)d + desc->vm_off_window_b);
			data[harddoom2_fifo_wptr(d, i)] = val;
			found = true;
			break;
		}
		if ((desc->type == FIFO_TYPE_ADDR) && addr == desc->bar_off_window_a) {
			uint64_t *data = (void *)((char *)d + desc->vm_off_window_a);
			int wptr = harddoom2_fifo_wptr(d, i);
			data[wptr] &= ~0xffffffffull;
			data[wptr] |= val;
			found = true;
			break;
		}
		if ((desc->type == FIFO_TYPE_ADDR) && addr == desc->bar_off_window_a + 4) {
			uint64_t *data = (void *)((char *)d + desc->vm_off_window_a);
			int wptr = harddoom2_fifo_wptr(d, i);
			data[wptr] &= 0xffffffffull;
			data[wptr] |= (uint64_t)val << 32;
			found = true;
			break;
		}
		if ((desc->type == FIFO_TYPE_MASK || desc->type == FIFO_TYPE_BLOCK_MASK) && addr == desc->bar_off_window_b) {
			uint64_t *data = (void *)((char *)d + desc->vm_off_window_b);
			int wptr = harddoom2_fifo_wptr(d, i);
			data[wptr] &= ~0xffffffffull;
			data[wptr] |= val;
			found = true;
		}
		if ((desc->type == FIFO_TYPE_MASK || desc->type == FIFO_TYPE_BLOCK_MASK) && addr == desc->bar_off_window_b + 4) {
			uint64_t *data = (void *)((char *)d + desc->vm_off_window_b);
			int wptr = harddoom2_fifo_wptr(d, i);
			data[wptr] &= 0xffffffffull;
			data[wptr] |= (uint64_t)val << 32;
			found = true;
		}
	}
	if (addr == HARDDOOM2_RESET) {
		harddoom2_reset(d, val);
		if (val & ~HARDDOOM2_RESET_ALL)
			fprintf(stderr, "harddoom2 error: invalid RESET value %08x\n", val);
		found = true;
	}
	if (addr == HARDDOOM2_INTR) {
		d->intr &= ~val;
		if (val & ~HARDDOOM2_INTR_MASK)
			fprintf(stderr, "harddoom2 error: invalid INTR value %08x\n", val);
		found = true;
	}
	if (addr == HARDDOOM2_CMD_SEND(7)) {
		harddoom2_cmd_send(d);
	}
	if (addr == HARDDOOM2_CMD_PT) {
		harddoom2_set_pt(d, HARDDOOM2_TLB_IDX_CMD, d->cmd_pt);
	}
	if (!found)
		fprintf(stderr, "harddoom2 error: invalid register write at %03x, value %08x\n", (int)addr, val);
	harddoom2_status_update(d);
	qemu_cond_signal(&d->cond);
	qemu_mutex_unlock(&d->mutex);
}

static uint32_t harddoom2_mmio_readb(void *opaque, hwaddr addr)
{
	int i, idx;
	uint8_t res = 0xff;
	bool found = false;
	HardDoom2State *d = opaque;
	qemu_mutex_lock(&d->mutex);
	for (i = 0; i < ARRAY_SIZE(harddoom2_registers); i++) {
		struct harddoom2_register *desc = &harddoom2_registers[i];
		if (addr >= desc->bar_off && addr < desc->bar_off + desc->num && desc->type == REG_TYPE_SIMPLE_8) {
			uint8_t *reg = (void *)((char *)opaque + desc->vm_off);
			idx = addr - desc->bar_off;
			res = reg[idx];
			found = true;
		}
	}
	for (i = 0; i < ARRAY_SIZE(harddoom2_fifos); i++) {
		struct harddoom2_fifo *desc = &harddoom2_fifos[i];
		if ((desc->type == FIFO_TYPE_BLOCK || desc->type == FIFO_TYPE_BLOCK_MASK) && (addr & ~0x3f) == desc->bar_off_window_a) {
			idx = addr & 0x3f;
			uint8_t *data = (void *)((char *)d + desc->vm_off_window_a);
			res = data[harddoom2_fifo_rptr(d, i) * HARDDOOM2_BLOCK_SIZE + idx];
			found = true;
			break;
		}
	}
	if (!found) {
		fprintf(stderr, "harddoom2 error: byte-sized read at %03x\n", (int)addr);
		res = 0xff;
	}
	qemu_mutex_unlock(&d->mutex);
	return res;
}

static uint32_t harddoom2_mmio_readl(void *opaque, hwaddr addr)
{
	HardDoom2State *d = opaque;
	uint32_t res = 0xffffffff;
	qemu_mutex_lock(&d->mutex);
	bool found = false;
	int i, idx;
	for (i = 0; i < ARRAY_SIZE(harddoom2_registers); i++) {
		struct harddoom2_register *desc = &harddoom2_registers[i];
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
	for (i = 0; i < ARRAY_SIZE(harddoom2_fifos); i++) {
		struct harddoom2_fifo *desc = &harddoom2_fifos[i];
		if (addr == desc->bar_off_state) {
			uint32_t *reg = (void *)((char *)opaque + desc->vm_off_state);
			res = *reg;
			found = true;
			break;
		}
		if ((desc->type == FIFO_TYPE_FECMD) && (addr & ~0x1f) == desc->bar_off_window_a) {
			idx = addr >> 2 & 7;
			uint32_t *data = (void *)((char *)d + desc->vm_off_window_a);
			res = data[harddoom2_fifo_rptr(d, i) * HARDDOOM2_CMD_SEND_SIZE + idx];
			found = true;
			break;
		}
		if ((desc->type == FIFO_TYPE_CMD) && addr == desc->bar_off_window_a) {
			uint32_t *data = (void *)((char *)d + desc->vm_off_window_a);
			res = data[harddoom2_fifo_rptr(d, i)];
			found = true;
			break;
		}
		if ((desc->type == FIFO_TYPE_CMD) && addr == desc->bar_off_window_b) {
			uint32_t *data = (void *)((char *)d + desc->vm_off_window_b);
			res = data[harddoom2_fifo_rptr(d, i)];
			found = true;
			break;
		}
		if ((desc->type == FIFO_TYPE_ADDR) && addr == desc->bar_off_window_a) {
			uint64_t *data = (void *)((char *)d + desc->vm_off_window_a);
			int rptr = harddoom2_fifo_rptr(d, i);
			res = data[rptr];
			found = true;
			break;
		}
		if ((desc->type == FIFO_TYPE_ADDR) && addr == desc->bar_off_window_a + 4) {
			uint64_t *data = (void *)((char *)d + desc->vm_off_window_a);
			int rptr = harddoom2_fifo_rptr(d, i);
			res = data[rptr] >> 32;
			found = true;
			break;
		}
		if ((desc->type == FIFO_TYPE_MASK || desc->type == FIFO_TYPE_BLOCK_MASK) && addr == desc->bar_off_window_b) {
			uint64_t *data = (void *)((char *)d + desc->vm_off_window_b);
			int wptr = harddoom2_fifo_rptr(d, i);
			res = data[wptr];
			found = true;
		}
		if ((desc->type == FIFO_TYPE_MASK || desc->type == FIFO_TYPE_BLOCK_MASK) && addr == desc->bar_off_window_b + 4) {
			uint64_t *data = (void *)((char *)d + desc->vm_off_window_b);
			int wptr = harddoom2_fifo_rptr(d, i);
			res = data[wptr] >> 32;
			found = true;
		}
	}
	if (addr == HARDDOOM2_STATUS) {
		res = d->status;
		found = true;
	}
	if (addr == HARDDOOM2_INTR) {
		res = d->intr;
		found = true;
	}
	if (addr == HARDDOOM2_CMD_FREE) {
		res = harddoom2_fifo_free(d, FIFO_FECMD);
		found = true;
	}
	if (!found) {
		fprintf(stderr, "harddoom2 error: invalid register read at %03x\n", (int)addr);
		res = 0xffffffff;
	}
	harddoom2_status_update(d);
	qemu_cond_signal(&d->cond);
	qemu_mutex_unlock(&d->mutex);
	return res;
}

static uint64_t harddoom2_mmio_read(void *opaque, hwaddr addr, unsigned size) {
	if (size == 1) {
		return harddoom2_mmio_readb(opaque, addr);
	} else if (size == 4) {
		return harddoom2_mmio_readl(opaque, addr);
	} else {
		fprintf(stderr, "harddoom2 error: invalid register read of size %u at %03x\n", size, (int)addr);
		return ~0ULL;
	}
}

static void harddoom2_mmio_write(void *opaque, hwaddr addr, uint64_t val,
                unsigned size)
{
	if (size == 1) {
		harddoom2_mmio_writeb(opaque, addr, val);
	} else if (size == 4) {
		harddoom2_mmio_writel(opaque, addr, val);
	} else {
		fprintf(stderr, "harddoom2 error: invalid register write of size %u at %03x\n", size, (int)addr);
	}
}

static const MemoryRegionOps harddoom2_mmio_ops = {
	.read = harddoom2_mmio_read,
	.write = harddoom2_mmio_write,
	.endianness = DEVICE_LITTLE_ENDIAN,
};

/* Power-up reset of the device.  */
static void harddoom2_power_reset(DeviceState *ds)
{
	HardDoom2State *s = container_of(ds, HardDoom2State, dev.qdev);
	int i, j;
	qemu_mutex_lock(&s->mutex);
	for (i = 0; i < ARRAY_SIZE(harddoom2_registers); i++) {
		struct harddoom2_register *desc = &harddoom2_registers[i];
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
	for (i = 0; i < ARRAY_SIZE(harddoom2_fifos); i++) {
		struct harddoom2_fifo *desc = &harddoom2_fifos[i];
		uint32_t mask = (2 * desc->size - 1) * 0x10001;
		uint32_t *state = (void *)((char *)s + desc->vm_off_state);
		*state = mrand48() & mask;
		if (desc->type == FIFO_TYPE_FECMD) {
			uint32_t *cmd = (void *)((char *)s + desc->vm_off_window_a);
			for (j = 0; j < desc->size * HARDDOOM2_CMD_SEND_SIZE; j++) {
				cmd[j] = mrand48();
			}
		}
		if (desc->type == FIFO_TYPE_CMD) {
			uint32_t *cmd = (void *)((char *)s + desc->vm_off_window_a);
			uint32_t *data = (void *)((char *)s + desc->vm_off_window_b);
			for (j = 0; j < desc->size; j++) {
				cmd[j] = mrand48() & 0xfff;
				data[j] = mrand48();
			}
		}
		if (desc->type == FIFO_TYPE_ADDR) {
			uint64_t *addr = (void *)((char *)s + desc->vm_off_window_a);
			for (j = 0; j < desc->size; j++) {
				addr[j] = (uint64_t)mrand48() << 32;
				addr[j] |= (uint32_t)mrand48();
			}
		}
		if (desc->type == FIFO_TYPE_BLOCK || desc->type == FIFO_TYPE_BLOCK_MASK) {
			uint8_t *data = (void *)((char *)s + desc->vm_off_window_a);
			for (j = 0; j < desc->size * HARDDOOM2_BLOCK_SIZE; j++)
				data[j] = mrand48();
		}
		if (desc->type == FIFO_TYPE_MASK || desc->type == FIFO_TYPE_BLOCK_MASK) {
			uint64_t *mask = (void *)((char *)s + desc->vm_off_window_b);
			for (j = 0; j < desc->size; j++) {
				mask[j] = (uint64_t)mrand48() << 32;
				mask[j] |= (uint32_t)mrand48();
			}
		}
	}
	s->intr = mrand48() & HARDDOOM2_INTR_MASK;
	/* These registers play fair. */
	s->enable = 0;
	s->intr_enable = 0;
	harddoom2_status_update(s);
	qemu_mutex_unlock(&s->mutex);
}

/* Runs CMD_FETCH for some time.  Returns true if anything has been done.  */
static bool harddoom2_run_cmd_fetch(HardDoom2State *d) {
	bool any = false;
	if (!(d->enable & HARDDOOM2_ENABLE_CMD_FETCH))
		return any;
	while (d->cmd_read_idx != d->cmd_write_idx) {
		/* Now, check if there's some place to put commands.  */
		if (!harddoom2_fifo_can_write(d, FIFO_FECMD))
			break;
		/* There are commands to read, and there's somewhere to put them. Do it.  */
		uint8_t cmdb[32];
		uint64_t paddr;
		if (!harddoom2_translate_addr(d, HARDDOOM2_TLB_IDX_CMD, d->cmd_read_idx * sizeof cmdb, &paddr, false)) {
			d->enable &= ~HARDDOOM2_ENABLE_CMD_FETCH;
			return any;
		}
		pci_dma_read(&d->dev, paddr, &cmdb, sizeof cmdb);
		uint32_t cmd[8];
		for (int i = 0; i < 8; i++)
			cmd[i] = le32_read(cmdb + i * 4);
		d->cmd_read_idx++;
		if (d->cmd_read_idx == d->cmd_size)
			d->cmd_read_idx = 0;
		d->cmd_read_idx &= HARDDOOM2_CMD_IDX_MASK;
		harddoom2_fecmd_write(d, cmd);
		any = true;
	}
	return any;
}

/* Runs the FE for some time.  Returns true if anything has been done.  */
static bool harddoom2_run_fe(HardDoom2State *d) {
	bool any = false;
	if (!(d->enable & HARDDOOM2_ENABLE_FE))
		return any;
	if (d->fe_state & HARDDOOM2_FE_STATE_WAIT_FIFO) {
		if (!harddoom2_fifo_can_read(d, FIFO_FECMD))
			return any;
		d->fe_state &= ~HARDDOOM2_FE_STATE_WAIT_FIFO;
	}
	int num_insns = 0x1000;
	while (num_insns--) {
		int pc = d->fe_state & HARDDOOM2_FE_STATE_PC_MASK;
		uint32_t insn = d->fe_code[pc++];
		pc &= HARDDOOM2_FE_STATE_PC_MASK;
		int op_a = insn >> 21 & 0x1f;
		int op_b = insn >> 16 & 0x1f;
		int op_c = insn >> 11 & 0x1f;
		int op_d = insn >> 6 & 0x1f;
		int op_e = insn >> 1 & 0x1f;
		uint32_t imm_12 = insn & 0xfff;
		uint32_t imm_7 = insn >> 12 & 0x7f;
		uint32_t imm_s11 = insn & 0x7ff;
		if (imm_s11 & 1 << 10)
			imm_s11 -= (1 << 11);
		uint32_t imm_s16 = insn & 0xffff;
		if (imm_s16 & 1 << 15)
			imm_s16 -= (1 << 16);
		uint32_t tmp, mask, addr;
		int fifo;
		uint64_t paddr;
		//printf("FE %03x %08x\n", pc, insn);
		switch (insn >> 26 & 0x3f) {
		case 0x00:
			/* rri12 format.  */
			switch (insn >> 12 & 0xf) {
			case 0x0:
				/* b is third-level opcode.  */
				switch (op_b) {
				case 0x00:
					/* b -- unconditional branch to imm1.  */
					pc = imm_12;
					break;
				case 0x01:
					/* bl -- unconditional branch to imm1, saving return address in r2 register.  */
					d->fe_reg[op_a] = pc;
					pc = imm_12;
					break;
				case 0x02:
					/* bi -- branch indirect to imm1 + register r2.  */
					pc = imm_12 + d->fe_reg[op_a];
					pc &= HARDDOOM2_FE_STATE_PC_MASK;
					break;
				case 0x03:
					/* rcmd -- read command from FIFO to registers 0-7.  */
					if (harddoom2_fifo_can_read(d, FIFO_FECMD)) {
						harddoom2_fecmd_read(d, d->fe_reg);
					} else {
						d->fe_state |= HARDDOOM2_FE_STATE_WAIT_FIFO;
						return true;
					}
					break;
				case 0x04:
					/* pong -- trigger PONG_ASYNC.  No arguments.  */
					d->intr |= HARDDOOM2_INTR_PONG_ASYNC;
					break;
				case 0x05:
					/* error -- trigger FE_ERROR and stop FE.  imm_12 is error code.  */
					d->fe_error_code = imm_12;
					d->enable &= ~HARDDOOM2_ENABLE_FE;
					d->intr |= HARDDOOM2_INTR_FE_ERROR;
					d->fe_state = pc;
					return true;
				case 0x06:
					/* stat -- bump statistics counter #imm_12.  */
#if 0
					printf("STAT %03x\n", imm_12);
#endif
					d->stats[imm_12 & 0x1f]++;
					break;
				case 0x07:
					/* setpt -- bind TLB #imm_12 to a new page table.  */
					harddoom2_set_pt(d, imm_12 & 7, d->fe_reg[op_a]);
					break;
				case 0x08:
					/* xycmd -- send a command to XY unit.  */
					fifo = FIFO_XYCMD;
					goto cmd;
				case 0x09:
					/* texcmd -- send a command to TEX unit.  */
					fifo = FIFO_TEXCMD;
					goto cmd;
				case 0x0a:
					/* flatcmd -- send a command to FLAT unit.  */
					fifo = FIFO_FLATCMD;
					goto cmd;
				case 0x0b:
					/* fuzzcmd -- send a command to FUZZ unit.  */
					fifo = FIFO_FUZZCMD;
					goto cmd;
				case 0x0c:
					/* ogcmd -- send a command to OG unit.  */
					fifo = FIFO_OGCMD;
					goto cmd;
				cmd:
					if (!harddoom2_fifo_can_write(d, fifo)) {
						return any;
					} else {
						harddoom2_fifo_cmd_write(d, fifo, imm_12, d->fe_reg[op_a]);
					}
					break;
				}
				break;
			case 0x1:
				/* tlb -- translate virtual address to physical.  */
				if (!harddoom2_translate_addr(d, imm_12 & 7, d->fe_reg[op_b], &paddr, imm_12 >> 3 & 1)) {
					d->enable &= ~HARDDOOM2_ENABLE_FE;
					return any;
				}
				d->fe_reg[op_a] = paddr >> 8;
				break;
			case 0x2:
				/* st -- store register a to memory at register b + imm_12. */
				addr = d->fe_reg[op_b] + imm_12;
				addr &= HARDDOOM2_FE_DATA_SIZE - 1;
				d->fe_data[addr] = d->fe_reg[op_a];
#if 0
				for (int i = 0; i < HARDDOOM2_FE_DATA_SIZE; i++) {
					printf("%08x", d->fe_data[i]);
					if (i % 0x10 == 0xf)
						printf("\n");
					else
						printf(" ");
				}
#endif
				break;
			case 0x3:
				/* ld -- load register a from memory at register b + imm_12. */
				addr = d->fe_reg[op_b] + imm_12;
				addr &= HARDDOOM2_FE_DATA_SIZE - 1;
				d->fe_reg[op_a] = d->fe_data[addr];
				break;
			case 0x4:
				/* be -- branch if registers equal.  */
				if (d->fe_reg[op_a] == d->fe_reg[op_b])
					pc = imm_12;
				break;
			case 0x5:
				/* bne -- branch if registers not equal.  */
				if (d->fe_reg[op_a] != d->fe_reg[op_b])
					pc = imm_12;
				break;
			case 0x6:
				/* bg -- branch if register r1 is greater than register r2.  */
				if (d->fe_reg[op_a] > d->fe_reg[op_b])
					pc = imm_12;
				break;
			case 0x7:
				/* ble -- branch if register r1 is <= register r2.  */
				if (d->fe_reg[op_a] <= d->fe_reg[op_b])
					pc = imm_12;
				break;
			case 0x8:
				/* bbs -- branch if bit r2 set in register r1.  r2 is treated as an immediate here.  */
				if (d->fe_reg[op_a] >> op_b & 1)
					pc = imm_12;
				break;
			case 0x9:
				/* bbc -- branch if bit clear, like above.  */
				if (!(d->fe_reg[op_a] >> op_b & 1))
					pc = imm_12;
				break;
			}
			break;
		case 0x01:
			/* ri7i12 format.  */
			switch (insn >> 19 & 3) {
			case 0x0:
				/* bei -- branch if register a equal to imm_7.  */
				if (d->fe_reg[op_a] == imm_7)
					pc = imm_12;
				break;
			case 0x1:
				/* bnei -- branch if register a not equal to imm_7.  */
				if (d->fe_reg[op_a] != imm_7)
					pc = imm_12;
				break;
			case 0x2:
				/* bgi -- branch if register a greater than imm_7.  */
				if (d->fe_reg[op_a] > imm_7)
					pc = imm_12;
				break;
			case 0x3:
				/* blei -- branch if register a <= imm_7.  */
				if (d->fe_reg[op_a] <= imm_7)
					pc = imm_12;
				break;
			}
			break;
		case 0x02:
			/* rrrrr format.  */
			/* mb or mbc -- move bits or move bits and clear others.  a is destination register,
			 * d is destination bit position, b is source register, e is source bit position,
			 * c is size in bits - 1.  */
			mask = (((uint32_t)2 << op_c) - 1) << op_d;
			tmp = d->fe_reg[op_b] >> op_e << op_d & mask;
			if (!(insn & 1))
				tmp |= d->fe_reg[op_a] & ~mask;
			d->fe_reg[op_a] = tmp;
			break;
		case 0x03:
			/* mbi -- move bits immediate.  Inserts (c+1) bits of imm_s11 to register a at position b.  */
			mask = (((uint32_t)2 << op_c) - 1) << op_b;
			tmp = imm_s11 << op_b & mask;
			tmp |= d->fe_reg[op_a] & ~mask;
			d->fe_reg[op_a] = tmp;
			break;
		case 0x04:
			/* li -- load immediate.  */
			d->fe_reg[op_a] = imm_s16;
			break;
		case 0x05:
			/* ai -- add immediate.  */
			d->fe_reg[op_a] = d->fe_reg[op_b] + imm_s16;
			break;
		case 0x06:
			/* a -- add reg b + imm_s11 + reg c, store to a.  */
			d->fe_reg[op_a] = d->fe_reg[op_b] + imm_s11 + d->fe_reg[op_c];
			break;
		case 0x07:
			/* s -- add reg b + imm_s11, subtract reg c, store to a.  */
			d->fe_reg[op_a] = d->fe_reg[op_b] + imm_s11 - d->fe_reg[op_c];
			break;
		case 0x08:
			/* sign --  store sign of reg b - reg c to reg a.  */
			d->fe_reg[op_a] = (d->fe_reg[op_b] > d->fe_reg[op_c] ? 1 : -1);
			break;
		}
		//printf("TRACE %03x %08x %08x %08x %08x\n", d->fe_state, insn, d->fe_reg[op_a], d->fe_reg[op_b], d->fe_reg[op_c]);
		d->fe_state = pc;
		any = true;
	}
	return any;
}

/* Runs the XY for some time.  Returns true if anything has been done.  */
static bool harddoom2_run_xy(HardDoom2State *d) {
	bool any = false;
	int iterations = 0x80;
	while (iterations--) {
		bool any_this = false;
		bool dst_busy = !!HARDDOOM2_XY_STATE_EXTR_DST_CMD_TYPE(d->xy_state);
		bool src_busy = !!HARDDOOM2_XY_STATE_EXTR_SRC_CMD_TYPE(d->xy_state);
		if (!(d->enable & HARDDOOM2_ENABLE_XY))
			return any;
		/* If we don't currently have a pending command, fetch one from FE.  */
		if (!HARDDOOM2_XY_STATE_EXTR_PENDING_CMD_TYPE(d->xy_state) && harddoom2_fifo_can_read(d, FIFO_XYCMD)) {
			int cmd = harddoom2_fifo_cmd_read(d, FIFO_XYCMD, &d->xy_pending_data) & 0xf;
			d->xy_state |= cmd << HARDDOOM2_XY_STATE_PENDING_CMD_TYPE_SHIFT;
			any_this = true;
		}
		/* Try to execute the pending command.  */
		int cmd = HARDDOOM2_XY_STATE_EXTR_PENDING_CMD_TYPE(d->xy_state);
		switch (cmd) {
			case HARDDOOM2_XYCMD_TYPE_SURF_DST_PT:
				/* Can change destination PT iff destination not busy.  */
				if (dst_busy)
					break;
				harddoom2_set_pt(d, HARDDOOM2_TLB_IDX_SURF_DST, d->xy_pending_data);
				goto clear_pending;
			case HARDDOOM2_XYCMD_TYPE_SURF_SRC_PT:
				/* Can change source PT iff destination not busy.  */
				if (src_busy)
					break;
				harddoom2_set_pt(d, HARDDOOM2_TLB_IDX_SURF_SRC, d->xy_pending_data);
				goto clear_pending;
			case HARDDOOM2_XYCMD_TYPE_SURF_DST_WIDTH:
				if (dst_busy)
					break;
				d->xy_state &= ~HARDDOOM2_XY_STATE_SURF_DST_WIDTH_MASK;
				d->xy_state |= (d->xy_pending_data & 0x3f) << HARDDOOM2_XY_STATE_SURF_DST_WIDTH_SHIFT;
				goto clear_pending;
			case HARDDOOM2_XYCMD_TYPE_SURF_SRC_WIDTH:
				if (src_busy)
					break;
				d->xy_state &= ~HARDDOOM2_XY_STATE_SURF_SRC_WIDTH_MASK;
				d->xy_state |= (d->xy_pending_data & 0x3f) << HARDDOOM2_XY_STATE_SURF_SRC_WIDTH_SHIFT;
				goto clear_pending;
			case HARDDOOM2_XYCMD_TYPE_INTERLOCK:
				/* If interlock signal from SW ready, consume it and the command.  Otherwise, wait.  */
				if (!d->fifo_xysync_state)
					break;
				d->fifo_xysync_state--;
				goto clear_pending;
			case HARDDOOM2_XYCMD_TYPE_WRITE_DST_H:
			case HARDDOOM2_XYCMD_TYPE_WRITE_DST_V:
			case HARDDOOM2_XYCMD_TYPE_READ_DST_V:
			case HARDDOOM2_XYCMD_TYPE_RMW_DST_V:
				/* Pass these to dst subunit if not busy.  */
				if (dst_busy)
					break;
				d->xy_state |= cmd << HARDDOOM2_XY_STATE_DST_CMD_TYPE_SHIFT;
				d->xy_dst_data = d->xy_pending_data;
				goto clear_pending;
			case HARDDOOM2_XYCMD_TYPE_READ_SRC_H:
			case HARDDOOM2_XYCMD_TYPE_READ_SRC_V:
				/* Pass these to src subunit if not busy.  */
				if (src_busy)
					break;
				d->xy_state |= cmd << HARDDOOM2_XY_STATE_SRC_CMD_TYPE_SHIFT;
				d->xy_src_data = d->xy_pending_data;
				goto clear_pending;
			clear_pending:
				d->xy_state &= ~HARDDOOM2_XY_STATE_PENDING_CMD_TYPE_MASK;
				any_this = true;
				break;
		}
		/* Try to step the dst subunit.  */
		if (HARDDOOM2_XY_STATE_EXTR_DST_CMD_TYPE(d->xy_state)) {
			int x = HARDDOOM2_XYCMD_DATA_EXTR_X(d->xy_dst_data);
			int y = HARDDOOM2_XYCMD_DATA_EXTR_Y(d->xy_dst_data);
			uint64_t addr;
			int count;
			switch (HARDDOOM2_XY_STATE_EXTR_DST_CMD_TYPE(d->xy_state)) {
				case HARDDOOM2_XYCMD_TYPE_WRITE_DST_H:
					count = HARDDOOM2_XYCMD_DATA_EXTR_WIDTH(d->xy_dst_data);
					if (!count)
						goto clear_dst;
					if (!harddoom2_fifo_can_write(d, FIFO_XYOUTW))
						break;
					if (harddoom2_translate_surf_xy(d, false, true, x, y, &addr)) {
						harddoom2_fifo_addr_write(d, FIFO_XYOUTW, addr);
						count--;
						x++;
						d->xy_dst_data = HARDDOOM2_XYCMD_DATA_H(x, y, count);
						any_this = true;
					}
					break;
				case HARDDOOM2_XYCMD_TYPE_WRITE_DST_V:
					count = HARDDOOM2_XYCMD_DATA_EXTR_HEIGHT(d->xy_dst_data);
					if (!count)
						goto clear_dst;
					if (!harddoom2_fifo_can_write(d, FIFO_XYOUTW))
						break;
					if (harddoom2_translate_surf_xy(d, false, true, x, y, &addr)) {
						harddoom2_fifo_addr_write(d, FIFO_XYOUTW, addr);
						count--;
						y++;
						d->xy_dst_data = HARDDOOM2_XYCMD_DATA_V(x, y, count);
						any_this = true;
					}
					break;
				case HARDDOOM2_XYCMD_TYPE_READ_DST_V:
					count = HARDDOOM2_XYCMD_DATA_EXTR_HEIGHT(d->xy_dst_data);
					if (!count)
						goto clear_dst;
					if (!harddoom2_fifo_can_write(d, FIFO_XYOUTR))
						break;
					if (harddoom2_translate_surf_xy(d, false, false, x, y, &addr)) {
						harddoom2_fifo_addr_write(d, FIFO_XYOUTR, addr);
						count--;
						y++;
						d->xy_dst_data = HARDDOOM2_XYCMD_DATA_V(x, y, count);
						any_this = true;
					}
					break;
				case HARDDOOM2_XYCMD_TYPE_RMW_DST_V:
					count = HARDDOOM2_XYCMD_DATA_EXTR_HEIGHT(d->xy_dst_data);
					if (!count)
						goto clear_dst;
					if (!harddoom2_fifo_can_write(d, FIFO_XYOUTR))
						break;
					if (!harddoom2_fifo_can_write(d, FIFO_XYOUTW))
						break;
					if (harddoom2_translate_surf_xy(d, false, true, x, y, &addr)) {
						harddoom2_fifo_addr_write(d, FIFO_XYOUTR, addr);
						harddoom2_fifo_addr_write(d, FIFO_XYOUTW, addr);
						count--;
						y++;
						d->xy_dst_data = HARDDOOM2_XYCMD_DATA_V(x, y, count);
						any_this = true;
					}
					break;
				clear_dst:
					d->xy_state &= ~HARDDOOM2_XY_STATE_DST_CMD_TYPE_MASK;
					any_this = true;
					break;
			}
		}
		/* Try to step the src subunit.  */
		if (HARDDOOM2_XY_STATE_EXTR_SRC_CMD_TYPE(d->xy_state)) {
			int x = HARDDOOM2_XYCMD_DATA_EXTR_X(d->xy_src_data);
			int y = HARDDOOM2_XYCMD_DATA_EXTR_Y(d->xy_src_data);
			uint64_t addr;
			int count;
			switch (HARDDOOM2_XY_STATE_EXTR_SRC_CMD_TYPE(d->xy_state)) {
				case HARDDOOM2_XYCMD_TYPE_READ_SRC_H:
					count = HARDDOOM2_XYCMD_DATA_EXTR_WIDTH(d->xy_src_data);
					if (!count)
						goto clear_src;
					if (!harddoom2_fifo_can_write(d, FIFO_XYOUTR))
						break;
					if (harddoom2_translate_surf_xy(d, true, false, x, y, &addr)) {
						harddoom2_fifo_addr_write(d, FIFO_XYOUTR, addr);
						count--;
						x++;
						d->xy_src_data = HARDDOOM2_XYCMD_DATA_H(x, y, count);
						any_this = true;
					}
					break;
				case HARDDOOM2_XYCMD_TYPE_READ_SRC_V:
					count = HARDDOOM2_XYCMD_DATA_EXTR_HEIGHT(d->xy_src_data);
					if (!count)
						goto clear_src;
					if (!harddoom2_fifo_can_write(d, FIFO_XYOUTR))
						break;
					if (harddoom2_translate_surf_xy(d, true, false, x, y, &addr)) {
						harddoom2_fifo_addr_write(d, FIFO_XYOUTR, addr);
						count--;
						y++;
						d->xy_src_data = HARDDOOM2_XYCMD_DATA_V(x, y, count);
						any_this = true;
					}
					break;
				clear_src:
					d->xy_state &= ~HARDDOOM2_XY_STATE_SRC_CMD_TYPE_MASK;
					any_this = true;
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
static bool harddoom2_run_sr(HardDoom2State *d) {
	bool any = false;
	if (!(d->enable & HARDDOOM2_ENABLE_SR))
		return false;
	while (1) {
		if (!harddoom2_fifo_can_read(d, FIFO_XYOUTR))
			return any;
		if (!harddoom2_fifo_can_write(d, FIFO_SROUT))
			return any;
		uint64_t addr = harddoom2_fifo_addr_read(d, FIFO_XYOUTR);
		int wptr = harddoom2_fifo_write(d, FIFO_SROUT);
		pci_dma_read(&d->dev, addr, &d->fifo_srout_data[wptr * HARDDOOM2_BLOCK_SIZE], HARDDOOM2_BLOCK_SIZE);
		any = true;
	}
	return any;
}

/* Ensures the flat cache is filled with the given line.  */
static void harddoom2_flat_fill_cache(HardDoom2State *d, int y) {
	if ((d->flat_cache_state & HARDDOOM2_FLAT_CACHE_STATE_VALID) &&
		((d->flat_cache_state & HARDDOOM2_FLAT_CACHE_STATE_TAG_MASK) == y)) {
		d->stats[HARDDOOM2_STAT_FLAT_CACHE_HIT]++;
		return;
	}
	d->stats[HARDDOOM2_STAT_FLAT_CACHE_MISS]++;
	uint64_t phys = (uint64_t)d->flat_addr << 8 | y << 6;
	pci_dma_read(&d->dev, phys, d->flat_cache, HARDDOOM2_FLAT_CACHE_SIZE);
	d->flat_cache_state = y | HARDDOOM2_FLAT_CACHE_STATE_VALID;
}

static bool harddoom2_tex_cached(HardDoom2State *d, uint32_t offset) {
	int tag = offset >> 6;
	return (d->tex_cache_state & HARDDOOM2_TEX_CACHE_STATE_VALID) &&
		(d->tex_cache_state & HARDDOOM2_TEX_CACHE_STATE_TAG_MASK) == tag;
}

/* Ensures the texture cache is filled with the line containing the given offset.
 * May cause a page fault and disable DRAW.  */
static bool harddoom2_tex_fill_cache(HardDoom2State *d, uint32_t offset) {
	offset &= ~0x3f;
	if (harddoom2_tex_cached(d, offset)) {
		d->stats[HARDDOOM2_STAT_TEX_CACHE_HIT]++;
		return true;
	}
	d->stats[HARDDOOM2_STAT_TEX_CACHE_MISS]++;
	if ((offset >> 6) <= HARDDOOM2_CMD_W7_B_EXTR_TEXTURE_LIMIT(d->tex_dims)) {
		uint64_t phys;
		if (!harddoom2_translate_addr(d, HARDDOOM2_TLB_IDX_TEXTURE, offset, &phys, false))
			return false;
		pci_dma_read(&d->dev, phys, d->tex_cache, HARDDOOM2_TEX_CACHE_SIZE);
	} else {
		memset(d->tex_cache, 0, sizeof d->tex_cache);
	}
	d->tex_cache_state &= ~HARDDOOM2_TEX_CACHE_STATE_TAG_MASK;
	d->tex_cache_state |= offset >> 6;
	d->tex_cache_state |= HARDDOOM2_TEX_CACHE_STATE_VALID;
	return true;
}

static uint32_t harddoom2_tex_offset(HardDoom2State *d, int x) {
	int height = HARDDOOM2_CMD_W7_B_EXTR_TEXTURE_HEIGHT(d->tex_dims);
	uint32_t coord = d->tex_column_coord[x];
	int icoord = coord >> 16;
	if (height)
		icoord %= height;
	uint32_t offset = d->tex_column_state[x] & HARDDOOM2_TEX_COLUMN_STATE_OFFSET_MASK;
	offset += icoord;
	return offset & HARDDOOM2_TEX_OFFSET_MASK;
}

static void harddoom2_tex_step(HardDoom2State *d, int x) {
	uint32_t height = HARDDOOM2_CMD_W7_B_EXTR_TEXTURE_HEIGHT(d->tex_dims);
	uint32_t coord = d->tex_column_coord[x];
	coord += d->tex_column_step[x];
	if (height)
		coord %= height << 16;
	d->tex_column_coord[x] = coord;
}

/* Runs the TEX for some time.  Returns true if anything has been done.  */
static bool harddoom2_run_tex(HardDoom2State *d) {
	bool any = false;
	if (!(d->enable & HARDDOOM2_ENABLE_TEX))
		return false;
	int x, h, pos;
	while (1) {
		if (!HARDDOOM2_TEX_DRAW_EXTR_LENGTH(d->tex_draw)) {
			if (!harddoom2_fifo_can_read(d, FIFO_TEXCMD))
				return any;
			uint32_t data;
			uint32_t cmd = harddoom2_fifo_cmd_read(d, FIFO_TEXCMD, &data);
			any = true;
			switch (cmd) {
			case HARDDOOM2_TEXCMD_TYPE_TEXTURE_PT:
				harddoom2_set_pt(d, HARDDOOM2_TLB_IDX_TEXTURE, data);
				d->tex_cache_state &= ~HARDDOOM2_TEX_CACHE_STATE_VALID;
				break;
			case HARDDOOM2_TEXCMD_TYPE_TEXTURE_DIMS:
				d->tex_dims = data;
				break;
			case HARDDOOM2_TEXCMD_TYPE_USTART:
				d->tex_ustart = data;
				break;
			case HARDDOOM2_TEXCMD_TYPE_USTEP:
				d->tex_ustep = data;
				break;
			case HARDDOOM2_TEXCMD_TYPE_START_COLUMN:
				x = HARDDOOM2_TEXCMD_DATA_EXTR_START_X(data);
				d->tex_mask |= 1ull << x;
				d->tex_column_state[x] = HARDDOOM2_TEXCMD_DATA_EXTR_OFFSET(data);
				d->tex_column_coord[x] = d->tex_ustart;
				d->tex_column_step[x] = d->tex_ustep;
				break;
			case HARDDOOM2_TEXCMD_TYPE_END_COLUMN:
				x = HARDDOOM2_TEXCMD_DATA_EXTR_END_X(data);
				d->tex_mask &= ~(1ull << x);
				break;
			case HARDDOOM2_TEXCMD_TYPE_DRAW_TEX:
				d->tex_draw = HARDDOOM2_TEXCMD_DATA_EXTR_DRAW_HEIGHT(data);
				break;
			}
		} else {
			h = HARDDOOM2_TEX_DRAW_EXTR_LENGTH(d->tex_draw);
			x = HARDDOOM2_TEX_DRAW_EXTR_X(d->tex_draw);
			while (h) {
				if (!harddoom2_fifo_can_write(d, FIFO_TEXOUT))
					return any;
				int wptr = harddoom2_fifo_wptr(d, FIFO_TEXOUT);
				if (d->tex_mask & 1ull << x) {
					int filled = d->tex_column_state[x] >> HARDDOOM2_TEX_COLUMN_STATE_SPEC_SHIFT;
					uint8_t byte;
					pos = d->tex_cache_state >> HARDDOOM2_TEX_CACHE_STATE_SPEC_POS_SHIFT;
					if (filled) {
						byte = d->tex_column_spec_data[x * HARDDOOM2_TEX_COLUMN_SPEC_DATA_SIZE + pos];
						filled--;
					} else {
						uint32_t offset = harddoom2_tex_offset(d, x);
						if (!harddoom2_tex_fill_cache(d, offset))
							return any;
						byte = d->tex_cache[offset & 0x3f];
						harddoom2_tex_step(d, x);
						while (filled < HARDDOOM2_TEX_COLUMN_SPEC_DATA_SIZE) {
							pos++;
							pos &= 0xf;
							offset = harddoom2_tex_offset(d, x);
							if (!harddoom2_tex_cached(d, offset)) {
								d->stats[HARDDOOM2_STAT_TEX_CACHE_SPEC_MISS]++;
								break;
							}
							d->stats[HARDDOOM2_STAT_TEX_CACHE_SPEC_HIT]++;
							d->tex_column_spec_data[x * HARDDOOM2_TEX_COLUMN_SPEC_DATA_SIZE + pos] = d->tex_cache[offset & 0x3f];
							filled++;
							harddoom2_tex_step(d, x);
						}
					}
					d->tex_column_state[x] &= ~HARDDOOM2_TEX_COLUMN_STATE_SPEC_MASK;
					d->tex_column_state[x] |= filled << HARDDOOM2_TEX_COLUMN_STATE_SPEC_SHIFT;
					d->fifo_texout_data[wptr * HARDDOOM2_BLOCK_SIZE + x] = byte;
				}
				x++;
				if (x == HARDDOOM2_BLOCK_SIZE) {
					d->fifo_texout_mask[wptr] = d->tex_mask;
					d->stats[HARDDOOM2_STAT_FIFO_TEXOUT_PIXEL] += ctpop64(d->tex_mask);
					harddoom2_fifo_write(d, FIFO_TEXOUT);
					x = 0;
					h--;
					pos = d->tex_cache_state >> HARDDOOM2_TEX_CACHE_STATE_SPEC_POS_SHIFT;
					pos++;
					d->tex_cache_state &= ~HARDDOOM2_TEX_CACHE_STATE_SPEC_POS_MASK;
					d->tex_cache_state |= (pos << HARDDOOM2_TEX_CACHE_STATE_SPEC_POS_SHIFT) & HARDDOOM2_TEX_CACHE_STATE_SPEC_POS_MASK;
				}
				d->tex_draw = x << HARDDOOM2_TEX_DRAW_X_SHIFT | h;
				any = true;
			}
			any = true;
		}
	}
}

/* Runs the FLAT for some time.  Returns true if anything has been done.  */
static bool harddoom2_run_flat(HardDoom2State *d) {
	bool any = false;
	if (!(d->enable & HARDDOOM2_ENABLE_FLAT))
		return false;
	while (1) {
		if (HARDDOOM2_FLAT_DRAW_EXTR_LENGTH(d->flat_draw)) {
			int num = HARDDOOM2_FLAT_DRAW_EXTR_LENGTH(d->flat_draw);
			int pos = HARDDOOM2_FLAT_DRAW_EXTR_X(d->flat_draw);
			while (num) {
				if (!harddoom2_fifo_can_write(d, FIFO_FLATOUT))
					return any;
				int wptr = harddoom2_fifo_wptr(d, FIFO_FLATOUT);
				int u = d->flat_ucoord >> 16;
				int v = d->flat_vcoord >> 16;
				d->flat_ucoord += d->flat_ustep;
				d->flat_vcoord += d->flat_vstep;
				d->flat_ucoord &= HARDDOOM2_FLAT_COORD_MASK;
				d->flat_vcoord &= HARDDOOM2_FLAT_COORD_MASK;
				harddoom2_flat_fill_cache(d, v);
				d->fifo_flatout_data[wptr * HARDDOOM2_BLOCK_SIZE + pos] = d->flat_cache[u];
				num--;
				pos++;
				d->stats[HARDDOOM2_STAT_FLAT_SPAN_PIXEL]++;
				if (pos == HARDDOOM2_BLOCK_SIZE || num == 0) {
					d->stats[HARDDOOM2_STAT_FLAT_SPAN_BLOCK]++;
					harddoom2_fifo_write(d, FIFO_FLATOUT);
					pos = 0;
				}
				d->flat_draw = pos << HARDDOOM2_FLAT_DRAW_X_SHIFT | num;
			}
			any = true;
		} else if (HARDDOOM2_FLAT_READ_EXTR_LENGTH(d->flat_read)) {
			int num = HARDDOOM2_FLAT_READ_EXTR_LENGTH(d->flat_read);
			int pos = HARDDOOM2_FLAT_READ_EXTR_POS(d->flat_read);
			while (num) {
				if (!harddoom2_fifo_can_write(d, FIFO_FLATOUT))
					return any;
				int wptr = harddoom2_fifo_write(d, FIFO_FLATOUT);
				harddoom2_flat_fill_cache(d, pos);
				memcpy(&d->fifo_flatout_data[wptr * HARDDOOM2_BLOCK_SIZE], d->flat_cache, HARDDOOM2_BLOCK_SIZE);
				num--;
				pos++;
				pos &= 0x3f;
				d->flat_read = pos << HARDDOOM2_FLAT_READ_POS_SHIFT | num;
			}
			any = true;
		} else {
			if (!harddoom2_fifo_can_read(d, FIFO_FLATCMD))
				return any;
			uint32_t data;
			uint32_t cmd = harddoom2_fifo_cmd_read(d, FIFO_FLATCMD, &data);
			any = true;
			switch (cmd) {
			case HARDDOOM2_FLATCMD_TYPE_FLAT_ADDR:
				d->flat_addr = data & HARDDOOM2_FLAT_ADDR_MASK;
				d->flat_cache_state &= ~HARDDOOM2_FLAT_CACHE_STATE_VALID;
				break;
			case HARDDOOM2_FLATCMD_TYPE_USTART:
				d->flat_ucoord = data & HARDDOOM2_FLAT_COORD_MASK;
				break;
			case HARDDOOM2_FLATCMD_TYPE_VSTART:
				d->flat_vcoord = data & HARDDOOM2_FLAT_COORD_MASK;
				break;
			case HARDDOOM2_FLATCMD_TYPE_USTEP:
				d->flat_ustep = data & HARDDOOM2_FLAT_COORD_MASK;
				break;
			case HARDDOOM2_FLATCMD_TYPE_VSTEP:
				d->flat_vstep = data & HARDDOOM2_FLAT_COORD_MASK;
				break;
			case HARDDOOM2_FLATCMD_TYPE_READ_FLAT:
				d->flat_read = data & HARDDOOM2_FLAT_READ_MASK;
				break;
			case HARDDOOM2_FLATCMD_TYPE_DRAW_SPAN:
				d->flat_draw = data & HARDDOOM2_FLAT_DRAW_LENGTH_MASK;
				break;
			}
		}
	}
}

/* Runs the FUZZ for some time.  Returns true if anything has been done.  */
static bool harddoom2_run_fuzz(HardDoom2State *d) {
	bool any = false;
	if (!(d->enable & HARDDOOM2_ENABLE_FUZZ))
		return false;
	while (1) {
		while (d->fuzz_draw) {
			if (!harddoom2_fifo_can_write(d, FIFO_FUZZOUT))
				return any;
			int wptr = harddoom2_fifo_write(d, FIFO_FUZZOUT);
			uint64_t mask = 0;
			for (int x = 0; x < HARDDOOM2_BLOCK_SIZE; x++) {
				if (0x121e650de224aull >> d->fuzz_position[x]++ & 1)
					mask |= 1ull << x;
				d->fuzz_position[x] %= 50;
			}
			d->fifo_fuzzout_mask[wptr] = mask;
			d->fuzz_draw--;
			any = true;
		}
		if (!harddoom2_fifo_can_read(d, FIFO_FUZZCMD))
			return any;
		uint32_t data;
		uint32_t cmd = harddoom2_fifo_cmd_read(d, FIFO_FUZZCMD, &data);
		any = true;
		int x, pos;
		switch (cmd) {
		case HARDDOOM2_FUZZCMD_TYPE_SET_COLUMN:
			x = HARDDOOM2_FUZZCMD_DATA_EXTR_X(data);
			pos = HARDDOOM2_FUZZCMD_DATA_EXTR_POS(data);
			d->fuzz_position[x] = pos;
			break;
		case HARDDOOM2_FUZZCMD_TYPE_DRAW_FUZZ:
			d->fuzz_draw = data & HARDDOOM2_FUZZ_DRAW_MASK;
			break;
		}
	}
}

static void harddoom2_og_bump_pos(HardDoom2State *d) {
	int pos = HARDDOOM2_OG_STATE_EXTR_BUF_POS(d->og_state);
	pos += HARDDOOM2_BLOCK_SIZE;
	pos &= 0xff;
	d->og_state &= ~HARDDOOM2_OG_STATE_BUF_POS_MASK;
	d->og_state |= pos << HARDDOOM2_OG_STATE_BUF_POS_SHIFT;
}

static bool harddoom2_og_buf_fill(HardDoom2State *d) {
	int cmd = HARDDOOM2_OG_STATE_EXTR_CMD(d->og_state);
	int pos = HARDDOOM2_OG_STATE_EXTR_BUF_POS(d->og_state);
	uint8_t *src;
	int flags = HARDDOOM2_OGCMD_DATA_EXTR_FLAGS(d->og_data);
	int rptr;
	switch (cmd) {
		case HARDDOOM2_OGCMD_TYPE_INIT_FUZZ:
		case HARDDOOM2_OGCMD_TYPE_DRAW_FUZZ:
			flags = 0;
			pos += 0x40;
			pos &= 0xff;
			goto read_sr;
		case HARDDOOM2_OGCMD_TYPE_COPY_V:
			pos &= 0x3f;
			flags = 0;
			goto read_sr;
		case HARDDOOM2_OGCMD_TYPE_COPY_H:
			flags = 0;
			goto read_sr;
		read_sr:
			if (!harddoom2_fifo_can_read(d, FIFO_SROUT))
				return false;
			rptr = harddoom2_fifo_read(d, FIFO_SROUT);
			src = &d->fifo_srout_data[rptr * HARDDOOM2_BLOCK_SIZE];
			break;
		case HARDDOOM2_OGCMD_TYPE_DRAW_SPAN:
			if (!harddoom2_fifo_can_read(d, FIFO_FLATOUT))
				return false;
			rptr = harddoom2_fifo_read(d, FIFO_FLATOUT);
			src = &d->fifo_flatout_data[rptr * HARDDOOM2_BLOCK_SIZE];
			break;
		case HARDDOOM2_OGCMD_TYPE_DRAW_TEX:
			if (!harddoom2_fifo_can_read(d, FIFO_TEXOUT))
				return false;
			rptr = harddoom2_fifo_read(d, FIFO_TEXOUT);
			src = &d->fifo_texout_data[rptr * HARDDOOM2_BLOCK_SIZE];
			d->og_mask = d->fifo_texout_mask[rptr];
			break;
		default:
			return true;
	}
	if (flags) {
		int i, j;
		if (flags & HARDDOOM2_OGCMD_FLAG_TRANSLATION)
			d->stats[HARDDOOM2_STAT_OG_TRANSLATION_BLOCK]++;
		if (flags & HARDDOOM2_OGCMD_FLAG_COLORMAP)
			d->stats[HARDDOOM2_STAT_OG_COLORMAP_BLOCK]++;
		for (i = pos, j = 0; j < HARDDOOM2_BLOCK_SIZE; i++, j++, i &= 0xff) {
			uint8_t byte = src[j];
			if (flags & HARDDOOM2_OGCMD_FLAG_TRANSLATION)
				byte = d->og_translation[byte];
			if (flags & HARDDOOM2_OGCMD_FLAG_COLORMAP)
				byte = d->og_colormap[byte];
			d->og_buf[i] = byte;
		}
	} else {
		if (pos + HARDDOOM2_BLOCK_SIZE > sizeof d->og_buf) {
			int mid = sizeof d->og_buf - pos;
			memcpy(d->og_buf + pos, src, mid);
			memcpy(d->og_buf, src + mid, HARDDOOM2_BLOCK_SIZE - mid);
		} else {
			memcpy(d->og_buf + pos, src, HARDDOOM2_BLOCK_SIZE);
		}
	}
	return true;
}

static bool harddoom2_og_send(HardDoom2State *d) {
	int cmd = HARDDOOM2_OG_STATE_EXTR_CMD(d->og_state);
	int pos = HARDDOOM2_OG_STATE_EXTR_BUF_POS(d->og_state);
	pos &= 0xc0;
	if (!harddoom2_fifo_free(d, FIFO_OGOUT))
		return false;
	int wptr = harddoom2_fifo_write(d, FIFO_OGOUT);
	memcpy(&d->fifo_ogout_data[wptr * HARDDOOM2_BLOCK_SIZE], d->og_buf + pos, HARDDOOM2_BLOCK_SIZE);
	d->fifo_ogout_mask[wptr] = d->og_mask;
	d->stats[HARDDOOM2_STAT_FIFO_OGOUT_PIXEL] += ctpop64(d->og_mask);
	int flags = HARDDOOM2_OGCMD_DATA_EXTR_FLAGS(d->og_data);
	switch (cmd) {
	case HARDDOOM2_OGCMD_TYPE_DRAW_BUF_H:
	case HARDDOOM2_OGCMD_TYPE_DRAW_BUF_V:
		d->stats[HARDDOOM2_STAT_OG_DRAW_BUF_BLOCK]++;
		d->stats[HARDDOOM2_STAT_OG_DRAW_BUF_PIXEL] += ctpop64(d->og_mask);
		break;
	case HARDDOOM2_OGCMD_TYPE_COPY_H:
	case HARDDOOM2_OGCMD_TYPE_COPY_V:
		d->stats[HARDDOOM2_STAT_OG_COPY_BLOCK]++;
		d->stats[HARDDOOM2_STAT_OG_COPY_PIXEL] += ctpop64(d->og_mask);
		break;
	case HARDDOOM2_OGCMD_TYPE_DRAW_FUZZ:
		d->stats[HARDDOOM2_STAT_OG_FUZZ_PIXEL] += ctpop64(d->og_mask);
		break;
	case HARDDOOM2_OGCMD_TYPE_DRAW_SPAN:
	case HARDDOOM2_OGCMD_TYPE_DRAW_TEX:
		if (flags & HARDDOOM2_OGCMD_FLAG_TRANSLATION)
			d->stats[HARDDOOM2_STAT_OG_TRANSLATION_PIXEL] += ctpop64(d->og_mask);
		if (flags & HARDDOOM2_OGCMD_FLAG_COLORMAP)
			d->stats[HARDDOOM2_STAT_OG_COLORMAP_PIXEL] += ctpop64(d->og_mask);
		break;
	}
	return true;
}

/* Runs the OG for some time.  Returns true if anything has been done.  */
static bool harddoom2_run_og(HardDoom2State *d) {
	bool any = false;
	if (!(d->enable & HARDDOOM2_ENABLE_OG))
		return false;
	while (1) {
		int cmd = HARDDOOM2_OG_STATE_EXTR_CMD(d->og_state);
		if (!cmd) {
			if (!harddoom2_fifo_can_read(d, FIFO_OGCMD))
				return any;
			cmd = harddoom2_fifo_cmd_read(d, FIFO_OGCMD, &d->og_data) & HARDDOOM2_OG_STATE_CMD_MASK;
			d->og_state |= cmd;
			any = true;
		}
		switch (cmd) {
		case HARDDOOM2_OGCMD_TYPE_NOP:
			break;
		case HARDDOOM2_OGCMD_TYPE_INTERLOCK:
		case HARDDOOM2_OGCMD_TYPE_FENCE:
		case HARDDOOM2_OGCMD_TYPE_PING:
		case HARDDOOM2_OGCMD_TYPE_TRANMAP_PT:
			if (!harddoom2_fifo_can_write(d, FIFO_SWCMD))
				return any;
			harddoom2_fifo_cmd_write(d, FIFO_SWCMD, cmd, d->og_data);
			break;
		case HARDDOOM2_OGCMD_TYPE_FILL_COLOR:
			memset(d->og_buf, d->og_data & 0xff, sizeof d->og_buf);
			break;
		case HARDDOOM2_OGCMD_TYPE_COLORMAP_ADDR:
			pci_dma_read(&d->dev, (uint64_t)d->og_data << 8, d->og_colormap, sizeof d->og_colormap);
			break;
		case HARDDOOM2_OGCMD_TYPE_TRANSLATION_ADDR:
			pci_dma_read(&d->dev, (uint64_t)d->og_data << 8, d->og_translation, sizeof d->og_translation);
			break;
		case HARDDOOM2_OGCMD_TYPE_DRAW_BUF_H:
		case HARDDOOM2_OGCMD_TYPE_COPY_H:
		case HARDDOOM2_OGCMD_TYPE_DRAW_SPAN: {
			/* The horizontal draw commands.  */
			int x = HARDDOOM2_OGCMD_DATA_EXTR_X(d->og_data);
			int w = HARDDOOM2_OGCMD_DATA_EXTR_H_WIDTH(d->og_data);
			while (w) {
				uint32_t state = d->og_state & HARDDOOM2_OG_STATE_STATE_MASK;
				if (state == HARDDOOM2_OG_STATE_STATE_INIT) {
					/* If this is the first iteration, initialize some things and tell the SW unit about the draw.  */
					int num = (w + x + 0x3f) >> 6;
					if (!harddoom2_fifo_can_write(d, FIFO_SWCMD))
						return any;
					int swcmd = HARDDOOM2_SWCMD_TYPE_DRAW;
					if (cmd == HARDDOOM2_OGCMD_TYPE_DRAW_SPAN && (HARDDOOM2_OGCMD_DATA_EXTR_FLAGS(d->og_data) & HARDDOOM2_OGCMD_FLAG_TRANMAP))
						swcmd = HARDDOOM2_SWCMD_TYPE_DRAW_TRANSPARENT;
					harddoom2_fifo_cmd_write(d, FIFO_SWCMD, swcmd, num);
					state = HARDDOOM2_OG_STATE_STATE_RUNNING;
					d->og_state &= ~HARDDOOM2_OG_STATE_BUF_POS_MASK;
					if (cmd != HARDDOOM2_OGCMD_TYPE_DRAW_BUF_H) {
						int pos = 0;
						if (cmd == HARDDOOM2_OGCMD_TYPE_COPY_H)
							pos = HARDDOOM2_OGCMD_DATA_EXTR_SRC_OFFSET(d->og_data);
						pos = x - pos;
						if (pos < 0) {
							state = HARDDOOM2_OG_STATE_STATE_PREFILL;
							pos += HARDDOOM2_BLOCK_SIZE;
						}
						d->og_state |= pos << HARDDOOM2_OG_STATE_BUF_POS_SHIFT;
					}
				} else if (state == HARDDOOM2_OG_STATE_STATE_PREFILL) {
					/* For some source alignments, we will need to pre-fill an extra source block.  */
					if (!harddoom2_og_buf_fill(d))
						return any;
					harddoom2_og_bump_pos(d);
					state = HARDDOOM2_OG_STATE_STATE_RUNNING;
				} else if (state == HARDDOOM2_OG_STATE_STATE_RUNNING) {
					/* Now, fill the buffer, if we need to.  */
					int pos = HARDDOOM2_OG_STATE_EXTR_BUF_POS(d->og_state);
					if (cmd != HARDDOOM2_OGCMD_TYPE_DRAW_BUF_H && (pos & 0x3f) < x + w) {
						if (!harddoom2_og_buf_fill(d))
							return any;
					}
					uint64_t mask = ~0ull << x;
					if (x + w < HARDDOOM2_BLOCK_SIZE)
						mask &= (1ull << (x + w)) - 1;
					d->og_mask = mask;
					state = HARDDOOM2_OG_STATE_STATE_FILLED;
				} else if (state == HARDDOOM2_OG_STATE_STATE_FILLED) {
					/* Everything ready, try to submit to SW.  */
					if (!harddoom2_og_send(d))
						return any;
					w -= (HARDDOOM2_BLOCK_SIZE - x);
					if (w < 0)
						w = 0;
					x = 0;
					d->og_data &= ~0x3ffff;
					d->og_data |= w << 6;
					harddoom2_og_bump_pos(d);
					state = HARDDOOM2_OG_STATE_STATE_RUNNING;
				} else {
					return any;
				}
				d->og_state &= ~HARDDOOM2_OG_STATE_STATE_MASK;
				d->og_state |= state;
				any = true;
			}
			/* Nothing left to do.  */
			break;
		}
		case HARDDOOM2_OGCMD_TYPE_DRAW_BUF_V:
		case HARDDOOM2_OGCMD_TYPE_COPY_V: {
			/* The vertical draw commands.  */
			int x = HARDDOOM2_OGCMD_DATA_EXTR_X(d->og_data);
			int w = HARDDOOM2_OGCMD_DATA_EXTR_V_WIDTH(d->og_data);
			int h = HARDDOOM2_OGCMD_DATA_EXTR_V_HEIGHT(d->og_data);
			while (h) {
				uint32_t state = d->og_state & HARDDOOM2_OG_STATE_STATE_MASK;
				if (state == HARDDOOM2_OG_STATE_STATE_INIT) {
					/* If this is the first iteration, initialize some things and tell the SW unit about the draw.  */
					if (!harddoom2_fifo_can_write(d, FIFO_SWCMD))
						return any;
					harddoom2_fifo_cmd_write(d, FIFO_SWCMD, HARDDOOM2_SWCMD_TYPE_DRAW, h);
					d->og_state &= ~HARDDOOM2_OG_STATE_BUF_POS_MASK;
					int pos = 0;
					if (cmd == HARDDOOM2_OGCMD_TYPE_COPY_V)
						pos = HARDDOOM2_OGCMD_DATA_EXTR_SRC_OFFSET(d->og_data);
					pos = x - pos;
					if (pos < 0)
						pos += 2 * HARDDOOM2_BLOCK_SIZE;
					d->og_state |= pos << HARDDOOM2_OG_STATE_BUF_POS_SHIFT;
					state = HARDDOOM2_OG_STATE_STATE_RUNNING;
				} else if (state == HARDDOOM2_OG_STATE_STATE_RUNNING) {
					/* Now, fill the buffer, if we need to.  */
					if (cmd != HARDDOOM2_OGCMD_TYPE_DRAW_BUF_V) {
						if (!harddoom2_og_buf_fill(d))
							return any;
					}
					uint64_t mask = ~0ull << x;
					if (x + w < HARDDOOM2_BLOCK_SIZE)
						mask &= (1ull << (x + w)) - 1;
					d->og_mask = mask;
					state = HARDDOOM2_OG_STATE_STATE_FILLED;
				} else if (state == HARDDOOM2_OG_STATE_STATE_FILLED) {
					/* Everything ready, try to submit to SW.  */
					if (!harddoom2_og_send(d))
						return any;
					h--;
					d->og_data &= ~0xfff000;
					d->og_data |= h << 12;
					state = HARDDOOM2_OG_STATE_STATE_RUNNING;
				} else {
					return any;
				}
				d->og_state &= ~HARDDOOM2_OG_STATE_STATE_MASK;
				d->og_state |= state;
				any = true;
			}
			/* Nothing left to do.  */
			break;
		}
		case HARDDOOM2_OGCMD_TYPE_READ_FLAT: {
			if (!harddoom2_fifo_can_read(d, FIFO_FLATOUT))
				return any;
			const uint8_t *src = &d->fifo_flatout_data[harddoom2_fifo_read(d, FIFO_FLATOUT) * HARDDOOM2_BLOCK_SIZE];
			for (int i = 0; i < 4; i++)
				memcpy(d->og_buf + i * HARDDOOM2_BLOCK_SIZE, src, HARDDOOM2_BLOCK_SIZE);
			break;
		}
		case HARDDOOM2_OGCMD_TYPE_DRAW_FUZZ: {
			int h = HARDDOOM2_OGCMD_DATA_EXTR_TF_HEIGHT(d->og_data);
			while (h) {
				uint32_t state = d->og_state & HARDDOOM2_OG_STATE_STATE_MASK;
				if (state == HARDDOOM2_OG_STATE_STATE_INIT) {
					/* If this is the first iteration, tell the SW unit about the draw.  */
					if (!harddoom2_fifo_can_write(d, FIFO_SWCMD))
						return any;
					harddoom2_fifo_cmd_write(d, FIFO_SWCMD, HARDDOOM2_SWCMD_TYPE_DRAW, h);
					state = HARDDOOM2_OG_STATE_STATE_PREFILL;
				} else if (state == HARDDOOM2_OG_STATE_STATE_PREFILL) {
					/* Fetch the mask from FUZZ.  */
					if (!harddoom2_fifo_can_read(d, FIFO_FUZZOUT))
						return any;
					int rptr = harddoom2_fifo_read(d, FIFO_FUZZOUT);
					d->og_fuzz_mask = d->fifo_fuzzout_mask[rptr];
					state = HARDDOOM2_OG_STATE_STATE_RUNNING;
				} else if (state == HARDDOOM2_OG_STATE_STATE_RUNNING) {
					/* Now, fill the buffer.  */
					if (!harddoom2_og_buf_fill(d))
						return any;
					/* And perform the fuzz.  */
					int i;
					int pos = HARDDOOM2_OG_STATE_EXTR_BUF_POS(d->og_state);
					pos &= 0xc0;
					int prev = (pos - 0x40) & 0xc0;
					int next = (pos + 0x40) & 0xc0;
					for (i = 0; i < HARDDOOM2_BLOCK_SIZE; i++) {
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
					state = HARDDOOM2_OG_STATE_STATE_FILLED;
				} else if (state == HARDDOOM2_OG_STATE_STATE_FILLED) {
					/* Everything ready, try to submit to SW.  */
					if (!harddoom2_og_send(d))
						return any;
					harddoom2_og_bump_pos(d);
					h--;
					d->og_data &= ~0xfff;
					d->og_data |= h;
					state = HARDDOOM2_OG_STATE_STATE_PREFILL;
				} else {
					return any;
				}
				d->og_state &= ~HARDDOOM2_OG_STATE_STATE_MASK;
				d->og_state |= state;
				any = true;
			}
			/* Nothing left to do.  */
			break;
		}
		case HARDDOOM2_OGCMD_TYPE_INIT_FUZZ:
			while (1) {
				uint32_t state = d->og_state & HARDDOOM2_OG_STATE_STATE_MASK;
				if (state == HARDDOOM2_OG_STATE_STATE_INIT) {
					d->og_mask = 0;
					d->og_state &= ~HARDDOOM2_OG_STATE_BUF_POS_MASK;
					state = HARDDOOM2_OG_STATE_STATE_PREFILL;
				} else if (state == HARDDOOM2_OG_STATE_STATE_PREFILL) {
					if (!harddoom2_og_buf_fill(d))
						return any;
					harddoom2_og_bump_pos(d);
					state = HARDDOOM2_OG_STATE_STATE_RUNNING;
				} else if (state == HARDDOOM2_OG_STATE_STATE_RUNNING) {
					if (!harddoom2_og_buf_fill(d))
						return any;
					harddoom2_og_bump_pos(d);
					break;
				}
				d->og_state &= ~HARDDOOM2_OG_STATE_STATE_MASK;
				d->og_state |= state;
				any = true;
			}
			break;
		case HARDDOOM2_OGCMD_TYPE_FUZZ_COLUMN:
			d->og_mask ^= 1ull << HARDDOOM2_OGCMD_DATA_EXTR_X(d->og_data);
			break;
		case HARDDOOM2_OGCMD_TYPE_DRAW_TEX: {
			int h = HARDDOOM2_OGCMD_DATA_EXTR_TF_HEIGHT(d->og_data);
			while (h) {
				uint32_t state = d->og_state & HARDDOOM2_OG_STATE_STATE_MASK;
				if (state == HARDDOOM2_OG_STATE_STATE_INIT) {
					/* If this is the first iteration, initialize some things and tell the SW unit about the draw.  */
					if (!harddoom2_fifo_free(d, FIFO_SWCMD))
						return any;
					int swcmd = HARDDOOM2_SWCMD_TYPE_DRAW;
					if (HARDDOOM2_OGCMD_DATA_EXTR_FLAGS(d->og_data) & HARDDOOM2_OGCMD_FLAG_TRANMAP)
						swcmd = HARDDOOM2_SWCMD_TYPE_DRAW_TRANSPARENT;
					harddoom2_fifo_cmd_write(d, FIFO_SWCMD, swcmd, h);
					d->og_state &= ~HARDDOOM2_OG_STATE_BUF_POS_MASK;
					state = HARDDOOM2_OG_STATE_STATE_RUNNING;
				} else if (state == HARDDOOM2_OG_STATE_STATE_RUNNING) {
					/* Now, fill the buffer.  */
					if (!harddoom2_og_buf_fill(d))
						return any;
					state = HARDDOOM2_OG_STATE_STATE_FILLED;
				} else if (state == HARDDOOM2_OG_STATE_STATE_FILLED) {
					/* Everything ready, try to submit to SW.  */
					if (!harddoom2_og_send(d))
						return any;
					h--;
					d->og_data &= ~0xfff;
					d->og_data |= h;
					state = HARDDOOM2_OG_STATE_STATE_RUNNING;
				} else {
					return any;
				}
				d->og_state &= ~HARDDOOM2_OG_STATE_STATE_MASK;
				d->og_state |= state;
				any = true;
			}
			/* Nothing left to do.  */
			break;
		}
		default:
			return any;
		}
		d->og_state &= ~HARDDOOM2_OG_STATE_CMD_MASK;
		d->og_state &= ~HARDDOOM2_OG_STATE_STATE_MASK;
		any = true;
	}
}

/* Looks up a pair of colors in the tranmap, returns true if succeeded (no page fault).  */
static bool harddoom2_sw_tranmap_lookup(HardDoom2State *d, uint8_t a, uint8_t b, uint8_t *res) {
	int line = a & 0xf;
	int tag = (b >> 6 & 3) | (a >> 2 & 0x3c);
	int byte = b & 0x3f;
	if (d->sw_cache_state[line] != (tag | HARDDOOM2_SW_CACHE_STATE_VALID)) {
		uint32_t offset = a << 8 | b;
		offset &= ~0x3f;
		uint64_t phys;
		if (!harddoom2_translate_addr(d, HARDDOOM2_TLB_IDX_TRANMAP, offset, &phys, false)) {
			d->enable &= ~HARDDOOM2_ENABLE_SW;
			return false;
		}
		pci_dma_read(&d->dev, phys, &d->sw_cache[line * HARDDOOM2_SW_CACHE_LINE_SIZE], HARDDOOM2_SW_CACHE_LINE_SIZE);
		d->sw_cache_state[line] = tag | HARDDOOM2_SW_CACHE_STATE_VALID;
		d->stats[HARDDOOM2_STAT_SW_TRANMAP_MISS]++;
	} else {
		d->stats[HARDDOOM2_STAT_SW_TRANMAP_HIT]++;
	}
	*res = d->sw_cache[line * HARDDOOM2_SW_CACHE_LINE_SIZE + byte];
	return true;
}

/* Runs the SW for some time.  Returns true if anything has been done.  */
static bool harddoom2_run_sw(HardDoom2State *d) {
	bool any = false;
	if (!(d->enable & HARDDOOM2_ENABLE_SW))
		return false;
	while (1) {
		int cmd = HARDDOOM2_SW_STATE_EXTR_CMD(d->sw_state);
		if (!cmd) {
			if (!harddoom2_fifo_can_read(d, FIFO_SWCMD))
				return any;
			uint32_t data;
			cmd = harddoom2_fifo_cmd_read(d, FIFO_SWCMD, &data);
			any = true;
			switch (cmd) {
			case HARDDOOM2_SWCMD_TYPE_INTERLOCK:
				d->sw_state = cmd << HARDDOOM2_SW_STATE_CMD_SHIFT;
				break;
			case HARDDOOM2_SWCMD_TYPE_FENCE:
				d->fence_counter++;
				d->stats[HARDDOOM2_STAT_SW_FENCE]++;
				if (d->fence_counter == d->fence_wait) {
					d->stats[HARDDOOM2_STAT_FENCE_INTR]++;
					d->intr |= HARDDOOM2_INTR_FENCE;
				}
				break;
			case HARDDOOM2_SWCMD_TYPE_PING:
				d->intr |= HARDDOOM2_INTR_PONG_SYNC;
				d->stats[HARDDOOM2_STAT_SW_PING_SYNC]++;
				break;
			case HARDDOOM2_SWCMD_TYPE_TRANMAP_PT:
				harddoom2_set_pt(d, HARDDOOM2_TLB_IDX_TRANMAP, data);
				for (int i = 0; i < HARDDOOM2_SW_CACHE_LINES; i++)
					d->sw_cache_state[i] &= ~HARDDOOM2_SW_CACHE_STATE_VALID;
				break;
			case HARDDOOM2_SWCMD_TYPE_DRAW:
			case HARDDOOM2_SWCMD_TYPE_DRAW_TRANSPARENT:
				d->sw_state = cmd << HARDDOOM2_SW_STATE_CMD_SHIFT | (data & HARDDOOM2_SW_STATE_DRAW_MASK);
				break;
			}
		} else {
			switch (cmd) {
			case HARDDOOM2_SWCMD_TYPE_INTERLOCK:
				if (d->fifo_xysync_state == HARDDOOM2_FIFO_XYSYNC_STATE_MASK)
					return any;
				d->stats[HARDDOOM2_STAT_FIFO_XYSYNC]++;
				d->fifo_xysync_state++;
				break;
			case HARDDOOM2_SWCMD_TYPE_DRAW:
			case HARDDOOM2_SWCMD_TYPE_DRAW_TRANSPARENT: {
				while (1) {
					if (d->sw_state & HARDDOOM2_SW_STATE_BLOCK_PENDING) {
						if (cmd == HARDDOOM2_SWCMD_TYPE_DRAW_TRANSPARENT) {
							int x = HARDDOOM2_SW_STATE_EXTR_X(d->sw_state);
							while (x < HARDDOOM2_BLOCK_SIZE) {
								if (d->sw_mask >> x & 1) {
									uint8_t res;
									if (!harddoom2_sw_tranmap_lookup(d, d->sw_buf[x], d->sw_old[x], &res))
										return any;
									d->sw_buf[x] = res;
								}
								d->stats[HARDDOOM2_STAT_SW_TRANMAP_PIXEL]++;
								x++;
								d->sw_state &= ~HARDDOOM2_SW_STATE_X_MASK;
								if (x != HARDDOOM2_BLOCK_SIZE)
									d->sw_state |= x << HARDDOOM2_SW_STATE_X_SHIFT;
								any = true;
							}
						}
						d->sw_state &= ~HARDDOOM2_SW_STATE_X_MASK;
						d->sw_state &= ~HARDDOOM2_SW_STATE_BLOCK_PENDING;
						int pos = 0;
						uint64_t mask = d->sw_mask;
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
								d->stats[HARDDOOM2_STAT_SW_XFER]++;
								pci_dma_write(&d->dev, d->sw_addr+pos, d->sw_buf+pos, len);
								if (pos+len > 64) {
									abort();
								}
								pos += len;
								if (len == 64)
									break;
								mask >>= len;
							}
						}
					} else {
						int num = d->sw_state & HARDDOOM2_SW_STATE_DRAW_MASK;
						if (!num)
							break;
						/* See if we have both data and address.  */
						if (!harddoom2_fifo_can_read(d, FIFO_XYOUTW))
							return any;
						if (!harddoom2_fifo_can_read(d, FIFO_OGOUT))
							return any;
						d->sw_addr = harddoom2_fifo_addr_read(d, FIFO_XYOUTW);
						int rptr = harddoom2_fifo_read(d, FIFO_OGOUT);
						d->sw_mask = d->fifo_ogout_mask[rptr];
						memcpy(d->sw_buf, d->fifo_ogout_data + rptr * HARDDOOM2_BLOCK_SIZE, HARDDOOM2_BLOCK_SIZE);
						d->sw_state |= HARDDOOM2_SW_STATE_BLOCK_PENDING;
						pci_dma_read(&d->dev, d->sw_addr, d->sw_old, HARDDOOM2_BLOCK_SIZE);
						num--;
						d->sw_state &= ~HARDDOOM2_SW_STATE_DRAW_MASK;
						d->sw_state |= num;
						if (cmd == HARDDOOM2_SWCMD_TYPE_DRAW_TRANSPARENT)
							d->stats[HARDDOOM2_STAT_SW_TRANMAP_BLOCK]++;
					}
					any = true;
				}
				break;
			}
			default:
				return any;
			}
			any = true;
			d->sw_state = 0;
		}
	}
}

static void *harddoom2_thread(void *opaque)
{
	HardDoom2State *d = opaque;
	qemu_mutex_lock(&d->mutex);
	while (1) {
		if (d->stopping)
			break;
		qemu_mutex_unlock(&d->mutex);
		qemu_mutex_lock_iothread();
		qemu_mutex_lock(&d->mutex);
		bool idle = true;
		if (harddoom2_run_cmd_fetch(d))
			idle = false;
		if (harddoom2_run_fe(d))
			idle = false;
		if (harddoom2_run_xy(d))
			idle = false;
		if (harddoom2_run_sr(d))
			idle = false;
		if (harddoom2_run_tex(d))
			idle = false;
		if (harddoom2_run_flat(d))
			idle = false;
		if (harddoom2_run_fuzz(d))
			idle = false;
		if (harddoom2_run_og(d))
			idle = false;
		if (harddoom2_run_sw(d))
			idle = false;
		harddoom2_status_update(d);
		qemu_mutex_unlock_iothread();
		if (idle) {
			qemu_cond_wait(&d->cond, &d->mutex);
		}
	}
	qemu_mutex_unlock(&d->mutex);
	return 0;
}

static void harddoom2_init(PCIDevice *pci_dev, Error **errp)
{
	HardDoom2State *d = DO_UPCAST(HardDoom2State, dev, pci_dev);
	uint8_t *pci_conf = d->dev.config;

	pci_config_set_interrupt_pin(pci_conf, 1);

	memory_region_init_io(&d->mmio, OBJECT(d), &harddoom2_mmio_ops, d, "harddoom2", 0x2000);
	pci_register_bar(&d->dev, 0, PCI_BASE_ADDRESS_SPACE_MEMORY, &d->mmio);

	qemu_mutex_init(&d->mutex);
	qemu_cond_init(&d->cond);

	harddoom2_power_reset(&pci_dev->qdev);

	d->stopping = false;
	qemu_thread_create(&d->thread, "harddoom2", harddoom2_thread,
			d, QEMU_THREAD_JOINABLE);
}

static void harddoom2_exit(PCIDevice *pci_dev)
{
	HardDoom2State *d = DO_UPCAST(HardDoom2State, dev, pci_dev);
	qemu_mutex_lock(&d->mutex);
	d->stopping = true;
	qemu_mutex_unlock(&d->mutex);
	qemu_cond_signal(&d->cond);
	qemu_thread_join(&d->thread);
	qemu_cond_destroy(&d->cond);
	qemu_mutex_destroy(&d->mutex);
}

static void harddoom2_class_init(ObjectClass *klass, void *data)
{
	DeviceClass *dc = DEVICE_CLASS(klass);
	PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);

	k->realize = harddoom2_init;
	k->exit = harddoom2_exit;
	k->vendor_id = HARDDOOM2_VENDOR_ID;
	k->device_id = HARDDOOM2_DEVICE_ID;
	k->class_id = PCI_CLASS_PROCESSOR_CO;
	dc->reset = harddoom2_power_reset;
	dc->vmsd = &vmstate_harddoom2;
}

static InterfaceInfo harddoom2_interfaces[] = {
	{ INTERFACE_CONVENTIONAL_PCI_DEVICE },
	{ },
};

static TypeInfo harddoom2_info = {
	.name          = "harddoom2",
	.parent        = TYPE_PCI_DEVICE,
	.instance_size = sizeof(HardDoom2State),
	.class_init    = harddoom2_class_init,
	.interfaces    = harddoom2_interfaces,
};

static void harddoom2_register_types(void)
{
	type_register_static(&harddoom2_info);
}

type_init(harddoom2_register_types)
