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
/* The reset register.  Punching 1 will clear all pending work.  There is
 * no reset for FETCH_CMD (initialize CMD_*_PTR instead).  */
#define HARDDOOM_RESET				0x004
#define HARDDOOM_RESET_DRAW			0x00000001
#define HARDDOOM_RESET_FIFO			0x00000002
#define HARDDOOM_RESET_TLB			0x00000008
#define HARDDOOM_RESET_CACHE			0x00000010
/* Interrupt status.  */
#define HARDDOOM_INTR				0x008
#define HARDDOOM_INTR_SYNC			0x00000001
#define HARDDOOM_INTR_INVALID_CMD		0x00000002
#define HARDDOOM_INTR_FIFO_OVERFLOW		0x00000004
#define HARDDOOM_INTR_SURF_OVERFLOW		0x00000008
#define HARDDOOM_INTR_PAGE_FAULT_SURF_DST	0x00000010
#define HARDDOOM_INTR_PAGE_FAULT_SURF_SRC	0x00000020
#define HARDDOOM_INTR_PAGE_FAULT_TEXTURE	0x00000040
#define HARDDOOM_INTR_MASK			0x0000007f
/* And enable (same bitfields).  */
#define HARDDOOM_INTR_ENABLE			0x00c
/* The last value of processed SYNC command.  */
#define HARDDOOM_SYNC_LAST			0x010
#define HARDDOOM_SYNC_MASK			0x03ffffff
/* The value that will trigger a SYNC interrupt when used in SYNC command.  */
#define HARDDOOM_SYNC_INTR			0x014
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
 * There are 0x200 entries, indexed by 10-bit indices (each entry is visible
 * under two indices).  Bits 0-9 is read pointer (index of the next entry to
 * be read by DRAW), 16-25 is write pointer (index of the next entry to be
 * written by FIFO_SEND).  FIFO is empty iff read == write, full iff read ==
 * write ^ 0x200.  Situations where ((write - read) & 0x3ff) > 0x200
 * are illegal and won't be reached in proper operation of the device.
 */
#define HARDDOOM_FIFO_STATE			0x028
#define HARDDOOM_FIFO_STATE_READ(st)		((st) & 0x3ff)
#define HARDDOOM_FIFO_STATE_WRITE(st)		((st) >> 16 & 0x3ff)
#define HARDDOOM_FIFO_STATE_MASK		0x03ff03ff
#define HARDDOOM_FIFO_PTR_MASK			0x000003ff

/* Internal DRAW state registers -- these store the last sent value for
 * the corresponding command.  */
#define HARDDOOM_STATE_SURF_DST_PT		0x080
#define HARDDOOM_STATE_SURF_SRC_PT		0x084
#define HARDDOOM_STATE_TEXTURE_PT		0x088
#define HARDDOOM_STATE_FLAT_ADDR		0x08c
#define HARDDOOM_STATE_COLORMAP_ADDR		0x090
#define HARDDOOM_STATE_TRANSLATION_ADDR		0x094
#define HARDDOOM_STATE_SURF_DIMS		0x098
#define HARDDOOM_STATE_TEXTURE_DIMS		0x09c
#define HARDDOOM_STATE_FILL_COLOR		0x0a0
#define HARDDOOM_STATE_DRAW_PARAMS		0x0a4
#define HARDDOOM_STATE_XY_A			0x0a8
#define HARDDOOM_STATE_XY_B			0x0ac
#define HARDDOOM_STATE_USTART			0x0b0
#define HARDDOOM_STATE_VSTART			0x0b4
#define HARDDOOM_STATE_USTEP			0x0b8
#define HARDDOOM_STATE_VSTEP			0x0bc

/* Internal DRAW trigger registers -- writing any of these will start the
 * given operation *now*, skipping the FIFO.  If some operation is already
 * in progress, the results are going to be unpredictable.  */
#define HARDDOOM_TRIGGER_COPY_RECT		0x0c0
#define HARDDOOM_TRIGGER_FILL_RECT		0x0c4
#define HARDDOOM_TRIGGER_DRAW_LINE		0x0c8
#define HARDDOOM_TRIGGER_DRAW_BACKGROUND	0x0cc
#define HARDDOOM_TRIGGER_DRAW_SPAN		0x0d0
#define HARDDOOM_TRIGGER_DRAW_COLUMN		0x0d4

#define HARDDOOM_TRIGGER_INTERLOCK		0x0f8
#define HARDDOOM_TRIGGER_SYNC			0x0fc

/* The single-entry TLBs, one for each paged buffer.
 * Tags have bits 000007ff: bits 0-9 are virtual page index, bit 10
 * is valid bit.  The pte registers are simply contents of the cached
 * PTE.
 */
#define HARDDOOM_TLB_SURF_DST_TAG		0x100
#define HARDDOOM_TLB_SURF_DST_PTE		0x104
#define HARDDOOM_TLB_SURF_SRC_TAG		0x108
#define HARDDOOM_TLB_SURF_SRC_PTE		0x10c
#define HARDDOOM_TLB_TEXTURE_TAG		0x110
#define HARDDOOM_TLB_TEXTURE_PTE		0x114
#define HARDDOOM_TLB_TAG_IDX_MASK		0x000003ff
#define HARDDOOM_TLB_TAG_VALID			0x00000400

/* Internal state of the cache.  There are 4 caches (one for each cacheable
 * buffer).  Every cache has a single 0x100-byte line.  In the case of color
 * maps, this effectively caches the whole buffer.  */
#define HARDDOOM_CACHE_STATE			0x120
#define HARDDOOM_CACHE_STATE_TEXTURE_TAG_MASK	0x00003fff
#define HARDDOOM_CACHE_STATE_TEXTURE_TAG_SHIFT	0
#define HARDDOOM_CACHE_STATE_TEXTURE_VALID	0x00004000
#define HARDDOOM_CACHE_STATE_FLAT_TAG_MASK	0x000f0000
#define HARDDOOM_CACHE_STATE_FLAT_TAG_SHIFT	16
#define HARDDOOM_CACHE_STATE_FLAT_VALID		0x00100000
#define HARDDOOM_CACHE_STATE_COLORMAP_VALID	0x01000000
#define HARDDOOM_CACHE_STATE_TRANSLATION_VALID	0x10000000

/* Internal DRAW unit state (operation progress).  */
#define HARDDOOM_DRAW_X_CUR			0x140
#define HARDDOOM_DRAW_Y_CUR			0x144
#define HARDDOOM_DRAW_X_RESTART			0x148
#define HARDDOOM_DRAW_END			0x14c
#define HARDDOOM_DRAW_TEXCOORD_U		0x150
#define HARDDOOM_DRAW_TEXCOORD_V		0x154
#define HARDDOOM_DRAW_STATE			0x158
#define HARDDOOM_DRAW_STATE_MODE_MASK		0x00000007
#define HARDDOOM_DRAW_STATE_MODE_IDLE		0x00000000
#define HARDDOOM_DRAW_STATE_MODE_COPY		0x00000001
#define HARDDOOM_DRAW_STATE_MODE_FILL		0x00000002
#define HARDDOOM_DRAW_STATE_MODE_LINE		0x00000003
#define HARDDOOM_DRAW_STATE_MODE_BACKGROUND	0x00000004
#define HARDDOOM_DRAW_STATE_MODE_COLUMN		0x00000005
#define HARDDOOM_DRAW_STATE_MODE_SPAN		0x00000006
#define HARDDOOM_DRAW_STATE_MODE_FUZZ		0x00000007
#define HARDDOOM_DRAW_STATE_LINE_X_MAJOR	0x00000008
#define HARDDOOM_DRAW_STATE_LINE_SX_NEGATIVE	0x00000010
#define HARDDOOM_DRAW_STATE_LINE_SY_NEGATIVE	0x00000020
#define HARDDOOM_DRAW_STATE_FUZZPOS_SHIFT	8
#define HARDDOOM_DRAW_STATE_FUZZPOS_MASK	0x00003f00
#define HARDDOOM_DRAW_STATE_LINE_D_SHIFT	16
#define HARDDOOM_DRAW_STATE_LINE_D_MASK		0x1fff0000
#define HARDDOOM_DRAW_LINE_SIZE			0x15c
#define HARDDOOM_DRAW_COLUMN_OFFSET		0x160

/* The cache contents (internal).  */
#define HARDDOOM_CACHE_DATA_TEXTURE(x)		(0x400 + (x))
#define HARDDOOM_CACHE_DATA_FLAT(x)		(0x500 + (x))
#define HARDDOOM_CACHE_DATA_COLORMAP(x)		(0x600 + (x))
#define HARDDOOM_CACHE_DATA_TRANSLATION(x)	(0x700 + (x))
#define HARDDOOM_CACHE_SIZE			0x100
#define HARDDOOM_CACHE_SHIFT			8

/* The contents of the FIFO (internal).  */
#define HARDDOOM_FIFO_CMD(x)			(0x800 + (x) * 4)
#define HARDDOOM_FIFO_CMD_NUM			0x200


/* Commands */

/* Jump in the command buffer.  */
#define HARDDOOM_CMD_TYPE_HI_JUMP		0x0

/* Surface to render to, all commands. */
#define HARDDOOM_CMD_TYPE_SURF_DST_PT		0x20
/* Source surface for COPY_RECT,  */
#define HARDDOOM_CMD_TYPE_SURF_SRC_PT		0x21
/* Texture for DRAW_COLUMN. */
#define HARDDOOM_CMD_TYPE_TEXTURE_PT		0x22
/* The flat for DRAW_BACKGROUND and DRAW_SPAN. */
#define HARDDOOM_CMD_TYPE_FLAT_ADDR		0x23
/* Fade/effect color map for DRAW_COLUMN and DRAW_SPAN, used iff DRAW_PARAMS_COLORMAP set. */
#define HARDDOOM_CMD_TYPE_COLORMAP_ADDR		0x24
/* Palette translation color map for DRAW_COLUMN and DRAW_SPAN, used iff DRAW_PARAMS_TRANSLATE set. */
#define HARDDOOM_CMD_TYPE_TRANSLATION_ADDR	0x25
/* Dimensions of all SURFs in use. */
#define HARDDOOM_CMD_TYPE_SURF_DIMS		0x26
/* Height and byte size of the texture for DRAW_COLUMN. */
#define HARDDOOM_CMD_TYPE_TEXTURE_DIMS		0x27
/* Solid fill color for FILL_RECT and DRAW_LINE. */
#define HARDDOOM_CMD_TYPE_FILL_COLOR		0x28
/* Flags for DRAW_COLUMN and DRAW_SPAN (FUZZ, TRANSLATE, COLORMAP).
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
/* Tex coord start for DRAW_COLUMN (U), DRAW_SPAN (U+V). */
#define HARDDOOM_CMD_TYPE_USTART		0x2c
#define HARDDOOM_CMD_TYPE_VSTART		0x2d
/* Tex coord derivative for DRAW_COLUMN (U), DRAW_SPAN (U+V). */
#define HARDDOOM_CMD_TYPE_USTEP			0x2e
#define HARDDOOM_CMD_TYPE_VSTEP			0x2f

/* V_CopyRect: The usual blit.  Rectangle size passed directly.  */
#define HARDDOOM_CMD_TYPE_COPY_RECT		0x30
/* V_FillRect: The usual solid fill.  Rectangle size passed directly.  */
#define HARDDOOM_CMD_TYPE_FILL_RECT		0x31
/* V_DrawLine: The usual solid line. */
#define HARDDOOM_CMD_TYPE_DRAW_LINE		0x32
/* V_DrawBackground: Fill whole FB with repeated flat. */
#define HARDDOOM_CMD_TYPE_DRAW_BACKGROUND	0x33
/* R_DrawColumn. */
#define HARDDOOM_CMD_TYPE_DRAW_COLUMN		0x34
/* R_DrawSpan. */
#define HARDDOOM_CMD_TYPE_DRAW_SPAN		0x35

/* Block further surface reads until inflight surface writes are complete.  */
#define HARDDOOM_CMD_TYPE_INTERLOCK		0x3e
/* Set the sync counter.  */
#define HARDDOOM_CMD_TYPE_SYNC			0x3f

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
#define HARDDOOM_CMD_SURF_DST_PT(addr)		(HARDDOOM_CMD_TYPE_SURF_DST_PT << 26 | (addr) >> 6)
#define HARDDOOM_CMD_SURF_SRC_PT(addr)		(HARDDOOM_CMD_TYPE_SURF_SRC_PT << 26 | (addr) >> 6)
#define HARDDOOM_CMD_TEXTURE_PT(addr)		(HARDDOOM_CMD_TYPE_TEXTURE_PT << 26 | (addr) >> 6)
#define HARDDOOM_CMD_FLAT_ADDR(addr)		(HARDDOOM_CMD_TYPE_FLAT_ADDR << 26 | (addr) >> 12)
#define HARDDOOM_CMD_COLORMAP_ADDR(addr)	(HARDDOOM_CMD_TYPE_COLORMAP_ADDR << 26 | (addr) >> 8)
#define HARDDOOM_CMD_TRANSLATION_ADDR(addr)	(HARDDOOM_CMD_TYPE_TRANSLATION_ADDR << 26 | (addr) >> 8)
#define HARDDOOM_CMD_SURF_DIMS(w, h)		(HARDDOOM_CMD_TYPE_SURF_DIMS << 26 | (w) >> 6 | (h) << 8)
#define HARDDOOM_CMD_TEXTURE_DIMS(sz, h)	(HARDDOOM_CMD_TYPE_TEXTURE_DIMS << 26 | ((sz) - 1) >> 8 << 12 | (h))
#define HARDDOOM_CMD_FILL_COLOR(color)		(HARDDOOM_CMD_TYPE_FILL_COLOR << 26 | (color))
#define HARDDOOM_CMD_DRAW_PARAMS(flags	)	(HARDDOOM_CMD_TYPE_DRAW_PARAMS << 26 | (flags))
#define HARDDOOM_CMD_XY_A(x, y)			(HARDDOOM_CMD_TYPE_XY_A << 26 | (y) << 12 | (x))
#define HARDDOOM_CMD_XY_B(x, y)			(HARDDOOM_CMD_TYPE_XY_B << 26 | (y) << 12 | (x))
#define HARDDOOM_CMD_USTART(arg)		(HARDDOOM_CMD_TYPE_XSTART << 26 | (arg))
#define HARDDOOM_CMD_VSTART(arg)		(HARDDOOM_CMD_TYPE_YSTART << 26 | (arg))
#define HARDDOOM_CMD_USTEP(arg)			(HARDDOOM_CMD_TYPE_XSTEP << 26 | (arg))
#define HARDDOOM_CMD_VSTEP(arg)			(HARDDOOM_CMD_TYPE_YSTEP << 26 | (arg))
#define HARDDOOM_CMD_INTERLOCK			(HARDDOOM_CMD_TYPE_INTERLOCK << 26)
#define HARDDOOM_CMD_SYNC(arg)			(HARDDOOM_CMD_TYPE_SYNC << 26 | (arg))

#define HARDDOOM_CMD_EXTR_TYPE_HI(cmd)		((cmd) >> 30 & 0x3)
#define HARDDOOM_CMD_EXTR_JUMP_ADDR(cmd)	((cmd) << 2 & 0xfffffffc)
#define HARDDOOM_CMD_EXTR_TYPE(cmd)		((cmd) >> 26 & 0x3f)
#define HARDDOOM_CMD_EXTR_RECT_WIDTH(cmd)	((cmd) & 0xfff)
#define HARDDOOM_CMD_EXTR_RECT_HEIGHT(cmd)	((cmd) >> 12 & 0xfff)
#define HARDDOOM_CMD_EXTR_COLUMN_OFFSET(cmd)	((cmd) & 0x3fffff)
#define HARDDOOM_CMD_EXTR_PT(cmd)		(((cmd) & 0x3ffffff) << 6)
#define HARDDOOM_CMD_EXTR_SURF_DIMS_WIDTH(cmd)	(((cmd) & 0x3f) << 6)
#define HARDDOOM_CMD_EXTR_SURF_DIMS_HEIGHT(cmd)	(((cmd) & 0xfff00) >> 8)
#define HARDDOOM_CMD_EXTR_TEXTURE_SIZE(cmd)	((((cmd) >> 12 & 0x3fff) + 1) << 8)
#define HARDDOOM_CMD_EXTR_TEXTURE_HEIGHT(cmd)	((cmd) & 0x3ff)
#define HARDDOOM_CMD_EXTR_FLAT_ADDR(cmd)	(((cmd) & 0xfffff) << 12)
#define HARDDOOM_CMD_EXTR_COLORMAP_ADDR(cmd)	(((cmd) & 0xffffff) << 8)
#define HARDDOOM_CMD_EXTR_FILL_COLOR(cmd)	((cmd) & 0xff)
#define HARDDOOM_CMD_EXTR_XY_X(cmd)		((cmd) & 0x7ff)
#define HARDDOOM_CMD_EXTR_XY_Y(cmd)		((cmd) >> 12 & 0x7ff)
#define HARDDOOM_CMD_EXTR_TEX_COORD(cmd)	((cmd) & 0x3ffffff)
#define HARDDOOM_CMD_EXTR_SYNC(cmd)		((cmd) & 0x3ffffff)

/* Page tables */

#define HARDDOOM_PTE_VALID		0x00000001
#define HARDDOOM_PTE_PHYS_MASK		0xfffff000
#define HARDDOOM_PAGE_SHIFT		12
#define HARDDOOM_PAGE_SIZE		0x1000

/* Misc */

#define HARDDOOM_COORD_MASK		0x7ff

#endif
