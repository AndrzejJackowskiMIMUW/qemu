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
#include "hw/hw.h"
#include "hw/pci/pci.h"

enum {
	TLB_SURF_DST = 0,
	TLB_SURF_SRC = 1,
	TLB_TEXTURE = 2,
};

typedef struct {
	PCIDevice dev;
	/* These correspond directly to the registers listed in harddoom.h.  */
	uint32_t enable;
	uint32_t status;
	uint32_t intr;
	uint32_t intr_enable;
	uint32_t sync_last;
	uint32_t sync_intr;
	uint32_t cmd_read_ptr;
	uint32_t cmd_write_ptr;
	uint32_t fifo_state;
	uint32_t state_pt[3];
	uint32_t state_colormap_addr[2];
	uint32_t state_flat_addr;
	uint32_t state_surf_dims;
	uint32_t state_texture_dims;
	uint32_t state_fill_color;
	uint32_t state_xy[2];
	uint32_t state_draw_params;
	uint32_t state_uvstart[2];
	uint32_t state_uvstep[2];
	uint32_t tlb_tag[3];
	uint32_t tlb_pte[3];
	uint32_t cache_state;
	uint32_t draw_x_cur;
	uint32_t draw_y_cur;
	uint32_t draw_x_restart;
	uint32_t draw_end;
	uint32_t draw_texcoord_u;
	uint32_t draw_texcoord_v;
	uint32_t draw_state;
	uint32_t draw_line_size;
	uint32_t draw_column_offset;
	uint8_t cache_data_texture[HARDDOOM_CACHE_SIZE];
	uint8_t cache_data_flat[HARDDOOM_CACHE_SIZE];
	uint8_t cache_data_colormap[HARDDOOM_CACHE_SIZE];
	uint8_t cache_data_translation[HARDDOOM_CACHE_SIZE];
	uint32_t fifo_cmd[HARDDOOM_FIFO_CMD_NUM];
	QEMUTimer *timer;
	MemoryRegion mmio;
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
		VMSTATE_UINT32(sync_last, HardDoomState),
		VMSTATE_UINT32(sync_intr, HardDoomState),
		VMSTATE_UINT32(cmd_read_ptr, HardDoomState),
		VMSTATE_UINT32(cmd_write_ptr, HardDoomState),
		VMSTATE_UINT32(fifo_state, HardDoomState),
		VMSTATE_UINT32_ARRAY(state_pt, HardDoomState, 3),
		VMSTATE_UINT32_ARRAY(state_colormap_addr, HardDoomState, 2),
		VMSTATE_UINT32(state_flat_addr, HardDoomState),
		VMSTATE_UINT32(state_surf_dims, HardDoomState),
		VMSTATE_UINT32(state_texture_dims, HardDoomState),
		VMSTATE_UINT32(state_fill_color, HardDoomState),
		VMSTATE_UINT32_ARRAY(state_xy, HardDoomState, 2),
		VMSTATE_UINT32(state_draw_params, HardDoomState),
		VMSTATE_UINT32_ARRAY(state_uvstart, HardDoomState, 2),
		VMSTATE_UINT32_ARRAY(state_uvstep, HardDoomState, 2),
		VMSTATE_UINT32_ARRAY(tlb_tag, HardDoomState, 3),
		VMSTATE_UINT32_ARRAY(tlb_pte, HardDoomState, 3),
		VMSTATE_UINT32(cache_state, HardDoomState),
		VMSTATE_UINT32(draw_x_cur, HardDoomState),
		VMSTATE_UINT32(draw_y_cur, HardDoomState),
		VMSTATE_UINT32(draw_x_restart, HardDoomState),
		VMSTATE_UINT32(draw_end, HardDoomState),
		VMSTATE_UINT32(draw_texcoord_u, HardDoomState),
		VMSTATE_UINT32(draw_texcoord_v, HardDoomState),
		VMSTATE_UINT32(draw_state, HardDoomState),
		VMSTATE_UINT32(draw_line_size, HardDoomState),
		VMSTATE_UINT32(draw_column_offset, HardDoomState),
		VMSTATE_UINT8_ARRAY(cache_data_texture, HardDoomState, HARDDOOM_CACHE_SIZE),
		VMSTATE_UINT8_ARRAY(cache_data_flat, HardDoomState, HARDDOOM_CACHE_SIZE),
		VMSTATE_UINT8_ARRAY(cache_data_colormap, HardDoomState, HARDDOOM_CACHE_SIZE),
		VMSTATE_UINT8_ARRAY(cache_data_translation, HardDoomState, HARDDOOM_CACHE_SIZE),
		VMSTATE_UINT32_ARRAY(fifo_cmd, HardDoomState, HARDDOOM_FIFO_CMD_NUM),
		VMSTATE_END_OF_LIST()
	}
};

static uint32_t le32_read(uint8_t *ptr) {
	return ptr[0] | ptr[1] << 8 | ptr[2] << 16 | ptr[3] << 24;
}

/* Recomputes status register and PCI interrupt line.  */
static void harddoom_status_update(HardDoomState *d) {
	d->status = 0;
	/* DRAW busy iff DRAW_STATE.MODE is non-0.  */
	if ((d->draw_state & HARDDOOM_DRAW_STATE_MODE_MASK) !=
		HARDDOOM_DRAW_STATE_MODE_IDLE)
		d->status |= HARDDOOM_STATUS_DRAW;
	/* FIFO busy iff read != write.  */
	int rptr = HARDDOOM_FIFO_STATE_READ(d->fifo_state);
	int wptr = HARDDOOM_FIFO_STATE_WRITE(d->fifo_state);
	if (rptr != wptr)
		d->status |= HARDDOOM_STATUS_FIFO;
	/* FETCH_CMD busy iff read != write.  */
	if (d->cmd_read_ptr != d->cmd_write_ptr)
		d->status |= HARDDOOM_STATUS_FETCH_CMD;
	/* determine and set PCI interrupt status */
	pci_set_irq(PCI_DEVICE(d), !!(d->intr & d->intr_enable));
}

/* Returns number of free slots in the FIFO.  */
static int harddoom_fifo_free(HardDoomState *d) {
	int rptr = HARDDOOM_FIFO_STATE_READ(d->fifo_state);
	int wptr = HARDDOOM_FIFO_STATE_WRITE(d->fifo_state);
	int used = (wptr - rptr) & HARDDOOM_FIFO_PTR_MASK;
	/* This considers overfull FIFO to have free slots.
	 * It's part of the fun.  */
	return (HARDDOOM_FIFO_CMD_NUM - used) & HARDDOOM_FIFO_PTR_MASK;
}

/* Schedules our worker timer.  Should be called whenever device has new
   work to do (eg. after FIFO_SEND).  status_update has to be called first,
   if necessary.  */
static void harddoom_schedule(HardDoomState *d) {
	/* Draw can draw.  */
	bool draw_busy = (d->status & HARDDOOM_STATUS_DRAW) &&
		(d->enable & HARDDOOM_ENABLE_DRAW);
	/* FIFO can feed draw.  */
	bool fifo_busy = (d->status & HARDDOOM_STATUS_FIFO) &&
		(d->enable & HARDDOOM_ENABLE_FIFO) &&
		!(d->status & HARDDOOM_STATUS_DRAW);
	/* FETCH_CMD can feed FIFO.  */
	bool fetch_cmd_busy = (d->status & HARDDOOM_STATUS_FETCH_CMD) &&
		(d->enable & HARDDOOM_ENABLE_FETCH_CMD) && harddoom_fifo_free(d);
	/* no active blocks - return */
	if (!draw_busy && !fifo_busy && !fetch_cmd_busy)
		return;
	timer_mod(d->timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + (lrand48() % 1000) * 10);
}

/* Resets DRAW unit, making it idle and ready to accept a new command.  */
static void harddoom_reset_draw(HardDoomState *d) {
	/* The bare minimum.  */
	d->draw_state &= ~HARDDOOM_DRAW_STATE_MODE_MASK;
	harddoom_status_update(d);
	/* This could wake up FIFO...  but if it does, someone's
	 * doing something stupid.  */
	harddoom_schedule(d);
}

/* Resets FIFO unit, making it empty.  */
static void harddoom_reset_fifo(HardDoomState *d) {
	/* Set both read and write pointers to 0.  */
	d->fifo_state = 0;
	harddoom_status_update(d);
	/* This could wake up FETCH_CMD...  but if it does, someone's
	 * doing something stupid.  */
	harddoom_schedule(d);
}

/* Resets TLBs, forcing a reread of PT.  */
static void harddoom_reset_tlb(HardDoomState *d) {
	/* Kill TAG valid bits.  */
	for (int i = 0; i < 3; i++)
		d->tlb_tag[i] &= ~HARDDOOM_TLB_TAG_VALID;
}

/* Resets the cache.  */
static void harddoom_reset_cache(HardDoomState *d) {
	/* Kill valid bits, keep the others.  */
	d->cache_state &= HARDDOOM_CACHE_STATE_TEXTURE_TAG_MASK | HARDDOOM_CACHE_STATE_FLAT_TAG_MASK;
}

/* Checks if command is valid (and not a JUMP).  */
static bool harddoom_valid_cmd(uint32_t val) {
	/* JUMPs are handled elsewhere.  If we see one here, it's bad.  */
	if (HARDDOOM_CMD_EXTR_TYPE_HI(val) == HARDDOOM_CMD_TYPE_HI_JUMP)
		return false;
	switch (HARDDOOM_CMD_EXTR_TYPE(val)) {
		case HARDDOOM_CMD_TYPE_SURF_DST_PT:
		case HARDDOOM_CMD_TYPE_SURF_SRC_PT:
		case HARDDOOM_CMD_TYPE_TEXTURE_PT:
			return !(val & ~0xffffffff);
		case HARDDOOM_CMD_TYPE_COLORMAP_ADDR:
		case HARDDOOM_CMD_TYPE_TRANSLATION_ADDR:
			return !(val & ~0xfcffffff);
		case HARDDOOM_CMD_TYPE_FLAT_ADDR:
			return !(val & ~0xfc0fffff);
		case HARDDOOM_CMD_TYPE_SURF_DIMS:
			return !(val & ~0xfc0fffff);
		case HARDDOOM_CMD_TYPE_TEXTURE_DIMS:
			return !(val & ~0xfffff3ff);
		case HARDDOOM_CMD_TYPE_FILL_COLOR:
			return !(val & ~0xfc0000ff);
		case HARDDOOM_CMD_TYPE_XY_A:
		case HARDDOOM_CMD_TYPE_XY_B:
			return !(val & ~0xfc7ff7ff);
		case HARDDOOM_CMD_TYPE_DRAW_PARAMS:
			return !(val & ~0xfc000007);
		case HARDDOOM_CMD_TYPE_USTART:
		case HARDDOOM_CMD_TYPE_VSTART:
		case HARDDOOM_CMD_TYPE_USTEP:
		case HARDDOOM_CMD_TYPE_VSTEP:
			return !(val & ~0xffffffff);
		case HARDDOOM_CMD_TYPE_COPY_RECT:
			return !(val & ~0xfcffffff);
		case HARDDOOM_CMD_TYPE_FILL_RECT:
			return !(val & ~0xfcffffff);
		case HARDDOOM_CMD_TYPE_DRAW_LINE:
			return !(val & ~0xfc000000);
		case HARDDOOM_CMD_TYPE_DRAW_BACKGROUND:
			return !(val & ~0xfc000000);
		case HARDDOOM_CMD_TYPE_DRAW_COLUMN:
			return !(val & ~0xfc3fffff);
		case HARDDOOM_CMD_TYPE_DRAW_SPAN:
			return !(val & ~0xfc000000);
		case HARDDOOM_CMD_TYPE_INTERLOCK:
			return !(val & ~0xfc000000);
		case HARDDOOM_CMD_TYPE_SYNC:
			return !(val & ~0xffffffff);
		default:
			return false;
	}
}

/* Makes given interrupt(s) pending.  */
static void harddoom_trigger(HardDoomState *d, uint32_t intr) {
	d->intr |= intr;
	harddoom_status_update(d);
}

/* Handles FIFO_SEND - appends a command to FIFO, or triggers INVALID_CMD
 * or FIFO_OVERFLOW.  */
static void harddoom_fifo_send(HardDoomState *d, uint32_t val) {
	int free = harddoom_fifo_free(d);
	if (!free) {
		harddoom_trigger(d, HARDDOOM_INTR_FIFO_OVERFLOW);
	} else if (!harddoom_valid_cmd(val)) {
		harddoom_trigger(d, HARDDOOM_INTR_INVALID_CMD);
	} else {
		int rptr = HARDDOOM_FIFO_STATE_READ(d->fifo_state);
		int wptr = HARDDOOM_FIFO_STATE_WRITE(d->fifo_state);
		d->fifo_cmd[wptr & (HARDDOOM_FIFO_CMD_NUM - 1)] = val;
		wptr++;
		wptr &= HARDDOOM_FIFO_PTR_MASK;
		d->fifo_state = rptr | wptr << 16;
		/* DRAW has work to do now.  */
		harddoom_status_update(d);
		harddoom_schedule(d);
	}
}

/* Executes a COPY_RECT command -- prepares DRAW state.  The actual drawing
 * will happen later, asynchronously.  */
static void harddoom_trigger_copy_rect(HardDoomState *d, uint32_t val) {
	int w = HARDDOOM_CMD_EXTR_RECT_WIDTH(val);
	int h = HARDDOOM_CMD_EXTR_RECT_HEIGHT(val);
	int x_a = HARDDOOM_CMD_EXTR_XY_X(d->state_xy[0]);
	int y_a = HARDDOOM_CMD_EXTR_XY_Y(d->state_xy[0]);
	int x_b = HARDDOOM_CMD_EXTR_XY_X(d->state_xy[1]);
	int y_b = HARDDOOM_CMD_EXTR_XY_Y(d->state_xy[1]);
	int x_e = (x_a + w - 1) & HARDDOOM_COORD_MASK;
	int y_e = (y_a + h - 1) & HARDDOOM_COORD_MASK;
	if (!w || !h)
		return;
	d->draw_state &= ~HARDDOOM_DRAW_STATE_MODE_MASK;
	d->draw_state |= HARDDOOM_DRAW_STATE_MODE_COPY;
	d->draw_x_cur = d->draw_x_restart = x_a | x_b << 16;
	d->draw_y_cur = y_a | y_b << 16;
	d->draw_end = x_e | y_e << 16;
	harddoom_status_update(d);
	harddoom_schedule(d);
}

/* Executes a FILL_RECT command.  */
static void harddoom_trigger_fill_rect(HardDoomState *d, uint32_t val) {
	int w = HARDDOOM_CMD_EXTR_RECT_WIDTH(val);
	int h = HARDDOOM_CMD_EXTR_RECT_HEIGHT(val);
	int x_a = HARDDOOM_CMD_EXTR_XY_X(d->state_xy[0]);
	int y_a = HARDDOOM_CMD_EXTR_XY_Y(d->state_xy[0]);
	int x_e = (x_a + w - 1) & HARDDOOM_COORD_MASK;
	int y_e = (y_a + h - 1) & HARDDOOM_COORD_MASK;
	if (!w || !h)
		return;
	d->draw_state &= ~HARDDOOM_DRAW_STATE_MODE_MASK;
	d->draw_state |= HARDDOOM_DRAW_STATE_MODE_FILL;
	d->draw_x_cur &= ~HARDDOOM_COORD_MASK;
	d->draw_x_cur |= x_a;
	d->draw_x_restart &= ~HARDDOOM_COORD_MASK;
	d->draw_x_restart |= x_a;
	d->draw_y_cur &= ~HARDDOOM_COORD_MASK;
	d->draw_y_cur |= y_a;
	d->draw_end = x_e | y_e << 16;
	harddoom_status_update(d);
	harddoom_schedule(d);
}

/* Executes a DRAW_LINE command.  */
static void harddoom_trigger_draw_line(HardDoomState *d, uint32_t val) {
	int x_a = HARDDOOM_CMD_EXTR_XY_X(d->state_xy[0]);
	int y_a = HARDDOOM_CMD_EXTR_XY_Y(d->state_xy[0]);
	int x_e = HARDDOOM_CMD_EXTR_XY_X(d->state_xy[1]);
	int y_e = HARDDOOM_CMD_EXTR_XY_Y(d->state_xy[1]);
	d->draw_state &= ~HARDDOOM_DRAW_STATE_MODE_MASK;
	d->draw_state |= HARDDOOM_DRAW_STATE_MODE_LINE;
	d->draw_x_cur &= ~HARDDOOM_COORD_MASK;
	d->draw_x_cur |= x_a;
	d->draw_y_cur &= ~HARDDOOM_COORD_MASK;
	d->draw_y_cur |= y_a;
	d->draw_end = x_e | y_e << 16;
	int ax = x_e - x_a;
	if (ax < 0) {
		ax = -ax;
		d->draw_state |= HARDDOOM_DRAW_STATE_LINE_SX_NEGATIVE;
	} else {
		d->draw_state &= ~HARDDOOM_DRAW_STATE_LINE_SX_NEGATIVE;
	}
	int ay = y_e - y_a;
	if (ay < 0) {
		ay = -ay;
		d->draw_state |= HARDDOOM_DRAW_STATE_LINE_SY_NEGATIVE;
	} else {
		d->draw_state &= ~HARDDOOM_DRAW_STATE_LINE_SY_NEGATIVE;
	}
	d->draw_line_size = ax | ay << 16;
	int line_d;
	if (ax > ay) {
		d->draw_state |= HARDDOOM_DRAW_STATE_LINE_X_MAJOR;
		line_d = 2 * ay - ax;
	} else {
		d->draw_state &= ~HARDDOOM_DRAW_STATE_LINE_X_MAJOR;
		line_d = 2 * ax - ay;
	}
	d->draw_state &= ~HARDDOOM_DRAW_STATE_LINE_D_MASK;
	d->draw_state |= line_d << HARDDOOM_DRAW_STATE_LINE_D_SHIFT &
		HARDDOOM_DRAW_STATE_LINE_D_MASK;
	harddoom_status_update(d);
	harddoom_schedule(d);
}

/* Executes a DRAW_BACKGROUND command.  */
static void harddoom_trigger_draw_background(HardDoomState *d, uint32_t val) {
	int x_e = (HARDDOOM_CMD_EXTR_SURF_DIMS_WIDTH(d->state_surf_dims) - 1) & HARDDOOM_COORD_MASK;
	int y_e = (HARDDOOM_CMD_EXTR_SURF_DIMS_HEIGHT(d->state_surf_dims) - 1) & HARDDOOM_COORD_MASK;
	d->draw_state &= ~HARDDOOM_DRAW_STATE_MODE_MASK;
	d->draw_state |= HARDDOOM_DRAW_STATE_MODE_BACKGROUND;
	/* Start from 0.  */
	d->draw_x_cur &= ~HARDDOOM_COORD_MASK;
	d->draw_x_restart &= ~HARDDOOM_COORD_MASK;
	d->draw_y_cur &= ~HARDDOOM_COORD_MASK;
	d->draw_end = x_e | y_e << 16;
	harddoom_status_update(d);
	harddoom_schedule(d);
}

/* Executes a DRAW_COLUMN command.  */
static void harddoom_trigger_draw_column(HardDoomState *d, uint32_t val) {
	uint32_t column_offset = HARDDOOM_CMD_EXTR_COLUMN_OFFSET(val);
	int x_a = HARDDOOM_CMD_EXTR_XY_X(d->state_xy[0]);
	int y_a = HARDDOOM_CMD_EXTR_XY_Y(d->state_xy[0]);
	int x_e = HARDDOOM_CMD_EXTR_XY_X(d->state_xy[1]);
	int y_e = HARDDOOM_CMD_EXTR_XY_Y(d->state_xy[1]);
	d->draw_state &= ~HARDDOOM_DRAW_STATE_MODE_MASK;
	if (d->state_draw_params & HARDDOOM_DRAW_PARAMS_FUZZ) {
		d->draw_state |= HARDDOOM_DRAW_STATE_MODE_FUZZ;
	} else {
		d->draw_state |= HARDDOOM_DRAW_STATE_MODE_COLUMN;
	}
	d->draw_x_cur &= ~HARDDOOM_COORD_MASK;
	d->draw_x_cur |= x_a;
	d->draw_x_restart &= ~HARDDOOM_COORD_MASK;
	d->draw_x_restart |= x_a;
	d->draw_y_cur &= ~HARDDOOM_COORD_MASK;
	d->draw_y_cur |= y_a;
	d->draw_end = x_e | y_e << 16;
	d->draw_texcoord_u = d->state_uvstart[0];
	d->draw_column_offset = column_offset;
	harddoom_status_update(d);
	harddoom_schedule(d);
}

/* Executes a DRAW_SPAN command.  */
static void harddoom_trigger_draw_span(HardDoomState *d, uint32_t val) {
	int x_a = HARDDOOM_CMD_EXTR_XY_X(d->state_xy[0]);
	int y_a = HARDDOOM_CMD_EXTR_XY_Y(d->state_xy[0]);
	int x_e = HARDDOOM_CMD_EXTR_XY_X(d->state_xy[1]);
	int y_e = HARDDOOM_CMD_EXTR_XY_Y(d->state_xy[1]);
	d->draw_state &= ~HARDDOOM_DRAW_STATE_MODE_MASK;
	if (d->state_draw_params & HARDDOOM_DRAW_PARAMS_FUZZ) {
		d->draw_state |= HARDDOOM_DRAW_STATE_MODE_FUZZ;
	} else {
		d->draw_state |= HARDDOOM_DRAW_STATE_MODE_SPAN;
	}
	d->draw_x_cur &= ~HARDDOOM_COORD_MASK;
	d->draw_x_cur |= x_a;
	d->draw_x_restart &= ~HARDDOOM_COORD_MASK;
	d->draw_x_restart |= x_a;
	d->draw_y_cur &= ~HARDDOOM_COORD_MASK;
	d->draw_y_cur |= y_a;
	d->draw_end = x_e | y_e << 16;
	d->draw_texcoord_u = d->state_uvstart[0];
	d->draw_texcoord_v = d->state_uvstart[1];
	harddoom_status_update(d);
	harddoom_schedule(d);
}

/* Converts virtual offset to a physical address -- handles PT lookup.
 * If something goes wrong, disables DRAW_ENABLE, fires an interrupt,
 * and returns junk.  */

static uint32_t harddoom_translate_addr(HardDoomState *d, int which, uint32_t offset) {
	uint32_t vidx = offset >> HARDDOOM_PAGE_SHIFT & HARDDOOM_TLB_TAG_IDX_MASK;
	uint32_t tag = vidx | HARDDOOM_TLB_TAG_VALID;
	uint32_t pte_addr = (d->state_pt[which] << 6) + (vidx << 2);
	uint32_t cur_tag = d->tlb_tag[which];
	uint32_t pte = d->tlb_pte[which];
	if (cur_tag != tag) {
		/* Mismatched or invalid tag -- fetch a new one.  */
		uint8_t pteb[4];
		pci_dma_read(&d->dev, pte_addr, &pteb, sizeof pteb);
		pte = le32_read(pteb) & 0xfffff001;
		d->tlb_tag[which] = tag;
		d->tlb_pte[which] = pte;
	}
	if (!(pte & HARDDOOM_PTE_VALID)) {
		d->enable &= ~HARDDOOM_ENABLE_DRAW;
		harddoom_trigger(d, HARDDOOM_INTR_PAGE_FAULT_SURF_DST << which);
	}
	return (pte & HARDDOOM_PTE_PHYS_MASK) | (offset & (HARDDOOM_PAGE_SIZE - 1));
}

/* Converts x, y position to virtual address.  If something goes wrong,
 * triggers SURF_OVERFLOW.  */
static uint32_t harddoom_translate_surf_xy(HardDoomState *d, uint16_t x, uint16_t y) {
	int w = HARDDOOM_CMD_EXTR_SURF_DIMS_WIDTH(d->state_surf_dims);
	int h = HARDDOOM_CMD_EXTR_SURF_DIMS_HEIGHT(d->state_surf_dims);
	if (x >= w || y >= h) {
		/* Fire an interrupt and disable ourselves.  */
		d->enable &= ~HARDDOOM_ENABLE_DRAW;
		harddoom_trigger(d, HARDDOOM_INTR_SURF_OVERFLOW);
	}
	return y * w + x;
}

/* Ensures the texture cache is filled with the line containing the given offset.
 * May cause a page fault and disable DRAW.  */
static void harddoom_fill_cache_texture(HardDoomState *d, uint32_t offset) {
	offset &= ~(HARDDOOM_CACHE_SIZE - 1);
	int tag = offset >> HARDDOOM_CACHE_SHIFT;
	if ((d->cache_state & HARDDOOM_CACHE_STATE_TEXTURE_VALID) &&
		((d->cache_state & HARDDOOM_CACHE_STATE_TEXTURE_TAG_MASK) >>
		 HARDDOOM_CACHE_STATE_TEXTURE_TAG_SHIFT) == tag)
		return;
	if (offset < HARDDOOM_CMD_EXTR_TEXTURE_SIZE(d->state_texture_dims)) {
		/* If it's out of range, just skip the fetch and use whatever
		 * crap we already have in the cache.  */
		uint32_t phys = harddoom_translate_addr(d, TLB_TEXTURE, offset);
		if (!(d->enable & HARDDOOM_ENABLE_DRAW))
			return;
		pci_dma_read(&d->dev, phys, d->cache_data_texture, HARDDOOM_CACHE_SIZE);
	}
	d->cache_state &= ~HARDDOOM_CACHE_STATE_TEXTURE_TAG_MASK;
	d->cache_state |= tag << HARDDOOM_CACHE_STATE_TEXTURE_TAG_SHIFT;
	d->cache_state |= HARDDOOM_CACHE_STATE_TEXTURE_VALID;
}

/* Ensures the flat cache is filled with the line containing the given offset.  */
static void harddoom_fill_cache_flat(HardDoomState *d, uint32_t offset) {
	offset &= ~(HARDDOOM_CACHE_SIZE - 1);
	int tag = offset >> HARDDOOM_CACHE_SHIFT;
	if ((d->cache_state & HARDDOOM_CACHE_STATE_FLAT_VALID) &&
		((d->cache_state & HARDDOOM_CACHE_STATE_FLAT_TAG_MASK) >>
		 HARDDOOM_CACHE_STATE_FLAT_TAG_SHIFT) == tag)
		return;
	uint32_t phys = d->state_flat_addr << 12 | offset;
	pci_dma_read(&d->dev, phys, d->cache_data_flat, HARDDOOM_CACHE_SIZE);
	d->cache_state &= ~HARDDOOM_CACHE_STATE_FLAT_TAG_MASK;
	d->cache_state |= tag << HARDDOOM_CACHE_STATE_FLAT_TAG_SHIFT;
	d->cache_state |= HARDDOOM_CACHE_STATE_FLAT_VALID;
}

/* Ensures the colormap cache is filled.  */
static void harddoom_fill_cache_colormap(HardDoomState *d) {
	if (d->cache_state & HARDDOOM_CACHE_STATE_COLORMAP_VALID)
		return;
	uint32_t phys = d->state_colormap_addr[0] << 8;
	pci_dma_read(&d->dev, phys, d->cache_data_colormap, HARDDOOM_CACHE_SIZE);
	d->cache_state |= HARDDOOM_CACHE_STATE_COLORMAP_VALID;
}

/* Ensures the translation cache is filled.  */
static void harddoom_fill_cache_translation(HardDoomState *d) {
	if (d->cache_state & HARDDOOM_CACHE_STATE_TRANSLATION_VALID)
		return;
	uint32_t phys = d->state_colormap_addr[1] << 8;
	pci_dma_read(&d->dev, phys, d->cache_data_translation, HARDDOOM_CACHE_SIZE);
	d->cache_state |= HARDDOOM_CACHE_STATE_TRANSLATION_VALID;
}

/* Executes a command.  */
static void harddoom_do_command(HardDoomState *d, int cmd, uint32_t val) {
	val &= ~0xfc000000;
	switch (cmd) {
	case HARDDOOM_CMD_TYPE_SURF_DST_PT:
		d->state_pt[TLB_SURF_DST] = val;
		d->tlb_tag[TLB_SURF_DST] &= ~HARDDOOM_TLB_TAG_VALID;
		break;
	case HARDDOOM_CMD_TYPE_SURF_SRC_PT:
		d->state_pt[TLB_SURF_SRC] = val;
		d->tlb_tag[TLB_SURF_SRC] &= ~HARDDOOM_TLB_TAG_VALID;
		break;
	case HARDDOOM_CMD_TYPE_TEXTURE_PT:
		d->state_pt[TLB_TEXTURE] = val;
		d->tlb_tag[TLB_TEXTURE] &= ~HARDDOOM_TLB_TAG_VALID;
		d->cache_state &= ~HARDDOOM_CACHE_STATE_TEXTURE_VALID;
		break;
	case HARDDOOM_CMD_TYPE_COLORMAP_ADDR:
		d->state_colormap_addr[0] = val & 0xffffff;
		d->cache_state &= ~HARDDOOM_CACHE_STATE_COLORMAP_VALID;
		break;
	case HARDDOOM_CMD_TYPE_TRANSLATION_ADDR:
		d->state_colormap_addr[1] = val & 0xffffff;
		d->cache_state &= ~HARDDOOM_CACHE_STATE_TRANSLATION_VALID;
		break;
	case HARDDOOM_CMD_TYPE_FLAT_ADDR:
		d->state_flat_addr = val & 0xfffff;
		d->cache_state &= ~HARDDOOM_CACHE_STATE_FLAT_VALID;
		break;
	case HARDDOOM_CMD_TYPE_SURF_DIMS:
		d->state_surf_dims = val & 0xffffff;
		break;
	case HARDDOOM_CMD_TYPE_TEXTURE_DIMS:
		d->state_texture_dims = val & 0x3fff3ff;
		break;
	case HARDDOOM_CMD_TYPE_FILL_COLOR:
		d->state_fill_color = val & 0xff;
		break;
	case HARDDOOM_CMD_TYPE_XY_A:
		d->state_xy[0] = val & 0x7ff7ff;
		break;
	case HARDDOOM_CMD_TYPE_XY_B:
		d->state_xy[1] = val & 0x7ff7ff;
		break;
	case HARDDOOM_CMD_TYPE_DRAW_PARAMS:
		d->state_draw_params = val & 7;
		break;
	case HARDDOOM_CMD_TYPE_USTART:
		d->state_uvstart[0] = val;
		break;
	case HARDDOOM_CMD_TYPE_VSTART:
		d->state_uvstart[1] = val;
		break;
	case HARDDOOM_CMD_TYPE_USTEP:
		d->state_uvstep[0] = val;
		break;
	case HARDDOOM_CMD_TYPE_VSTEP:
		d->state_uvstep[1] = val;
		break;
	case HARDDOOM_CMD_TYPE_COPY_RECT:
		harddoom_trigger_copy_rect(d, val);
		break;
	case HARDDOOM_CMD_TYPE_FILL_RECT:
		harddoom_trigger_fill_rect(d, val);
		break;
	case HARDDOOM_CMD_TYPE_DRAW_LINE:
		harddoom_trigger_draw_line(d, val);
		break;
	case HARDDOOM_CMD_TYPE_DRAW_BACKGROUND:
		harddoom_trigger_draw_background(d, val);
		break;
	case HARDDOOM_CMD_TYPE_DRAW_COLUMN:
		harddoom_trigger_draw_column(d, val);
		break;
	case HARDDOOM_CMD_TYPE_DRAW_SPAN:
		harddoom_trigger_draw_span(d, val);
		break;
	case HARDDOOM_CMD_TYPE_INTERLOCK:
		break;
	case HARDDOOM_CMD_TYPE_SYNC:
		d->sync_last = val;
		if (d->sync_intr == val)
			harddoom_trigger(d, HARDDOOM_INTR_SYNC);
		break;
	default:
		fprintf(stderr, "harddoom error: invalid command %d executed [param %08x]\n", cmd, val);
	}
}

/* Draws a few pixels of a line.  */
static void harddoom_draw_work_line(HardDoomState *d) {
	int x = d->draw_x_cur & HARDDOOM_COORD_MASK;
	int y = d->draw_y_cur & HARDDOOM_COORD_MASK;
	int x_e = d->draw_end & HARDDOOM_COORD_MASK;
	int y_e = d->draw_end >> 16 & HARDDOOM_COORD_MASK;
	int line_d = d->draw_state & HARDDOOM_DRAW_STATE_LINE_D_MASK >> HARDDOOM_DRAW_STATE_LINE_D_SHIFT;
	if (line_d & 0x1000)
		line_d -= 0x2000;
	int ax = d->draw_line_size & HARDDOOM_COORD_MASK;
	int ay = d->draw_line_size >> 16 & HARDDOOM_COORD_MASK;
	int sx = 1;
	if (d->draw_state & HARDDOOM_DRAW_STATE_LINE_SX_NEGATIVE)
		sx = -1;
	int sy = 1;
	if (d->draw_state & HARDDOOM_DRAW_STATE_LINE_SY_NEGATIVE)
		sy = -1;
	/* Tweakable parameter here.  */
	int steps = 64;
	while (steps--) {
		/* Find and draw the actual pixel.  */
		uint32_t virt = harddoom_translate_surf_xy(d, x, y);
		if (!(d->enable & HARDDOOM_ENABLE_DRAW))
			return;
		uint32_t phys = harddoom_translate_addr(d, TLB_SURF_DST, virt);
		if (!(d->enable & HARDDOOM_ENABLE_DRAW))
			return;
		uint8_t color = d->state_fill_color;
		pci_dma_write(&d->dev, phys, &color, 1);
		/* Now, the Bresenham algorithm.  */
		if (d->draw_state & HARDDOOM_DRAW_STATE_LINE_X_MAJOR) {
			if (line_d >= 0) {
				y += sy;
				line_d -= ax * 2;
			}
			x += sx;
			line_d += ay * 2;
		} else {
			if (line_d >= 0) {
				x += sx;
				line_d -= ay * 2;
			}
			y += sy;
			line_d += ax * 2;
		}
		/* Write the new coordinates back.  */
		d->draw_x_cur &= ~HARDDOOM_COORD_MASK;
		d->draw_x_cur |= x;
		d->draw_y_cur &= ~HARDDOOM_COORD_MASK;
		d->draw_y_cur |= y;
		d->draw_state &= ~HARDDOOM_DRAW_STATE_LINE_D_MASK;
		d->draw_state |= line_d << HARDDOOM_DRAW_STATE_LINE_D_SHIFT & HARDDOOM_DRAW_STATE_LINE_D_MASK;
		if (x == x_e && y == y_e) {
			/* We're done here.  */
			d->draw_state &= ~HARDDOOM_DRAW_STATE_MODE_MASK;
			harddoom_status_update(d);
			return;
		}
	}
}

/* Does a single "atom" of work -- up to 64 pixels in a single 64-byte aligned
 * block.  */
static void harddoom_draw_work_atom(HardDoomState *d) {
	int mode = d->draw_state & HARDDOOM_DRAW_STATE_MODE_MASK;
	assert(mode != HARDDOOM_DRAW_STATE_MODE_IDLE);
	if (mode == HARDDOOM_DRAW_STATE_MODE_LINE) {
		/* Lines are special.  */
		harddoom_draw_work_line(d);
		return;
	}
	int x = d->draw_x_cur & HARDDOOM_COORD_MASK;
	int y = d->draw_y_cur & HARDDOOM_COORD_MASK;
	int x_e = d->draw_end & HARDDOOM_COORD_MASK;
	int y_e = d->draw_end >> 16 & HARDDOOM_COORD_MASK;
	int x_s = d->draw_x_cur >> 16 & HARDDOOM_COORD_MASK;
	int y_s = d->draw_y_cur >> 16 & HARDDOOM_COORD_MASK;
	uint32_t virt = harddoom_translate_surf_xy(d, x, y);
	if (!(d->enable & HARDDOOM_ENABLE_DRAW))
		return;
	uint32_t phys = harddoom_translate_addr(d, TLB_SURF_DST, virt);
	if (!(d->enable & HARDDOOM_ENABLE_DRAW))
		return;
	uint8_t buf[64] = { 0 };
	int count = x_e - x + 1;
	if (count <= 0)
		count = 64;
	int align_count = (x | 0x3f) + 1 - x;
	if (align_count < count)
		count = align_count;
	assert(count > 0 && count <= 64);
	if (mode == HARDDOOM_DRAW_STATE_MODE_COPY) {
		align_count = (x_s | 0x3f) + 1 - x_s;
		if (align_count < count)
			count = align_count;
		uint32_t src_virt = harddoom_translate_surf_xy(d, x_s, y_s);
		if (!(d->enable & HARDDOOM_ENABLE_DRAW))
			return;
		uint32_t src_phys = harddoom_translate_addr(d, TLB_SURF_SRC, src_virt);
		if (!(d->enable & HARDDOOM_ENABLE_DRAW))
			return;
		pci_dma_read(&d->dev, src_phys, buf, count);
	} else if (mode == HARDDOOM_DRAW_STATE_MODE_FILL) {
		memset(buf, d->state_fill_color, sizeof buf);
	} else if (mode == HARDDOOM_DRAW_STATE_MODE_BACKGROUND) {
		harddoom_fill_cache_flat(d, (y & 0x3f) << 6);
		int start = (x & 0x3f) | (y & 3) << 6;
		memcpy(buf, d->cache_data_flat + start, count);
	} else if (mode == HARDDOOM_DRAW_STATE_MODE_COLUMN) {
		if (d->state_draw_params & HARDDOOM_DRAW_PARAMS_TRANSLATE)
			harddoom_fill_cache_translation(d);
		if (d->state_draw_params & HARDDOOM_DRAW_PARAMS_COLORMAP)
			harddoom_fill_cache_colormap(d);
		int ty = d->draw_texcoord_u >> 16;
		int th = HARDDOOM_CMD_EXTR_TEXTURE_HEIGHT(d->state_texture_dims);
		if (th)
			ty %= th;
		int off = ty + d->draw_column_offset;
		off &= 0x3fffff;
		harddoom_fill_cache_texture(d, off);
		if (!(d->enable & HARDDOOM_ENABLE_DRAW))
			return;
		uint8_t color = d->cache_data_texture[off & 0xff];
		if (d->state_draw_params & HARDDOOM_DRAW_PARAMS_TRANSLATE)
			color = d->cache_data_translation[color];
		if (d->state_draw_params & HARDDOOM_DRAW_PARAMS_COLORMAP)
			color = d->cache_data_colormap[color];
		memset(buf, color, count);
		d->draw_texcoord_u += d->state_uvstep[0];
		d->draw_texcoord_u &= 0x3ffffff;
	} else if (mode == HARDDOOM_DRAW_STATE_MODE_SPAN) {
		if (d->state_draw_params & HARDDOOM_DRAW_PARAMS_TRANSLATE)
			harddoom_fill_cache_translation(d);
		if (d->state_draw_params & HARDDOOM_DRAW_PARAMS_COLORMAP)
			harddoom_fill_cache_colormap(d);
		for (int i = 0; i < count; i++) {
			int tx = d->draw_texcoord_u >> 16 & 0x3f;
			int ty = d->draw_texcoord_v >> 16 & 0x3f;
			int off = ty << 6 | tx;
			harddoom_fill_cache_flat(d, off);
			uint8_t color = d->cache_data_flat[off & 0xff];
			if (d->state_draw_params & HARDDOOM_DRAW_PARAMS_TRANSLATE)
				color = d->cache_data_translation[color];
			if (d->state_draw_params & HARDDOOM_DRAW_PARAMS_COLORMAP)
				color = d->cache_data_colormap[color];
			buf[i] = color;
			d->draw_texcoord_u += d->state_uvstep[0];
			d->draw_texcoord_v += d->state_uvstep[1];
		}
		d->draw_texcoord_u &= 0x3fffff;
		d->draw_texcoord_v &= 0x3fffff;
	} else if (mode == HARDDOOM_DRAW_STATE_MODE_FUZZ) {
		harddoom_fill_cache_colormap(d);
		uint8_t bufp[64], bufn[64];
		int py = y - 1;
		if (py < 0)
			py = 0;
		int ny = y + 1;
		if (ny == HARDDOOM_CMD_EXTR_SURF_DIMS_HEIGHT(d->state_surf_dims))
			ny = y;
		uint32_t p_virt = harddoom_translate_surf_xy(d, x, py);
		if (!(d->enable & HARDDOOM_ENABLE_DRAW))
			return;
		uint32_t p_phys = harddoom_translate_addr(d, TLB_SURF_DST, p_virt);
		if (!(d->enable & HARDDOOM_ENABLE_DRAW))
			return;
		uint32_t n_virt = harddoom_translate_surf_xy(d, x, ny);
		if (!(d->enable & HARDDOOM_ENABLE_DRAW))
			return;
		uint32_t n_phys = harddoom_translate_addr(d, TLB_SURF_DST, n_virt);
		if (!(d->enable & HARDDOOM_ENABLE_DRAW))
			return;
		pci_dma_read(&d->dev, p_phys, bufp, count);
		pci_dma_read(&d->dev, n_phys, bufn, count);
		int fuzzpos = (d->draw_state & HARDDOOM_DRAW_STATE_FUZZPOS_MASK) >> HARDDOOM_DRAW_STATE_FUZZPOS_SHIFT;
		for (int i = 0; i < count; i++) {
			uint8_t color;
			if (0x121e650de224aull >> fuzzpos & 1)
				color = bufp[i];
			else
				color = bufn[i];
			buf[i] = d->cache_data_colormap[color];
			fuzzpos++;
			fuzzpos &= 0x3f;
			if (fuzzpos >= 50)
				fuzzpos = 0;
		}
		d->draw_state &= ~HARDDOOM_DRAW_STATE_FUZZPOS_MASK;
		d->draw_state |= fuzzpos << HARDDOOM_DRAW_STATE_FUZZPOS_SHIFT;
	}
	pci_dma_write(&d->dev, phys, buf, count);
	x += count;
	x_s += count;
	int x_done = x - 1 == x_e;
	x &= HARDDOOM_COORD_MASK;
	x_s &= HARDDOOM_COORD_MASK;
	d->draw_x_cur = x_s << 16 | x;
	if (x_done) {
		d->draw_x_cur = d->draw_x_restart;
		int y_done = y == y_e;
		y++;
		y_s++;
		y &= HARDDOOM_COORD_MASK;
		y_s &= HARDDOOM_COORD_MASK;
		d->draw_y_cur = y_s << 16 | y;
		if (y_done) {
			d->draw_state &= ~HARDDOOM_DRAW_STATE_MODE_MASK;
		}
	}
}

/* Main loop of the DRAW unit.  Does a random amount of work.  */
static void harddoom_tick_draw(HardDoomState *d) {
	/* Make the device as evil as easily possible by randomizing everything. */
	/* First, determine how many units of work we do this time. */
	int work_cnt = lrand48() & 0x3ffff;
	while (work_cnt--) {
		/* We may self-disable while working if we hit an error.  */
		if (!(d->enable & HARDDOOM_ENABLE_DRAW))
			return;
		if (d->status & HARDDOOM_STATUS_DRAW) {
			/* First, draw if there's drawing to do.  */
			harddoom_draw_work_atom(d);
		} else if (d->status & HARDDOOM_STATUS_FIFO) {
			if (!(d->enable & HARDDOOM_ENABLE_FIFO))
				return;
			/* No drawing in progress, execute a command if there is one.  */
			int rptr = HARDDOOM_FIFO_STATE_READ(d->fifo_state);
			int wptr = HARDDOOM_FIFO_STATE_WRITE(d->fifo_state);
			uint32_t cmd = d->fifo_cmd[rptr & 0x1ff];
			rptr++;
			rptr &= 0x3ff;
			d->fifo_state = rptr | wptr << 16;
			harddoom_do_command(d, HARDDOOM_CMD_EXTR_TYPE(cmd), cmd);
		} else {
			/* Nothing to do.  */
			return;
		}
		harddoom_status_update(d);
	}
}

/* Main loop of the FETCH_CMD unit.  Does a random amount of work.  */
static void harddoom_tick_fetch_cmd(HardDoomState *d) {
	if (!(d->enable & HARDDOOM_ENABLE_FETCH_CMD))
		return;
	/* First, determine how many commands we process this time. */
	int cmd_cnt = lrand48() % 100;
	while (cmd_cnt--) {
		/* First, check if we're still allowed to work. */
		if (!(d->status & HARDDOOM_STATUS_FETCH_CMD))
			return;
		/* Now, check if there's some place to put commands.  */
		if (!harddoom_fifo_free(d))
			return;
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
		harddoom_status_update(d);
	}
}

/* Main loop of the device.  Does a random amount of work.  */
static void harddoom_tick(void *opaque) {
	HardDoomState *d = opaque;
	harddoom_tick_draw(d);
	harddoom_tick_fetch_cmd(d);
	/* Schedule the next appointment. */
	harddoom_schedule(d);
}

/* MMIO write handlers.  */
static void harddoom_mmio_writeb(void *opaque, hwaddr addr, uint32_t val)
{
	int i;
	HardDoomState *d = opaque;
	if ((addr & ~(HARDDOOM_CACHE_SIZE - 1)) == HARDDOOM_CACHE_DATA_TEXTURE(0)) {
		i = addr & (HARDDOOM_CACHE_SIZE - 1);
		d->cache_data_texture[i] = val;
		return;
	}
	if ((addr & ~(HARDDOOM_CACHE_SIZE - 1)) == HARDDOOM_CACHE_DATA_FLAT(0)) {
		i = addr & (HARDDOOM_CACHE_SIZE - 1);
		d->cache_data_flat[i] = val;
		return;
	}
	if ((addr & ~(HARDDOOM_CACHE_SIZE - 1)) == HARDDOOM_CACHE_DATA_COLORMAP(0)) {
		i = addr & (HARDDOOM_CACHE_SIZE - 1);
		d->cache_data_colormap[i] = val;
		return;
	}
	if ((addr & ~(HARDDOOM_CACHE_SIZE - 1)) == HARDDOOM_CACHE_DATA_TRANSLATION(0)) {
		i = addr & (HARDDOOM_CACHE_SIZE - 1);
		d->cache_data_translation[i] = val;
		return;
	}
	fprintf(stderr, "harddoom error: byte-sized write at %03x, value %02x\n", (int)addr, val);
}

static void harddoom_mmio_writew(void *opaque, hwaddr addr, uint32_t val)
{
	fprintf(stderr, "harddoom error: word-sized write at %03x, value %04x\n", (int)addr, val);
}

static void harddoom_mmio_writel(void *opaque, hwaddr addr, uint32_t val)
{
	int i;
	HardDoomState *d = opaque;
	switch (addr) {
	/* Documented registers.  */
	case HARDDOOM_ENABLE:
		d->enable = val & (HARDDOOM_ENABLE_DRAW | HARDDOOM_ENABLE_FIFO | HARDDOOM_ENABLE_FETCH_CMD);
		if (val & ~(HARDDOOM_ENABLE_DRAW | HARDDOOM_ENABLE_FIFO | HARDDOOM_ENABLE_FETCH_CMD))
			fprintf(stderr, "harddoom error: invalid ENABLE value %08x\n", val);
		harddoom_schedule(d);
		return;
	case HARDDOOM_RESET:
		if (val & HARDDOOM_RESET_DRAW)
			harddoom_reset_draw(d);
		if (val & HARDDOOM_RESET_FIFO)
			harddoom_reset_fifo(d);
		if (val & HARDDOOM_RESET_TLB)
			harddoom_reset_tlb(d);
		if (val & HARDDOOM_RESET_CACHE)
			harddoom_reset_cache(d);
		if (val & ~(HARDDOOM_RESET_DRAW | HARDDOOM_RESET_FIFO | HARDDOOM_RESET_TLB | HARDDOOM_RESET_CACHE))
			fprintf(stderr, "harddoom error: invalid RESET value %08x\n", val);
		return;
	case HARDDOOM_INTR:
		d->intr &= ~val;
		if (val & ~HARDDOOM_INTR_MASK)
			fprintf(stderr, "harddoom error: invalid INTR value %08x\n", val);
		harddoom_status_update(d);
		return;
	case HARDDOOM_INTR_ENABLE:
		d->intr_enable = val & HARDDOOM_INTR_MASK;
		if (val & ~HARDDOOM_INTR_MASK)
			fprintf(stderr, "harddoom error: invalid INTR_ENABLE value %08x\n", val);
		harddoom_status_update(d);
		return;
	case HARDDOOM_SYNC_LAST:
		d->sync_last = val;
		if (val & ~HARDDOOM_SYNC_MASK)
			fprintf(stderr, "harddoom error: invalid SYNC_LAST value %08x\n", val);
		return;
	case HARDDOOM_SYNC_INTR:
		d->sync_intr = val;
		if (val & ~HARDDOOM_SYNC_MASK)
			fprintf(stderr, "harddoom error: invalid SYNC_INTR value %08x\n", val);
		return;
	case HARDDOOM_FIFO_SEND:
		harddoom_fifo_send(d, val);
		return;
	case HARDDOOM_CMD_READ_PTR:
		if (val & 3) {
			fprintf(stderr, "harddoom error: CMD_READ_PTR not aligned\n");
			val &= ~3;
		}
		/* If FETCH_CMD is working, this is a bad idea.  */
		if ((d->enable & HARDDOOM_ENABLE_FETCH_CMD))
			fprintf(stderr, "harddoom warning: CMD_READ_PTR written while command fetch enabled\n");
		d->cmd_read_ptr = val;
		harddoom_status_update(d);
		harddoom_schedule(d);
		return;
	case HARDDOOM_CMD_WRITE_PTR:
		if (val & 3) {
			fprintf(stderr, "harddoom error: CMD_WRITE_PTR not aligned\n");
			val &= ~3;
		}
		d->cmd_write_ptr = val;
		harddoom_status_update(d);
		harddoom_schedule(d);
		return;
	/* Undocumented registers -- direct access to TLB state.  */
	case HARDDOOM_TLB_SURF_DST_TAG:
		d->tlb_tag[TLB_SURF_DST] = val & (HARDDOOM_TLB_TAG_IDX_MASK | HARDDOOM_TLB_TAG_VALID);
		return;
	case HARDDOOM_TLB_SURF_DST_PTE:
		d->tlb_pte[TLB_SURF_DST] = val & (HARDDOOM_PTE_PHYS_MASK | HARDDOOM_PTE_VALID);
		return;
	case HARDDOOM_TLB_SURF_SRC_TAG:
		d->tlb_tag[TLB_SURF_SRC] = val & (HARDDOOM_TLB_TAG_IDX_MASK | HARDDOOM_TLB_TAG_VALID);
		return;
	case HARDDOOM_TLB_SURF_SRC_PTE:
		d->tlb_pte[TLB_SURF_SRC] = val & (HARDDOOM_PTE_PHYS_MASK | HARDDOOM_PTE_VALID);
		return;
	case HARDDOOM_TLB_TEXTURE_TAG:
		d->tlb_tag[TLB_TEXTURE] = val & (HARDDOOM_TLB_TAG_IDX_MASK | HARDDOOM_TLB_TAG_VALID);
		return;
	case HARDDOOM_TLB_TEXTURE_PTE:
		d->tlb_pte[TLB_TEXTURE] = val & (HARDDOOM_PTE_PHYS_MASK | HARDDOOM_PTE_VALID);
		return;
	/* Direct access to DRAW state.  */
	case HARDDOOM_DRAW_X_CUR:
		d->draw_x_cur = val & 0x07ff07ff;
		return;
	case HARDDOOM_DRAW_Y_CUR:
		d->draw_y_cur = val & 0x07ff07ff;
		return;
	case HARDDOOM_DRAW_X_RESTART:
		d->draw_x_restart = val & 0x07ff07ff;
		return;
	case HARDDOOM_DRAW_END:
		d->draw_end = val & 0x07ff07ff;
		return;
	case HARDDOOM_DRAW_TEXCOORD_U:
		d->draw_texcoord_u = val & 0x003fffff;
		return;
	case HARDDOOM_DRAW_TEXCOORD_V:
		d->draw_texcoord_v = val & 0x03ffffff;
		return;
	case HARDDOOM_DRAW_STATE:
		d->draw_state = val & 0x1fff3f3f;
		harddoom_status_update(d);
		harddoom_schedule(d);
		return;
	case HARDDOOM_DRAW_LINE_SIZE:
		d->draw_line_size = val & 0x07ff07ff;
		return;
	case HARDDOOM_DRAW_COLUMN_OFFSET:
		d->draw_column_offset = val & 0x003fffff;
		return;
	/* Direct access to the cache.  */
	case HARDDOOM_CACHE_STATE:
		d->cache_state = val & 0x111f7fff;
		return;
	/* And to the FIFO.  */
	case HARDDOOM_FIFO_STATE:
		d->fifo_state = val & 0x03ff03ff;
		return;
	}
	/* Direct access to the commands.  */
	if (addr >= HARDDOOM_STATE_SURF_DST_PT && addr <= HARDDOOM_TRIGGER_SYNC && !(addr & 3)) {
		harddoom_do_command(d, addr >> 2 & 0x3f, val);
		return;
	}
	/* And to the FIFO contents.  */
	if ((addr & ~((HARDDOOM_FIFO_CMD_NUM - 1) << 2)) == HARDDOOM_FIFO_CMD(0)) {
		i = addr >> 2 & (HARDDOOM_FIFO_CMD_NUM - 1);
		d->fifo_cmd[i] = val;
		return;
	}
	fprintf(stderr, "harddoom error: invalid register write at %03x, value %08x\n", (int)addr, val);
}

static uint32_t harddoom_mmio_readb(void *opaque, hwaddr addr)
{
	int i;
	HardDoomState *d = opaque;
	if ((addr & ~(HARDDOOM_CACHE_SIZE - 1)) == HARDDOOM_CACHE_DATA_TEXTURE(0)) {
		i = addr & (HARDDOOM_CACHE_SIZE - 1);
		return d->cache_data_texture[i];
	}
	if ((addr & ~(HARDDOOM_CACHE_SIZE - 1)) == HARDDOOM_CACHE_DATA_FLAT(0)) {
		i = addr & (HARDDOOM_CACHE_SIZE - 1);
		return d->cache_data_flat[i];
	}
	if ((addr & ~(HARDDOOM_CACHE_SIZE - 1)) == HARDDOOM_CACHE_DATA_COLORMAP(0)) {
		i = addr & (HARDDOOM_CACHE_SIZE - 1);
		return d->cache_data_colormap[i];
	}
	if ((addr & ~(HARDDOOM_CACHE_SIZE - 1)) == HARDDOOM_CACHE_DATA_TRANSLATION(0)) {
		i = addr & (HARDDOOM_CACHE_SIZE - 1);
		return d->cache_data_translation[i];
	}
	fprintf(stderr, "harddoom error: byte-sized read at %03x\n", (int)addr);
	return 0xff;
}

static uint32_t harddoom_mmio_readw(void *opaque, hwaddr addr)
{
	fprintf(stderr, "harddoom error: word-sized read at %03x\n", (int)addr);
	return 0xffff;
}

static uint32_t harddoom_mmio_readl(void *opaque, hwaddr addr)
{
	HardDoomState *d = opaque;
	int i;
	switch(addr) {
	/* Documented registers... */
	case HARDDOOM_ENABLE:
		return d->enable;
	case HARDDOOM_STATUS:
		return d->status;
	case HARDDOOM_INTR:
		return d->intr;
	case HARDDOOM_INTR_ENABLE:
		return d->intr_enable;
	case HARDDOOM_SYNC_LAST:
		return d->sync_last;
	case HARDDOOM_SYNC_INTR:
		return d->sync_intr;
	case HARDDOOM_CMD_READ_PTR:
		return d->cmd_read_ptr;
	case HARDDOOM_CMD_WRITE_PTR:
		return d->cmd_write_ptr;
	case HARDDOOM_FIFO_FREE:
		return harddoom_fifo_free(d);
	/* Undocumented registers - direct TLB access.  */
	case HARDDOOM_TLB_SURF_DST_TAG:
		return d->tlb_tag[TLB_SURF_DST];
	case HARDDOOM_TLB_SURF_DST_PTE:
		return d->tlb_pte[TLB_SURF_DST];
	case HARDDOOM_TLB_SURF_SRC_TAG:
		return d->tlb_tag[TLB_SURF_SRC];
	case HARDDOOM_TLB_SURF_SRC_PTE:
		return d->tlb_pte[TLB_SURF_SRC];
	case HARDDOOM_TLB_TEXTURE_TAG:
		return d->tlb_tag[TLB_TEXTURE];
	case HARDDOOM_TLB_TEXTURE_PTE:
		return d->tlb_pte[TLB_TEXTURE];
	/* Direct state command access.  */
	case HARDDOOM_STATE_SURF_DST_PT:
		return d->state_pt[TLB_SURF_DST];
	case HARDDOOM_STATE_SURF_SRC_PT:
		return d->state_pt[TLB_SURF_SRC];
	case HARDDOOM_STATE_TEXTURE_PT:
		return d->state_pt[TLB_TEXTURE];
	case HARDDOOM_STATE_COLORMAP_ADDR:
		return d->state_colormap_addr[0];
	case HARDDOOM_STATE_TRANSLATION_ADDR:
		return d->state_colormap_addr[1];
	case HARDDOOM_STATE_FLAT_ADDR:
		return d->state_flat_addr;
	case HARDDOOM_STATE_SURF_DIMS:
		return d->state_surf_dims;
	case HARDDOOM_STATE_TEXTURE_DIMS:
		return d->state_texture_dims;
	case HARDDOOM_STATE_FILL_COLOR:
		return d->state_fill_color;
	case HARDDOOM_STATE_XY_A:
		return d->state_xy[0];
	case HARDDOOM_STATE_XY_B:
		return d->state_xy[1];
	case HARDDOOM_STATE_DRAW_PARAMS:
		return d->state_draw_params;
	case HARDDOOM_STATE_USTART:
		return d->state_uvstart[0];
	case HARDDOOM_STATE_VSTART:
		return d->state_uvstart[1];
	case HARDDOOM_STATE_USTEP:
		return d->state_uvstep[0];
	case HARDDOOM_STATE_VSTEP:
		return d->state_uvstep[1];
	/* DRAW state.  */
	case HARDDOOM_DRAW_X_CUR:
		return d->draw_x_cur;
	case HARDDOOM_DRAW_Y_CUR:
		return d->draw_y_cur;
	case HARDDOOM_DRAW_X_RESTART:
		return d->draw_x_restart;
	case HARDDOOM_DRAW_END:
		return d->draw_end;
	case HARDDOOM_DRAW_TEXCOORD_U:
		return d->draw_texcoord_u;
	case HARDDOOM_DRAW_TEXCOORD_V:
		return d->draw_texcoord_v;
	case HARDDOOM_DRAW_STATE:
		return d->draw_state;
	case HARDDOOM_DRAW_LINE_SIZE:
		return d->draw_line_size;
	case HARDDOOM_DRAW_COLUMN_OFFSET:
		return d->draw_column_offset;
	/* CACHE state.  */
	case HARDDOOM_CACHE_STATE:
		return d->cache_state;
	/* FIFO access.  */
	case HARDDOOM_FIFO_STATE:
		return d->fifo_state;
	}
	if ((addr & ~((HARDDOOM_FIFO_CMD_NUM - 1) << 2)) == HARDDOOM_FIFO_CMD(0)) {
		i = addr >> 2 & (HARDDOOM_FIFO_CMD_NUM - 1);
		return d->fifo_cmd[i];
	}
	fprintf(stderr, "harddoom error: invalid register read at %03x\n", (int)addr);
	return 0xffffffff;
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
	.endianness = DEVICE_NATIVE_ENDIAN,
};

/* Power-up reset of the device.  */
static void harddoom_reset(DeviceState *d)
{
	HardDoomState *s = container_of(d, HardDoomState, dev.qdev);
	int i;
	/* These registers play fair. */
	s->enable = 0;
	s->intr_enable = 0;
	/* But these don't; hardware is evil. */
	s->intr = mrand48() & HARDDOOM_INTR_MASK;
	s->sync_last = mrand48() & HARDDOOM_SYNC_MASK;
	s->sync_intr = mrand48() & HARDDOOM_SYNC_MASK;
	s->cmd_read_ptr = mrand48() & ~3;
	s->cmd_write_ptr = mrand48() & ~3;
	for (i = 0; i < 3; i++) {
		s->tlb_tag[i] = mrand48() & (HARDDOOM_TLB_TAG_IDX_MASK | HARDDOOM_TLB_TAG_VALID);
		s->tlb_pte[i] = mrand48() & (HARDDOOM_PTE_PHYS_MASK | HARDDOOM_PTE_VALID);
		s->state_pt[i] = mrand48() & 0x3ffffff;
	}
	s->state_colormap_addr[0] = mrand48() & 0xffffff;
	s->state_colormap_addr[1] = mrand48() & 0xffffff;
	s->state_flat_addr = mrand48() & 0xfffff;
	s->state_surf_dims = mrand48() & 0xfffff;
	s->state_texture_dims = mrand48() & 0x3fff3ff;
	s->state_fill_color = mrand48() & 0xff;
	s->state_xy[0] = mrand48() & 0x7ff7ff;
	s->state_xy[1] = mrand48() & 0x7ff7ff;
	s->state_draw_params = mrand48() & 7;
	s->state_uvstart[0] = mrand48() & 0x3ffffff;
	s->state_uvstart[1] = mrand48() & 0x3ffffff;
	s->state_uvstep[0] = mrand48() & 0x3ffffff;
	s->state_uvstep[1] = mrand48() & 0x3ffffff;
	s->draw_x_cur = mrand48() & 0x07ff07ff;
	s->draw_y_cur = mrand48() & 0x07ff07ff;
	s->draw_x_restart = mrand48() & 0x07ff07ff;
	s->draw_end = mrand48() & 0x07ff07ff;
	s->draw_texcoord_u = mrand48() & 0x003fffff;
	s->draw_texcoord_v = mrand48() & 0x03ffffff;
	s->draw_state = mrand48() & 0x1fff3f3f;
	s->draw_line_size = mrand48() & 0x07ff07ff;
	s->draw_column_offset = mrand48() & 0x003fffff;
	s->cache_state = mrand48() & 0x111f7fff;
	for (i = 0; i < HARDDOOM_CACHE_SIZE; i++) {
		s->cache_data_texture[i] = mrand48();
		s->cache_data_flat[i] = mrand48();
		s->cache_data_colormap[i] = mrand48();
		s->cache_data_translation[i] = mrand48();
	}
	s->fifo_state = mrand48() & 0x03ff03ff;
	for (i = 0; i < HARDDOOM_FIFO_CMD_NUM; i++)
		s->fifo_cmd[i] = mrand48();
	harddoom_status_update(s);
}

static int harddoom_init(PCIDevice *pci_dev)
{
	HardDoomState *d = DO_UPCAST(HardDoomState, dev, pci_dev);
	uint8_t *pci_conf = d->dev.config;

	pci_config_set_interrupt_pin(pci_conf, 1);

	memory_region_init_io(&d->mmio, OBJECT(d), &harddoom_mmio_ops, d, "harddoom", 0x1000);
	pci_register_bar(&d->dev, 0, PCI_BASE_ADDRESS_SPACE_MEMORY, &d->mmio);

	harddoom_reset(&pci_dev->qdev);
	d->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, harddoom_tick, d);

	return 0;
}

static void harddoom_exit(PCIDevice *pci_dev)
{
	HardDoomState *d = DO_UPCAST(HardDoomState, dev, pci_dev);

	timer_free(d->timer);
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
	dc->reset = harddoom_reset;
	dc->vmsd = &vmstate_harddoom;
	dc->props = harddoom_properties;
}

static TypeInfo harddoom_info = {
	.name          = "harddoom",
	.parent        = TYPE_PCI_DEVICE,
	.instance_size = sizeof(HardDoomState),
	.class_init    = harddoom_class_init,
};

static void harddoom_register_types(void)
{
	type_register_static(&harddoom_info);
}

type_init(harddoom_register_types)
