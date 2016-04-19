#ifndef HARDDOOM_H
#define HARDDOOM_H

/* PCI ids */

#define HARDDOOM_VENDOR_ID			0x1af4
#define HARDDOOM_DEVICE_ID			0x10f4

/* Registers */

/* Enables active units of the device.  TLB is passive and doesn't have
 * an enable (disable DRAW instead).  */
#define HARDDOOM_ENABLE				0x000
#define HARDDOOM_ENABLE_DRAW			0x00000001
#define HARDDOOM_ENABLE_FIFO			0x00000002
#define HARDDOOM_ENABLE_FETCH_CMD		0x00000004
/* Status of device units -- 1 means they have work to do.  */
#define HARDDOOM_STATUS				0x004
#define HARDDOOM_STATUS_DRAW			0x00000001
#define HARDDOOM_STATUS_FIFO			0x00000002
#define HARDDOOM_STATUS_FETCH_CMD		0x00000004
/* Interrupt status.  */
#define HARDDOOM_INTR				0x008
#define HARDDOOM_INTR_NOTIFY			0x00000001
#define HARDDOOM_INTR_INVALID_CMD		0x00000002
#define HARDDOOM_INTR_FIFO_OVERFLOW		0x00000004
#define HARDDOOM_INTR_SURF_OVERFLOW		0x00000008
#define HARDDOOM_INTR_PAGE_FAULT_SURF_DST	0x00000010
#define HARDDOOM_INTR_PAGE_FAULT_SURF_SRC	0x00000020
#define HARDDOOM_INTR_PAGE_FAULT_TEXTURE	0x00000040
#define HARDDOOM_INTR_MASK			0x000001ff
/* And enable (same bitfields).  */
#define HARDDOOM_INTR_ENABLE			0x00c
/* The reset register.  Punching 1 will clear all pending work.  There is
 * no reset for FETCH_CMD (initialize CMD_*_PTR instead).  */
#define HARDDOOM_RESET				0x010
#define HARDDOOM_RESET_DRAW			0x00000001
#define HARDDOOM_RESET_FIFO			0x00000002
#define HARDDOOM_RESET_TLB			0x00000008
/* The value that will trigger a NOTIFY when reached by STATE_COUNTER.  */
#define HARDDOOM_COUNTER_NOTIFY			0x014
#define HARDDOOM_COUNTER_MASK			0x03ffffff
/* Command read pointer -- whenever not equal to CMD_WRITE_PTR, FETCH_CMD will
 * fetch command from here and increment.  */
#define HARDDOOM_CMD_READ_PTR			0x018
/* Command write pointer -- FETCH_CMD halts when it hits this address.  */
#define HARDDOOM_CMD_WRITE_PTR			0x01c
/* Direct command submission (goes to FIFO bypassing FETCH_CMD).  */
#define HARDDOOM_FIFO_SEND			0x020
/* Read-only number of free slots in FIFO.  */
#define HARDDOOM_FIFO_FREE			0x024
/* Internal state of the FIFO -- read and write pointers.
 * The pointers use the double map trick.  */
#define HARDDOOM_FIFO_STATE			0x028
#define HARDDOOM_FIFO_STATE_READ(st)		((st) & 0x3ff)
#define HARDDOOM_FIFO_STATE_WRITE(st)		((st) >> 16 & 0x3ff)

/* Internal state of the TLB.  */
#define HARDDOOM_TLB_SURF_DST_TAG		0x040
#define HARDDOOM_TLB_SURF_DST_PTE		0x044
#define HARDDOOM_TLB_SURF_SRC_TAG		0x048
#define HARDDOOM_TLB_SURF_SRC_PTE		0x04c
#define HARDDOOM_TLB_TEXTURE_TAG		0x050
#define HARDDOOM_TLB_TEXTURE_PTE		0x054
#define HARDDOOM_TLB_TAG_IDX_MASK		0x000003ff
#define HARDDOOM_TLB_TAG_VALID			0x00000400

/* Internal DRAW trigger registers.  */
#define HARDDOOM_TRIGGER_COPY_RECT		0x140
#define HARDDOOM_TRIGGER_FILL_RECT		0x144
#define HARDDOOM_TRIGGER_DRAW_LINE		0x148
#define HARDDOOM_TRIGGER_DRAW_BACKGROUND	0x14c
#define HARDDOOM_TRIGGER_DRAW_SPAN		0x150
#define HARDDOOM_TRIGGER_DRAW_COLUMN		0x154
#define HARDDOOM_TRIGGER_MELT_COLUMN		0x158

/* Internal DRAW state registers.  */
#define HARDDOOM_STATE_SURF_DST_PT		0x180
#define HARDDOOM_STATE_SURF_SRC_PT		0x188
#define HARDDOOM_STATE_TEXTURE_PT		0x184
#define HARDDOOM_STATE_FLAT_ADDR		0x18c
#define HARDDOOM_STATE_COLORMAP_ADDR		0x190
#define HARDDOOM_STATE_TRANSLATION_ADDR		0x194
#define HARDDOOM_STATE_SURF_DIMS		0x19c
#define HARDDOOM_STATE_TEXTURE_SIZE		0x1a0
#define HARDDOOM_STATE_FILL_COLOR		0x1a4
#define HARDDOOM_STATE_XY_A			0x1a8
#define HARDDOOM_STATE_XY_B			0x1ac
#define HARDDOOM_STATE_TEXTUREMID		0x1b0
#define HARDDOOM_STATE_ISCALE			0x1b4
#define HARDDOOM_STATE_DRAW_PARAMS		0x1b8
#define HARDDOOM_STATE_XFRAC			0x1c0
#define HARDDOOM_STATE_YFRAC			0x1c4
#define HARDDOOM_STATE_XSTEP			0x1c8
#define HARDDOOM_STATE_YSTEP			0x1cc
#define HARDDOOM_STATE_COUNTER			0x1fc

/* The contents of the FIFO (internal).  */
#define HARDDOOM_FIFO_CMD(x)			(0x800 + (x) * 4)
#define HARDDOOM_FIFO_CMD_NUM			0x200


/* Commands */

/* Jump in the command buffer.  */
#define HARDDOOM_CMD_TYPE_HI_JUMP		0x0

/* V_CopyRect: The usual blit.  Rectangle size passed directly.  */
#define HARDDOOM_CMD_TYPE_COPY_RECT		0x10
/* V_FillRect: The usual solid fill.  Rectangle size passed directly.  */
#define HARDDOOM_CMD_TYPE_FILL_RECT		0x11
/* V_DrawLine: The usual solid line. */
#define HARDDOOM_CMD_TYPE_DRAW_LINE		0x12
/* V_DrawBackground: Fill whole FB with repeated flat. */
#define HARDDOOM_CMD_TYPE_DRAW_BACKGROUND	0x13
/* R_DrawColumn. */
#define HARDDOOM_CMD_TYPE_DRAW_COLUMN		0x14
/* R_DrawSpan. */
#define HARDDOOM_CMD_TYPE_DRAW_SPAN		0x15
/* wipe_doMelt: The melt effect, one column at a time. */
#define HARDDOOM_CMD_TYPE_MELT_COLUMN		0x16

/* Surface to render to, all commands. */
#define HARDDOOM_CMD_TYPE_SURF_DST_PT		0x20
/* Source surface for COPY_RECT, new source surface for MELT_COLUMN. */
#define HARDDOOM_CMD_TYPE_SURF_SRC_PT		0x21
/* Texture for DRAW_COLUMN, old source surface for MELT_COLUMN. */
#define HARDDOOM_CMD_TYPE_TEXTURE_PT		0x22
/* The flat for DRAW_BACKGROUND and DRAW_SPAN. */
#define HARDDOOM_CMD_TYPE_FLAT_ADDR		0x23
/* Fade/effect color map for DRAW_COLUMN and DRAW_SPAN, used iff DRAW_PARAMS_COLORMAP set. */
#define HARDDOOM_CMD_TYPE_COLORMAP_ADDR		0x24
/* Palette translation color map for DRAW_COLUMN and DRAW_SPAN, used iff DRAW_PARAMS_TRANSLATE set. */
#define HARDDOOM_CMD_TYPE_TRANSLATION_ADDR	0x25
/* Dimensions of all SURFs in use. */
#define HARDDOOM_CMD_TYPE_SURF_DIMS		0x26
/* Byte size of the texture for DRAW_COLUMN. */
#define HARDDOOM_CMD_TYPE_TEXTURE_SIZE		0x27
/* Solid fill color for FILL_RECT and DRAW_LINE. */
#define HARDDOOM_CMD_TYPE_FILL_COLOR		0x28
/* Flags for DRAW_COLUMN and DRAW_SPAN (FUZZ, TRANSLATE, COLORMAP).
 * Also rarely changing parameters for DRAW_COLUMN (horizon line, texture height).
 */
#define HARDDOOM_CMD_TYPE_DRAW_PARAMS		0x29
/* Destination rect top left corner for COPY_RECT, FILL_RECT.
 * First end point for DRAW_LINE.
 * Top end point for DRAW_COLUMN.
 * Left end point for DRAW_SPAN.
 */
#define HARDDOOM_CMD_TYPE_XY_A			0x2a
/* Source rect corner top left for COPY_RECT.
 * Second end point for DRAW_LINE.
 * Botoom end point for DRAW_COLUMN.
 * Right end point for DRAW_SPAN.
 */
#define HARDDOOM_CMD_TYPE_XY_B			0x2b
/* Tex coord at horizon line for DRAW_COLUMN. */
#define HARDDOOM_CMD_TYPE_TEXTUREMID		0x2c
/* Tex coord scale factor for DRAW_COLUMN. */
#define HARDDOOM_CMD_TYPE_ISCALE		0x2d
/* Tex coord start for DRAW_SPAN. */
#define HARDDOOM_CMD_TYPE_XFRAC			0x30
#define HARDDOOM_CMD_TYPE_YFRAC			0x31
/* Tex coord derivative for DRAW_SPAN. */
#define HARDDOOM_CMD_TYPE_XSTEP			0x32
#define HARDDOOM_CMD_TYPE_YSTEP			0x33
/* Set the sync counter.  */
#define HARDDOOM_CMD_TYPE_COUNTER		0x3f

#define HARDDOOM_DRAW_PARAMS_FUZZ		0x1
#define HARDDOOM_DRAW_PARAMS_TRANSLATE		0x2
#define HARDDOOM_DRAW_PARAMS_COLORMAP		0x4

#define HARDDOOM_CMD_JUMP(addr)			(HARDDOOM_CMD_TYPE_HI_JUMP << 30 | (addr) >> 2)
#define HARDDOOM_CMD_COPY_RECT(w, h)		(HARDDOOM_CMD_TYPE_COPY_RECT << 26 | (h) << 12 | (w))
#define HARDDOOM_CMD_FILL_RECT(w, h)		(HARDDOOM_CMD_TYPE_FILL_RECT << 26 | (h) << 12 | (w))
#define HARDDOOM_CMD_DRAW_LINE			(HARDDOOM_CMD_TYPE_DRAW_LINE << 26)
#define HARDDOOM_CMD_DRAW_BACKGROUND		(HARDDOOM_CMD_TYPE_DRAW_BACKGROUND << 26)
#define HARDDOOM_CMD_DRAW_COLUMN(iscale)	(HARDDOOM_CMD_TYPE_DRAW_COLUMN << 26 | (iscale))
#define HARDDOOM_CMD_DRAW_SPAN			(HARDDOOM_CMD_TYPE_DRAW_SPAN << 26)
#define HARDDOOM_CMD_MELT_COLUMN(x, yoff)	(HARDDOOM_CMD_TYPE_MELT_COLUMN << 26 | (yoff) << 12 | (x))
#define HARDDOOM_CMD_SURF_DST_PT(addr)		(HARDDOOM_CMD_TYPE_SURF_DST_PT << 26 | (addr) >> 6)
#define HARDDOOM_CMD_SURF_SRC_PT(addr)		(HARDDOOM_CMD_TYPE_SURF_SRC_PT << 26 | (addr) >> 6)
#define HARDDOOM_CMD_TEXTURE_PT(addr)		(HARDDOOM_CMD_TYPE_TEXTURE_PT << 26 | (addr) >> 6)
#define HARDDOOM_CMD_FLAT_ADDR(addr)		(HARDDOOM_CMD_TYPE_FLAT_ADDR << 26 | (addr) >> 12)
#define HARDDOOM_CMD_COLORMAP_ADDR(addr)	(HARDDOOM_CMD_TYPE_COLORMAP_ADDR << 26 | (addr) >> 8)
#define HARDDOOM_CMD_TRANSLATION_ADDR(addr)	(HARDDOOM_CMD_TYPE_TRANSLATION_ADDR << 26 | (addr) >> 8)
#define HARDDOOM_CMD_SURF_DIMS(w, h)		(HARDDOOM_CMD_TYPE_SURF_DIMS << 26 | (w) >> 6 | (h) << 8)
#define HARDDOOM_CMD_TEXTURE_SIZE(sz)		(HARDDOOM_CMD_TYPE_TEXTURE_SIZE << 26 | (sz) - 1)
#define HARDDOOM_CMD_FILL_COLOR(color)		(HARDDOOM_CMD_TYPE_FILL_COLOR << 26 | (color))
#define HARDDOOM_CMD_DRAW_PARAMS(flags, cy, th)	(HARDDOOM_CMD_TYPE_DRAW_PARAMS << 26 | (th) << 16 | (cy) << 4 | (flags))
#define HARDDOOM_CMD_XY_A(x, y)			(HARDDOOM_CMD_TYPE_XY_A << 26 | (y) << 12 | (x))
#define HARDDOOM_CMD_XY_B(x, y)			(HARDDOOM_CMD_TYPE_XY_B << 26 | (y) << 12 | (x))
#define HARDDOOM_CMD_TEXTUREMID(tmid)		(HARDDOOM_CMD_TYPE_TEXTUREMID << 26 | (tmid))
#define HARDDOOM_CMD_ISCALE(iscale)		(HARDDOOM_CMD_TYPE_ISCALE << 26 | (iscale))
#define HARDDOOM_CMD_XFRAC(arg)			(HARDDOOM_CMD_TYPE_XFRAC << 26 | (arg))
#define HARDDOOM_CMD_YFRAC(arg)			(HARDDOOM_CMD_TYPE_YFRAC << 26 | (arg))
#define HARDDOOM_CMD_XSTEP(arg)			(HARDDOOM_CMD_TYPE_XSTEP << 26 | (arg))
#define HARDDOOM_CMD_YSTEP(arg)			(HARDDOOM_CMD_TYPE_YSTEP << 26 | (arg))
#define HARDDOOM_CMD_COUNTER(arg)		(HARDDOOM_CMD_TYPE_COUNTER << 26 | (arg))

#define HARDDOOM_CMD_EXTR_TYPE_HI(cmd)		((cmd) >> 30 & 0x3)
#define HARDDOOM_CMD_EXTR_JUMP_ADDR(cmd)	((cmd) << 2 & 0xfffffffc)
#define HARDDOOM_CMD_EXTR_TYPE(cmd)		((cmd) >> 26 & 0x3f)
#define HARDDOOM_CMD_EXTR_RECT_WIDTH(cmd)	((cmd) & 0xfff)
#define HARDDOOM_CMD_EXTR_RECT_HEIGHT(cmd)	((cmd) >> 12 & 0xfff)
#define HARDDOOM_CMD_EXTR_COLUMN_OFFSET(cmd)	((cmd) & 0x3fffff)
#define HARDDOOM_CMD_EXTR_MELT_X(cmd)		((cmd) & 0x7ff)
#define HARDDOOM_CMD_EXTR_MELT_Y_OFFSET(cmd)	((cmd) >> 12 & 0xfff)
#define HARDDOOM_CMD_EXTR_PT(cmd)		(((cmd) & 0x3ffffff) << 6)
#define HARDDOOM_CMD_EXTR_SURF_DIMS_WIDTH(cmd)	(((cmd) & 0x3f) << 6)
#define HARDDOOM_CMD_EXTR_SURF_DIMS_HEIGHT(cmd)	(((cmd) & 0xfff00) >> 8)
#define HARDDOOM_CMD_EXTR_TEXTURE_SIZE(cmd)	(((cmd) & 0x3fffff) + 1)
#define HARDDOOM_CMD_EXTR_FLAT_ADDR(cmd)	(((cmd) & 0xfffff) << 12)
#define HARDDOOM_CMD_EXTR_COLORMAP_ADDR(cmd)	(((cmd) & 0xffffff) << 8)
#define HARDDOOM_CMD_EXTR_FILL_COLOR(cmd)	((cmd) & 0xff)
#define HARDDOOM_CMD_EXTR_CENTERY(cmd)		((cmd) >> 4 & 0x7ff)
#define HARDDOOM_CMD_EXTR_TEXTURE_HEIGHT(cmd)	((cmd) >> 16 & 0x3ff)
#define HARDDOOM_CMD_EXTR_XY_X(cmd)		((cmd) & 0x7ff)
#define HARDDOOM_CMD_EXTR_XY_Y(cmd)		((cmd) >> 12 & 0x7ff)
#define HARDDOOM_CMD_EXTR_TEX_COORD(cmd)	((cmd) & 0x3ffffff)
#define HARDDOOM_CMD_EXTR_FLAT_COORD(cmd)	((cmd) & 0x3fffff)
#define HARDDOOM_CMD_EXTR_COUNTER(cmd)		((cmd) & 0x3ffffff)

/* Page tables */

#define HARDDOOM_PTE_VALID		0x00000001
#define HARDDOOM_PTE_PHYS_MASK		0xfffff000
#define HARDDOOM_PAGE_SHIFT		12
#define HARDDOOM_PAGE_SIZE		0x1000

#endif
