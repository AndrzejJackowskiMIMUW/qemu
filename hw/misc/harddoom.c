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
	/* These correspond directly to documented registers.  */
	uint32_t enable;
	uint32_t status;
	uint32_t intr;
	uint32_t intr_enable;
	uint32_t counter_notify;
	uint32_t cmd_read_ptr;
	uint32_t cmd_write_ptr;
	/* Undocumented registers... */
	/* The single-entry TLBs, one for source and one for destination.
	 * Tags have bits 000007ff: bits 0-9 are virtual page index, bit 10
	 * is valid bit.  The pte registers are simply contents of the cached
	 * PTE.
	 */
	uint32_t tlb_tag[3];
	uint32_t tlb_pte[3];
	/* Last sent value of the state commands.  */
	uint32_t state_pt[3];
	uint32_t state_colormap_addr[2];
	uint32_t state_flat_addr;
	uint32_t state_surf_dims;
	uint32_t state_texture_size;
	uint32_t state_fill_color;
	uint32_t state_xy[2];
	uint32_t state_texturemid;
	uint32_t state_iscale;
	uint32_t state_draw_params;
	uint32_t state_xyfrac[2];
	uint32_t state_xystep[2];
	uint32_t state_counter;
	/* State of the FIFO, bits 03ff03ff.  There are 0x200 entries, indexed
	   by 10-bit indices (each entry is visible under two indices).  Bits
	   0-9 is read pointer (index of the next entry to be read by DRAW),
	   16-25 is write pointer (index of the next entry to be written by
	   FIFO_SEND).  FIFO is empty iff read == write, full iff read ==
	   write ^ 0x200.  Situations where ((write - read) & 0x3ff) > 0x200
	   are illegal and won't be reached in proper operation of the
	   device.  */
	uint32_t fifo_state;
	/* Contents of the FIFO.  */
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
		VMSTATE_UINT32(counter_notify, HardDoomState),
		VMSTATE_UINT32(cmd_read_ptr, HardDoomState),
		VMSTATE_UINT32(cmd_write_ptr, HardDoomState),
		VMSTATE_UINT32_ARRAY(tlb_tag, HardDoomState, 3),
		VMSTATE_UINT32_ARRAY(tlb_pte, HardDoomState, 3),
		VMSTATE_UINT32_ARRAY(state_pt, HardDoomState, 3),
		VMSTATE_UINT32_ARRAY(state_colormap_addr, HardDoomState, 2),
		VMSTATE_UINT32(state_flat_addr, HardDoomState),
		VMSTATE_UINT32(state_surf_dims, HardDoomState),
		VMSTATE_UINT32(state_texture_size, HardDoomState),
		VMSTATE_UINT32(state_fill_color, HardDoomState),
		VMSTATE_UINT32_ARRAY(state_xy, HardDoomState, 2),
		VMSTATE_UINT32(state_texturemid, HardDoomState),
		VMSTATE_UINT32(state_iscale, HardDoomState),
		VMSTATE_UINT32(state_draw_params, HardDoomState),
		VMSTATE_UINT32_ARRAY(state_xyfrac, HardDoomState, 2),
		VMSTATE_UINT32_ARRAY(state_xystep, HardDoomState, 2),
		VMSTATE_UINT32(state_counter, HardDoomState),
		VMSTATE_UINT32(fifo_state, HardDoomState),
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
	// XXX
	/* DRAW busy iff draw_left non-0.  */
	if (0)
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
	int used = (wptr - rptr) & 0x3ff;
	/* This considers overfull FIFO to have free slots.
	 * It's part of the fun.  */
	return (0x200 - used) & 0x3ff;
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
	// XXX
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
	/* Set TAGs to 0, including their valid bits.  */
	for (int i = 0; i < 3; i++)
		d->tlb_tag[i] = 0;
}

/* Checks if command is valid (and not a JUMP).  */
static bool harddoom_valid_cmd(uint32_t val) {
	/* JUMPs are handled elsewhere.  If we see one here, it's bad.  */
	if (HARDDOOM_CMD_EXTR_TYPE_HI(val) == HARDDOOM_CMD_TYPE_HI_JUMP)
		return false;
	switch (HARDDOOM_CMD_EXTR_TYPE(val)) {
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
		case HARDDOOM_CMD_TYPE_MELT_COLUMN:
			return !(val & ~0xfcfff7ff);
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
		case HARDDOOM_CMD_TYPE_TEXTURE_SIZE:
			return !(val & ~0xfc3fffff);
		case HARDDOOM_CMD_TYPE_FILL_COLOR:
			return !(val & ~0xfc0000ff);
		case HARDDOOM_CMD_TYPE_XY_A:
		case HARDDOOM_CMD_TYPE_XY_B:
			return !(val & ~0xfc7ff7ff);
		case HARDDOOM_CMD_TYPE_TEXTUREMID:
		case HARDDOOM_CMD_TYPE_ISCALE:
			return !(val & ~0xffffffff);
		case HARDDOOM_CMD_TYPE_DRAW_PARAMS:
			return !(val & ~0xffff7ff7);
		case HARDDOOM_CMD_TYPE_XFRAC:
		case HARDDOOM_CMD_TYPE_YFRAC:
		case HARDDOOM_CMD_TYPE_XSTEP:
		case HARDDOOM_CMD_TYPE_YSTEP:
			return !(val & ~0xffffffff);
		case HARDDOOM_CMD_TYPE_COUNTER:
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
		d->fifo_cmd[wptr & 0x1ff] = val;
		wptr++;
		wptr &= 0x3ff;
		d->fifo_state = rptr | wptr << 16;
		/* DRAW has work to do now.  */
		harddoom_status_update(d);
		harddoom_schedule(d);
	}
}

/* Executes a COPY_RECT command. XXX */
static void harddoom_trigger_copy_rect(HardDoomState *d, uint32_t val) {
	int w = HARDDOOM_CMD_EXTR_RECT_WIDTH(val);
	int h = HARDDOOM_CMD_EXTR_RECT_HEIGHT(val);
	// XXX
	(void)w;
	(void)h;
	harddoom_status_update(d);
	harddoom_schedule(d);
}

/* Executes a FILL_RECT command. XXX */
static void harddoom_trigger_fill_rect(HardDoomState *d, uint32_t val) {
	int w = HARDDOOM_CMD_EXTR_RECT_WIDTH(val);
	int h = HARDDOOM_CMD_EXTR_RECT_HEIGHT(val);
	// XXX
	(void)w;
	(void)h;
	harddoom_status_update(d);
	harddoom_schedule(d);
}

/* Executes a DRAW_LINE command. XXX */
static void harddoom_trigger_draw_line(HardDoomState *d, uint32_t val) {
	// XXX
	harddoom_status_update(d);
	harddoom_schedule(d);
}

/* Executes a DRAW_BACKGROUND command. XXX */
static void harddoom_trigger_draw_background(HardDoomState *d, uint32_t val) {
	// XXX
	harddoom_status_update(d);
	harddoom_schedule(d);
}

/* Executes a DRAW_COLUMN command. XXX */
static void harddoom_trigger_draw_column(HardDoomState *d, uint32_t val) {
	uint32_t column_offset = HARDDOOM_CMD_EXTR_COLUMN_OFFSET(val);
	// XXX
	(void)column_offset;
	harddoom_status_update(d);
	harddoom_schedule(d);
}

/* Executes a DRAW_SPAN command. XXX */
static void harddoom_trigger_draw_span(HardDoomState *d, uint32_t val) {
	// XXX
	harddoom_status_update(d);
	harddoom_schedule(d);
}

/* Executes a MELT_COLUMN command. XXX */
static void harddoom_trigger_melt_column(HardDoomState *d, uint32_t val) {
	int x = HARDDOOM_CMD_EXTR_MELT_X(val);
	int y_off = HARDDOOM_CMD_EXTR_MELT_Y_OFFSET(val);
	// XXX
	(void)x;
	(void)y_off;
	harddoom_status_update(d);
	harddoom_schedule(d);
}

// XXX
#if 0
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
#endif

/* Executes a command.  */
static void harddoom_do_command(HardDoomState *d, int cmd, uint32_t val) {
	val &= ~0xfc000000;
	switch (cmd) {
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
	case HARDDOOM_CMD_TYPE_MELT_COLUMN:
		harddoom_trigger_melt_column(d, val);
		break;
	case HARDDOOM_CMD_TYPE_SURF_DST_PT:
		d->state_pt[TLB_SURF_DST] = val;
		break;
	case HARDDOOM_CMD_TYPE_SURF_SRC_PT:
		d->state_pt[TLB_SURF_SRC] = val;
		break;
	case HARDDOOM_CMD_TYPE_TEXTURE_PT:
		d->state_pt[TLB_TEXTURE] = val;
		break;
	case HARDDOOM_CMD_TYPE_COLORMAP_ADDR:
		d->state_colormap_addr[0] = val & 0xffffff;
		break;
	case HARDDOOM_CMD_TYPE_TRANSLATION_ADDR:
		d->state_colormap_addr[1] = val & 0xffffff;
		break;
	case HARDDOOM_CMD_TYPE_FLAT_ADDR:
		d->state_flat_addr = val & 0xfffff;
		break;
	case HARDDOOM_CMD_TYPE_SURF_DIMS:
		d->state_surf_dims = val & 0xffffff;
		break;
	case HARDDOOM_CMD_TYPE_TEXTURE_SIZE:
		d->state_texture_size = val & 0x3fffff;
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
	case HARDDOOM_CMD_TYPE_TEXTUREMID:
		d->state_texturemid = val;
		break;
	case HARDDOOM_CMD_TYPE_ISCALE:
		d->state_iscale = val;
		break;
	case HARDDOOM_CMD_TYPE_DRAW_PARAMS:
		d->state_iscale = val & 0x3ff7ff7;
		break;
	case HARDDOOM_CMD_TYPE_XFRAC:
		d->state_xyfrac[0] = val;
		break;
	case HARDDOOM_CMD_TYPE_YFRAC:
		d->state_xyfrac[1] = val;
		break;
	case HARDDOOM_CMD_TYPE_XSTEP:
		d->state_xystep[0] = val;
		break;
	case HARDDOOM_CMD_TYPE_YSTEP:
		d->state_xystep[1] = val;
		break;
	case HARDDOOM_CMD_TYPE_COUNTER:
		d->state_counter = val;
		break;
	default:
		fprintf(stderr, "harddoom error: invalid command %d executed [param %08x]\n", cmd, val);
	}
}

/* Main loop of the DRAW unit.  Does a random amount of work.  */
static void harddoom_tick_draw(HardDoomState *d) {
	/* Make the device as evil as easily possible by randomizing everything. */
	/* First, determine how many units of work we do this time. */
	int work_cnt = lrand48() & 0x3ff;
	// XXX
	(void)work_cnt;
#if 0
	while (work_cnt > 0) {
		/* We may self-disable while working if we hit an error.  */
		if (!(d->enable & HARDDOOM_ENABLE_DRAW))
			return;
		if (d->status & HARDDOOM_STATUS_DRAW) {
			/* First, draw if there's drawing to do.  */
			int left_pixels = HARDDOOM_DRAW_LEFT_PIXELS(d->draw_left);
			int left_lines = HARDDOOM_DRAW_LEFT_LINES(d->draw_left);
			if (left_pixels) {
				/* We're already working on a line - continue. */
				uint32_t src_addr = 0, dst_addr;
				bool is_blit = !!(d->draw_state & HARDDOOM_DRAW_STATE_MODE_BLIT);
				if (is_blit)
					src_addr = harddoom_canvas_addr(d, true);
				dst_addr = harddoom_canvas_addr(d, false);
				/* Could be a canvas overflow, or a page fault.  */
				if (!(d->enable & HARDDOOM_ENABLE_DRAW))
					return;
				uint8_t buf[0x1000];
				/* Determine how many pixels to process this run.  */
				int cnt = left_pixels;
				int cnt_src, cnt_dst, cnt_src_page, cnt_dst_page;
				if (d->draw_state & HARDDOOM_DRAW_STATE_DIR_X_LEFT) {
					/* Don't go out of this page.  */
					cnt_src_page = (src_addr & 0xfff) + 1;
					cnt_dst_page = (dst_addr & 0xfff) + 1;
					/* And don't go out of the canvas (if this is the limitting factor, there'll be an error in the next iteration.  */
					cnt_src = HARDDOOM_CMD_POS_X(d->src_pos) + 1;
					cnt_dst = HARDDOOM_CMD_POS_X(d->dst_pos) + 1;
				} else {
					cnt_src_page = 0x1000 - (src_addr & 0xfff);
					cnt_dst_page = 0x1000 - (dst_addr & 0xfff);
					cnt_src = HARDDOOM_CMD_WIDTH(d->canvas_dims) - HARDDOOM_CMD_POS_X(d->src_pos);
					cnt_dst = HARDDOOM_CMD_WIDTH(d->canvas_dims) - HARDDOOM_CMD_POS_X(d->dst_pos);
				}
				if (is_blit) {
					if (cnt_src < cnt)
						cnt = cnt_src;
					if (cnt_src_page < cnt)
						cnt = cnt_src_page;
				}
				if (cnt_dst < cnt)
					cnt = cnt_dst;
				if (cnt_dst_page < cnt)
					cnt = cnt_dst_page;
				/* Start address.  */
				if (d->draw_state & HARDDOOM_DRAW_STATE_DIR_X_LEFT) {
					/* If drawing leftwards, pos points at
					 * the rightmost pixel of current span
					 * - adjust it.  */
					src_addr -= cnt - 1;
					dst_addr -= cnt - 1;
				}
				if (is_blit) {
					/* Read the source.  */
					pci_dma_read(&d->dev, src_addr, buf, cnt);
					/* Update source position.  */
					int pos_y = HARDDOOM_CMD_POS_Y(d->src_pos);
					int pos_x = HARDDOOM_CMD_POS_X(d->src_pos);
					if (d->draw_state & HARDDOOM_DRAW_STATE_DIR_X_LEFT)
						pos_x -= cnt;
					else
						pos_x += cnt;
					/* Minor hw bug #3: no overflow check here.
					 * If there are more pixels to draw, the next
					 * span will give an error... unless width is 2048 pixels, which will happily wrap to the other edge instead.  */
					pos_x &= 0x7ff;
					d->src_pos = pos_x << 8 | pos_y << 20;
				} else {
					memset(buf, d->fill_color >> 8, cnt);
				}
				/* Write the destination.  */
				pci_dma_write(&d->dev, dst_addr, buf, cnt);
				/* Update destination position.  */
				int pos_y = HARDDOOM_CMD_POS_Y(d->dst_pos);
				int pos_x = HARDDOOM_CMD_POS_X(d->dst_pos);
				if (d->draw_state & HARDDOOM_DRAW_STATE_DIR_X_LEFT)
					pos_x -= cnt;
				else
					pos_x += cnt;
				pos_x &= 0x7ff;
				d->dst_pos = pos_x << 8 | pos_y << 20;
				/* And mark as done.  */
				left_pixels -= cnt;
			} else if (left_lines) {
				/* Start work on a new line. */
				left_lines--;
				left_pixels = HARDDOOM_DRAW_STATE_WIDTHM1(d->draw_state) + 1;
				int dst_pos_x = HARDDOOM_CMD_POS_X(d->dst_pos);
				int dst_pos_y = HARDDOOM_CMD_POS_Y(d->dst_pos);
				int src_pos_x = HARDDOOM_CMD_POS_X(d->src_pos);
				int src_pos_y = HARDDOOM_CMD_POS_Y(d->src_pos);
				/* Go to the next line.  */
				if (d->draw_state & HARDDOOM_DRAW_STATE_DIR_Y_UP) {
					src_pos_y--;
					dst_pos_y--;
				} else {
					src_pos_y++;
					dst_pos_y++;
				}
				/* Again, no overflow checking here.  */
				src_pos_y &= 0x7ff;
				dst_pos_y &= 0x7ff;
				/* Go back to the beginning of the line.  */
				if (d->draw_state & HARDDOOM_DRAW_STATE_DIR_X_LEFT) {
					src_pos_x += left_pixels;
					dst_pos_x += left_pixels;
				} else {
					src_pos_x -= left_pixels;
					dst_pos_x -= left_pixels;
				}
				src_pos_x &= 0x7ff;
				dst_pos_x &= 0x7ff;
				/* Update the positions.  */
				if (d->draw_state & HARDDOOM_DRAW_STATE_MODE_BLIT)
					d->src_pos = src_pos_x << 8 | src_pos_y << 20;
				d->dst_pos = dst_pos_x << 8 | dst_pos_y << 20;
			}
			/* Update draw_left, count it as one unit of work.  */
			d->draw_left = left_pixels | left_lines << 12;
			work_cnt--;
		} else if (d->status & HARDDOOM_STATUS_FIFO) {
			/* No drawing in progress, execute a command if there is one.  */
			int rptr = HARDDOOM_FIFO_STATE_READ(d->fifo_state);
			int wptr = HARDDOOM_FIFO_STATE_WRITE(d->fifo_state);
			uint32_t cmd = d->fifo_cmd[rptr & 0x1ff];
			rptr++;
			rptr &= 0x3ff;
			d->fifo_state = rptr | wptr << 16;
			harddoom_do_command(d, HARDDOOM_CMD_TYPE(cmd), cmd);
			/* Count commands as 64 lines... */
			work_cnt -= 0x40;
		} else {
			/* Nothing to do.  */
			return;
		}
		harddoom_status_update(d);
	}
#endif
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
	/* Documented registers... */
	if (addr == HARDDOOM_ENABLE) {
		d->enable = val & (HARDDOOM_ENABLE_DRAW | HARDDOOM_ENABLE_FIFO | HARDDOOM_ENABLE_FETCH_CMD);
		if (val & ~(HARDDOOM_ENABLE_DRAW | HARDDOOM_ENABLE_FIFO | HARDDOOM_ENABLE_FETCH_CMD))
			fprintf(stderr, "harddoom error: invalid ENABLE value %08x\n", val);
		harddoom_schedule(d);
		return;
	}
	if (addr == HARDDOOM_INTR) {
		d->intr &= ~val;
		if (val & ~HARDDOOM_INTR_MASK)
			fprintf(stderr, "harddoom error: invalid INTR value %08x\n", val);
		harddoom_status_update(d);
		return;
	}
	if (addr == HARDDOOM_INTR_ENABLE) {
		d->intr_enable = val & HARDDOOM_INTR_MASK;
		if (val & ~HARDDOOM_INTR_MASK)
			fprintf(stderr, "harddoom error: invalid INTR_ENABLE value %08x\n", val);
		harddoom_status_update(d);
		return;
	}
	if (addr == HARDDOOM_RESET) {
		if (val & HARDDOOM_RESET_DRAW)
			harddoom_reset_draw(d);
		if (val & HARDDOOM_RESET_FIFO)
			harddoom_reset_fifo(d);
		if (val & HARDDOOM_RESET_TLB)
			harddoom_reset_tlb(d);
		if (val & ~(HARDDOOM_RESET_DRAW | HARDDOOM_RESET_FIFO | HARDDOOM_RESET_TLB))
			fprintf(stderr, "harddoom error: invalid RESET value %08x\n", val);
		return;
	}
	if (addr == HARDDOOM_COUNTER_NOTIFY) {
		d->counter_notify = val;
		if (val & ~HARDDOOM_COUNTER_MASK)
			fprintf(stderr, "harddoom error: invalid COUNTER_NOTIFY value %08x\n", val);
		return;
	}
	if (addr == HARDDOOM_FIFO_SEND) {
		harddoom_fifo_send(d, val);
		return;
	}
	if (addr == HARDDOOM_CMD_READ_PTR) {
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
	}
	if (addr == HARDDOOM_CMD_WRITE_PTR) {
		if (val & 3) {
			fprintf(stderr, "harddoom error: CMD_WRITE_PTR not aligned\n");
			val &= ~3;
		}
		d->cmd_write_ptr = val;
		harddoom_status_update(d);
		harddoom_schedule(d);
		return;
	}
	/* Undocumented registers -- direct access to TLB state.  */
	if (addr == HARDDOOM_TLB_SURF_DST_TAG) {
		d->tlb_tag[TLB_SURF_DST] = val & (HARDDOOM_TLB_TAG_IDX_MASK | HARDDOOM_TLB_TAG_VALID);
		return;
	}
	if (addr == HARDDOOM_TLB_SURF_DST_PTE) {
		d->tlb_pte[TLB_SURF_DST] = val & (HARDDOOM_PTE_PHYS_MASK | HARDDOOM_PTE_VALID);
		return;
	}
	if (addr == HARDDOOM_TLB_SURF_SRC_TAG) {
		d->tlb_tag[TLB_SURF_SRC] = val & (HARDDOOM_TLB_TAG_IDX_MASK | HARDDOOM_TLB_TAG_VALID);
		return;
	}
	if (addr == HARDDOOM_TLB_SURF_SRC_PTE) {
		d->tlb_pte[TLB_SURF_SRC] = val & (HARDDOOM_PTE_PHYS_MASK | HARDDOOM_PTE_VALID);
		return;
	}
	if (addr == HARDDOOM_TLB_TEXTURE_TAG) {
		d->tlb_tag[TLB_TEXTURE] = val & (HARDDOOM_TLB_TAG_IDX_MASK | HARDDOOM_TLB_TAG_VALID);
		return;
	}
	if (addr == HARDDOOM_TLB_TEXTURE_PTE) {
		d->tlb_pte[TLB_TEXTURE] = val & (HARDDOOM_PTE_PHYS_MASK | HARDDOOM_PTE_VALID);
		return;
	}
	/* Direct access to the commands.  */
	if (addr >= HARDDOOM_TRIGGER_COPY_RECT && addr <= HARDDOOM_STATE_COUNTER && !(addr & 3)) {
		harddoom_do_command(d, addr >> 2 & 0x3f, val);
		return;
	}
	// XXX internal crap
	/* And to the FIFO.  */
	if (addr == HARDDOOM_FIFO_STATE) {
		d->fifo_state = val & 0x03ff03ff;
		return;
	}
	for (i = 0; i < HARDDOOM_FIFO_CMD_NUM; i++) {
		if (addr == HARDDOOM_FIFO_CMD(i)) {
			d->fifo_cmd[i] = val;
			return;
		}
	}
	fprintf(stderr, "harddoom error: invalid register write at %03x, value %08x\n", (int)addr, val);
}

static uint32_t harddoom_mmio_readb(void *opaque, hwaddr addr)
{
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
	/* Documented registers... */
	if (addr == HARDDOOM_ENABLE)
		return d->enable;
	if (addr == HARDDOOM_STATUS)
		return d->status;
	if (addr == HARDDOOM_INTR)
		return d->intr;
	if (addr == HARDDOOM_INTR_ENABLE)
		return d->intr_enable;
	if (addr == HARDDOOM_COUNTER_NOTIFY)
		return d->counter_notify;
	if (addr == HARDDOOM_CMD_READ_PTR)
		return d->cmd_read_ptr;
	if (addr == HARDDOOM_CMD_WRITE_PTR)
		return d->cmd_write_ptr;
	if (addr == HARDDOOM_FIFO_FREE)
		return harddoom_fifo_free(d);
	/* Undocumented registers - direct TLB access.  */
	if (addr == HARDDOOM_TLB_SURF_DST_TAG)
		return d->tlb_tag[TLB_SURF_DST];
	if (addr == HARDDOOM_TLB_SURF_DST_PTE)
		return d->tlb_pte[TLB_SURF_DST];
	if (addr == HARDDOOM_TLB_SURF_SRC_TAG)
		return d->tlb_tag[TLB_SURF_SRC];
	if (addr == HARDDOOM_TLB_SURF_SRC_PTE)
		return d->tlb_pte[TLB_SURF_SRC];
	if (addr == HARDDOOM_TLB_TEXTURE_TAG)
		return d->tlb_tag[TLB_TEXTURE];
	if (addr == HARDDOOM_TLB_TEXTURE_PTE)
		return d->tlb_pte[TLB_TEXTURE];
	/* Direct state command access.  */
	if (addr == HARDDOOM_STATE_SURF_DST_PT)
		return d->state_pt[TLB_SURF_DST];
	if (addr == HARDDOOM_STATE_SURF_SRC_PT)
		return d->state_pt[TLB_SURF_SRC];
	if (addr == HARDDOOM_STATE_TEXTURE_PT)
		return d->state_pt[TLB_TEXTURE];
	if (addr == HARDDOOM_STATE_COLORMAP_ADDR)
		return d->state_colormap_addr[0];
	if (addr == HARDDOOM_STATE_TRANSLATION_ADDR)
		return d->state_colormap_addr[1];
	if (addr == HARDDOOM_STATE_FLAT_ADDR)
		return d->state_flat_addr;
	if (addr == HARDDOOM_STATE_SURF_DIMS)
		return d->state_surf_dims;
	if (addr == HARDDOOM_STATE_TEXTURE_SIZE)
		return d->state_texture_size;
	if (addr == HARDDOOM_STATE_FILL_COLOR)
		return d->state_fill_color;
	if (addr == HARDDOOM_STATE_XY_A)
		return d->state_xy[0];
	if (addr == HARDDOOM_STATE_XY_B)
		return d->state_xy[1];
	if (addr == HARDDOOM_STATE_TEXTUREMID)
		return d->state_texturemid;
	if (addr == HARDDOOM_STATE_ISCALE)
		return d->state_iscale;
	if (addr == HARDDOOM_STATE_DRAW_PARAMS)
		return d->state_draw_params;
	if (addr == HARDDOOM_STATE_XFRAC)
		return d->state_xyfrac[0];
	if (addr == HARDDOOM_STATE_YFRAC)
		return d->state_xyfrac[1];
	if (addr == HARDDOOM_STATE_XSTEP)
		return d->state_xystep[0];
	if (addr == HARDDOOM_STATE_YSTEP)
		return d->state_xystep[1];
	if (addr == HARDDOOM_STATE_COUNTER)
		return d->state_counter;
	// XXX internal crap
	/* FIFO access.  */
	if (addr == HARDDOOM_FIFO_STATE)
		return d->fifo_state;
	for (i = 0; i < HARDDOOM_FIFO_CMD_NUM; i++) {
		if (addr == HARDDOOM_FIFO_CMD(i))
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
	s->counter_notify = mrand48() & HARDDOOM_COUNTER_MASK;
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
	s->state_texture_size = mrand48() & 0x3fffff;
	s->state_fill_color = mrand48() & 0xff;
	s->state_xy[0] = mrand48() & 0x7ff7ff;
	s->state_xy[1] = mrand48() & 0x7ff7ff;
	s->state_texturemid = mrand48() & 0x3ffffff;
	s->state_iscale = mrand48() & 0x3ffffff;
	s->state_draw_params = mrand48() & 0x3ff7fff;
	s->state_xyfrac[0] = mrand48() & 0x3ffffff;
	s->state_xyfrac[1] = mrand48() & 0x3ffffff;
	s->state_xystep[0] = mrand48() & 0x3ffffff;
	s->state_xystep[1] = mrand48() & 0x3ffffff;
	s->state_counter = mrand48() & 0x3ffffff;
	// XXX internal crap here
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
