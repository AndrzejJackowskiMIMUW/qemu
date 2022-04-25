/*
 *
 * This work is licensed under the terms of the GNU GPL, version 2.  See
 * the COPYING file in the top-level directory.
 */

#include <stdlib.h>
#include <stdint.h>


#include "casinodev.h"
#include "qemu/osdep.h"
#include "qemu/main-loop.h"
#include "hw/hw.h"
#include "hw/pci/pci.h"

#define TYPE_CASINODEV "casinodev"

#define CASINODEV_DEV(obj) \
	OBJECT_CHECK(CasinoDevState, (obj), TYPE_CASINODEV)

#define CMDS_BUFFER_SIZE 255
#define MAX_CARDS 52

struct BufferSlot
{
	enum deck_type type;

	struct card cards_buffer[MAX_CARDS];
	uint32_t cards_buffer_pos;

	uint32_t * result_buffer_page_table;
	uint32_t result_buffer_pos;

	uint32_t seed;
	bool enabled;
};

static void init_deck(struct card cards[])
{
	enum card_suit suit;
	enum card_rank rank;
	int32_t count = 0;
	for (suit = CLUB; suit <= SPADE; ++suit)
	{
		for (rank = TWO; rank <= ACE; ++rank)
		{
			cards[count].suit = suit;
			cards[count].rank = rank;
			count += 1;
		}
	}
}

static void swap_cards(struct card * c1, struct card *c2)
{
	struct card tmp = *c1;
	*c1 = *c2;
	*c2 = tmp;
}

static void shuffle(struct card cards[])
{
	int i;
	for (i = 0; i < 1000; ++i)
	{
		swap_cards(&cards[rand() % MAX_CARDS], &cards[rand() % MAX_CARDS]);
	}
}

static void new_deck(struct BufferSlot * slot, uint32_t increment_seed)
{
	init_deck(slot->cards_buffer);
	if (increment_seed)
	{
		slot->seed += 1;
	}
	srand(slot->seed);
	shuffle(slot->cards_buffer);
	slot->cards_buffer_pos = 0;
}

static struct card * get_card(struct BufferSlot * slot, uint32_t get_cards_type, uint32_t increment_seed)
{
	if (slot->cards_buffer_pos == MAX_CARDS)
	{
		new_deck(slot, increment_seed);
	}

	// MAGIC TRICK
	if (get_cards_type > 0)
	{
		uint32_t i;
		uint32_t max_rank = 0;
		uint32_t max_rank_pos = -1;
		uint32_t min_rank = 9999;
		uint32_t min_rank_pos = -1;

		for(i = slot->cards_buffer_pos; i < MAX_CARDS; ++i)
		{
			int rank = slot->cards_buffer[i].rank;
			if (rank > max_rank)
			{
				max_rank = rank;
				max_rank_pos = i;
			}
			if (rank < min_rank)
			{
				min_rank = rank;
				min_rank_pos = i;
			}
		}
		if (get_cards_type == CASINODEV_USER_OUTPUT_GOOD)
		{
			swap_cards(&slot->cards_buffer[slot->cards_buffer_pos], &slot->cards_buffer[max_rank_pos]);
		}
		else
		{
			swap_cards(&slot->cards_buffer[slot->cards_buffer_pos], &slot->cards_buffer[min_rank_pos]);
		}
	}

	struct card * ret = &slot->cards_buffer[slot->cards_buffer_pos];
	slot->cards_buffer_pos += 1;

	if (slot->type == NINE_PLUS && ret->rank < NINE)
	{
		return get_card(slot, get_cards_type, increment_seed);
	}
	return ret;
}

typedef struct {
	PCIDevice dev;
	MemoryRegion mmio;
	QemuThread thread;
	QemuMutex mutex;
	QemuCond cond;
	bool stopping;
	bool enable;
	uint32_t cmd_fence_last;
	uint32_t cmd_fence_wait;
	/* Registers.  */
	uint32_t intr;
	uint32_t intr_enable;
	uint32_t increment_seed;
	/* Commands */
	uint32_t cmds[CMDS_BUFFER_SIZE];
	uint32_t cmds_start;
	uint32_t cmds_end;
	/* Processing */
	enum ProcessingState proc_state;
	uint32_t proc_regs[3];
	/* Slots */
	struct BufferSlot slots[16];
} CasinoDevState;

static void casinodev_status_update(CasinoDevState *d)
{
	//fprintf(stderr, "casinodev_status_update %d %d\n", d->intr, d->intr_enable);
	pci_set_irq(PCI_DEVICE(d), !!(d->intr & d->intr_enable));
}

static void cmds_insert(uint32_t val, CasinoDevState *d) {
	if ((d->cmds_end + 1) % CMDS_BUFFER_SIZE == d->cmds_start)
	{
		d->intr |= CASINODEV_INTR_FEED_ERROR;
	}
	else
	{
		d->cmds[d->cmds_end] = val;
		d->cmds_end = (d->cmds_end + 1) % CMDS_BUFFER_SIZE;
	}
}

static uint32_t cmds_get_one(CasinoDevState *d)
{
	return d->cmds[d->cmds_start];
}

static void cmds_move_one(CasinoDevState *d)
{
	if (d->cmds_start == d->cmds_end)
	{
		d->intr |= CASINODEV_INTR_FEED_ERROR;
	}
	else
	{
		d->cmds_start = (d->cmds_start + 1) % CMDS_BUFFER_SIZE;
	}
}


static uint32_t cmds_to_process(CasinoDevState *d) {
	if(d->cmds_end >= d->cmds_start)
	{
		return d->cmds_end - d->cmds_start;
	}
	return CMDS_BUFFER_SIZE + d->cmds_end - d->cmds_start;
}

static uint32_t casinodev_cmd_manual_free(CasinoDevState *d) {
	uint32_t free = CMDS_BUFFER_SIZE - cmds_to_process(d);
	return free;
}

static uint64_t casinodev_mmio_read(void *opaque, hwaddr addr, unsigned size) {
	//fprintf(stderr, "MMIO READ %ld %d\n", addr, size);
	uint64_t res = ~0ULL;
	CasinoDevState *d = opaque;
	qemu_mutex_lock(&d->mutex);
	if (size == 4 && addr == CASINODEV_INTR) {
		res = d->intr;
	} else if (size == 4 && addr == CASINODEV_INTR_ENABLE) {
		res = d->intr_enable;
	} else if (size == 4 && addr == CASINODEV_ENABLE) {
		res = d->enable;
	} else if (size == 4 && addr == CASINODEV_INCREMENT_SEED) {
		res = d->increment_seed;
	} else if (size == 4 && addr == CASINODEV_CMD_FENCE_LAST) {
		res = d->cmd_fence_last;
	} else if (size == 4 && addr == CASINODEV_CMD_FENCE_WAIT) {
		res = d->cmd_fence_wait;
	} else if (size == 4 && addr == CMD_MANUAL_FREE) {
		res = casinodev_cmd_manual_free(d);
	} else {
		d->intr |= CASINODEV_INTR_MEM_ERROR;
	}

	casinodev_status_update(d);
	qemu_cond_signal(&d->cond);
	qemu_mutex_unlock(&d->mutex);
	return res;
}

static void casinodev_cmd_manual_feed(CasinoDevState *d, uint32_t val) {
	cmds_insert(val, d);
}

static void casinodev_mmio_write(void *opaque, hwaddr addr, uint64_t val,
                unsigned size)
{
	//fprintf(stderr, "MMIO WRITE %ld %ld %d \n", addr, val, size);
	CasinoDevState *d = opaque;
	qemu_mutex_lock(&d->mutex);
	if (size == 4 && addr == CASINODEV_INTR) {
		d->intr &= ~val;
	} else if (size == 4 && addr == CASINODEV_INTR_ENABLE) {
		d->intr_enable = val;
	} else if (size == 4 && addr == CASINODEV_ENABLE) {
		d->enable = val;
	} else if (size == 4 && addr == CASINODEV_INCREMENT_SEED) {
		d->increment_seed = val;
	} else if (size == 4 && addr == CASINODEV_PROC_STATE) {
		d->proc_state = val;
	} else if (size == 4 && addr == CASINODEV_CMD_FENCE_LAST) {
		d->cmd_fence_last = val;
	} else if (size == 4 && addr == CASINODEV_CMD_FENCE_WAIT) {
		d->cmd_fence_wait = val;
	} else if (size == 4 && addr == CMD_MANUAL_FEED) {
		casinodev_cmd_manual_feed(d, val);
	} else {
		d->intr |= CASINODEV_INTR_MEM_ERROR;
	}

	casinodev_status_update(d);
	qemu_cond_signal(&d->cond);
	qemu_mutex_unlock(&d->mutex);
}

static const MemoryRegionOps casinodev_mmio_ops = {
	.read = casinodev_mmio_read,
	.write = casinodev_mmio_write,
	.endianness = DEVICE_LITTLE_ENDIAN,
};

/* Power-up reset of the device.  */
static void casinodev_power_reset(DeviceState *ds)
{
	CasinoDevState *d = CASINODEV_DEV(ds);
	qemu_mutex_lock(&d->mutex);
	d->intr = mrand48() & 1;
	d->intr_enable = 0;
	d->enable = 0;
	d->increment_seed = 0;
	qemu_mutex_unlock(&d->mutex);
}

static void run_one_cmd(CasinoDevState *d)
{
	uint32_t one_cmd = cmds_get_one(d);
	if (d->proc_state == NONE)
	{
		if ((one_cmd & 0xF) == CASINODEV_USER_CMD_TYPE_NOP)
		{
			// NOP
		}
		else if ((one_cmd & 0xF) == CASINODEV_USER_CMD_TYPE_BIND_SLOT)
		{
			d->proc_regs[0] = one_cmd;
			d->proc_state = BIND_SLOT_0;
		}
		else if ((one_cmd & 0xF) == CASINODEV_USER_CMD_TYPE_GET_CARDS)
		{
			uint32_t i;
			uint32_t slot = (one_cmd >> 24) & 0xFF;
			uint32_t * page_table = d->slots[slot].result_buffer_page_table;
			uint32_t get_cards_type = (one_cmd >> 20) & 0xF;

			for (i = 0; i < ((one_cmd >> 4) & 0xFFFF); ++i)
			{
				struct card * c = get_card(&d->slots[slot], get_cards_type, d->increment_seed);
				uint32_t page_num = (d->slots[slot].result_buffer_pos * sizeof (struct card)) / CASINODEV_PAGE_SIZE;

				dma_addr_t page_addr = 0;
				pci_dma_read(&d->dev, (dma_addr_t) (page_table + page_num), &page_addr, sizeof(uint32_t));
				if ((page_addr & 0xF) == 0x1)
				{
					page_addr = (page_addr & 0xfffffff0) << 8;
					struct card * addr = ((struct card *) page_addr) + d->slots[slot].result_buffer_pos % (CASINODEV_PAGE_SIZE / sizeof (struct card));
					pci_dma_write(&d->dev, (dma_addr_t) addr, c, sizeof(struct card));
					d->slots[slot].result_buffer_pos += 1;
				}
				else
				{
					d->intr |= CASINODEV_INTR_MEM_ERROR;
				}
			}
		}
		else if ((one_cmd & 0xF) == CASINODEV_USER_CMD_TYPE_FENCE)
		{
			d->cmd_fence_last = one_cmd >> 4;
			if (d->cmd_fence_wait == d->cmd_fence_last) {
				d->intr |= CASINODEV_INTR_FENCE_WAIT;
			}
		}
		else if ((one_cmd & 0xF) == CASINODEV_USER_CMD_NEW_DECK)
		{
			uint32_t slot_id = (one_cmd >> 4) & 0xFF;
			new_deck(&d->slots[slot_id], d->increment_seed);
		}
		else if ((one_cmd & 0xF) == CASINODEV_USER_CMD_TYPE_UNBIND_SLOT)
		{
			uint32_t slot_id = (one_cmd >> 4) & 0xFF;
			d->slots[slot_id].enabled = 0;
		}
		else {
			//fprintf(stderr, "CASINODEV_INTR_CMD_ERROR CMD, cmd %d \n", one_cmd);
			d->intr |= CASINODEV_INTR_CMD_ERROR;
		}
	}
	else if (d->proc_state == BIND_SLOT_0)
	{
		d->proc_regs[1] = one_cmd;
		d->proc_state = BIND_SLOT_1;
	}
	else if (d->proc_state == BIND_SLOT_1)
	{
		d->proc_regs[2] = one_cmd;

		uint32_t slot_id = (d->proc_regs[0] >> 4) & (0xF);
		struct BufferSlot * slot = &(d->slots[slot_id]);
		if (!slot->enabled)
		{
			slot->enabled = 1;
			slot->type = d->proc_regs[0] >> 28;
			slot->cards_buffer_pos = MAX_CARDS;

			slot->result_buffer_page_table = (uint32_t *) (d->proc_regs[1] | (((dma_addr_t) d->proc_regs[2]) << 32));
			slot->result_buffer_pos = 0;

			slot->seed = (d->proc_regs[0] >> 12) & (0xFFFF);
		}
		else
		{
			d->intr |= CASINODEV_INTR_SLOT_ERROR;
		}

		d->proc_state = NONE;
	}
	cmds_move_one(d);
	//fprintf(stderr, "FINISHED CMD, state %d \n",  d->proc_state);
}

static void *casinodev_thread(void *opaque)
{
	CasinoDevState *d = opaque;
	qemu_mutex_lock(&d->mutex);
	while (1) {
		if (d->stopping)
			break;
		qemu_mutex_unlock(&d->mutex);
		qemu_mutex_lock_iothread();
		qemu_mutex_lock(&d->mutex);
		bool idle = (0 == cmds_to_process(d)) || (!d->enable);

		if (!idle) {
			//fprintf(stderr, "not idle %d %d %d\n", cmds_to_process(d), d->cmds_end, d->cmds_start);

			run_one_cmd(d);
		}

		casinodev_status_update(d);
		qemu_mutex_unlock_iothread();
		if (idle) {
			qemu_cond_wait(&d->cond, &d->mutex);
		}
	}
	qemu_mutex_unlock(&d->mutex);
	return 0;
}

static void casinodev_realize(PCIDevice *pci_dev, Error **errp)
{
	CasinoDevState *d = CASINODEV_DEV(pci_dev);
	uint8_t *pci_conf = d->dev.config;

	pci_config_set_interrupt_pin(pci_conf, 1);

	memory_region_init_io(&d->mmio, OBJECT(d), &casinodev_mmio_ops, d, "casinodev", CASINODEV_BAR_SIZE);
	pci_register_bar(&d->dev, 0, PCI_BASE_ADDRESS_SPACE_MEMORY, &d->mmio);

	qemu_mutex_init(&d->mutex);
	qemu_cond_init(&d->cond);

	casinodev_power_reset(&pci_dev->qdev);

	d->enable = 0;
	d->increment_seed = 0;
	d->cmds_start = 0;
	d->cmds_end = 0;

	d->stopping = false;

	d->cmd_fence_last = 0;
	d->cmd_fence_wait = 0;

	qemu_thread_create(&d->thread, "casinodev", casinodev_thread,
			d, QEMU_THREAD_JOINABLE);
}

static void casinodev_exit(PCIDevice *pci_dev)
{
	CasinoDevState *d = CASINODEV_DEV(pci_dev);
	qemu_mutex_lock(&d->mutex);
	d->stopping = true;
	qemu_mutex_unlock(&d->mutex);
	qemu_cond_signal(&d->cond);
	qemu_thread_join(&d->thread);
	qemu_cond_destroy(&d->cond);
	qemu_mutex_destroy(&d->mutex);
}

static void casinodev_class_init(ObjectClass *klass, void *data)
{
	DeviceClass *dc = DEVICE_CLASS(klass);
	PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);

	k->realize = casinodev_realize;
	k->exit = casinodev_exit;
	k->vendor_id = CASINODEV_VENDOR_ID;
	k->device_id = CASINODEV_DEVICE_ID;
	k->class_id = PCI_CLASS_PROCESSOR_CO;
	dc->reset = casinodev_power_reset;
}

static InterfaceInfo casinodev_interfaces[] = {
	{ INTERFACE_CONVENTIONAL_PCI_DEVICE },
	{ },
};

static TypeInfo casinodev_info = {
	.name          = TYPE_CASINODEV,
	.parent        = TYPE_PCI_DEVICE,
	.instance_size = sizeof(CasinoDevState),
	.class_init    = casinodev_class_init,
	.interfaces    = casinodev_interfaces,
};

static void casinodev_register_types(void)
{
	type_register_static(&casinodev_info);
}

type_init(casinodev_register_types)
