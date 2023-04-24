/*
 *
 * This work is licensed under the terms of the GNU GPL, version 2.  See
 * the COPYING file in the top-level directory.
 */

#include <stdlib.h>
#include <stdint.h>


#include "dicedev.h"
#include "qemu/osdep.h"
#include "qemu/main-loop.h"
#include "hw/hw.h"
#include "hw/pci/pci.h"

#define TYPE_DICEDEV "dicedev"

#define DICEDEV_DEV(obj) \
	OBJECT_CHECK(DiceDevState, (obj), TYPE_DICEDEV)

#define CMDS_BUFFER_SIZE 255
#define MAX_PIPS 32
#define MAX_DICE_BUFFER_SIZE 100

struct buffer_slot
{
	uint64_t allowed;

	struct dice dice_buffer[MAX_DICE_BUFFER_SIZE];
	uint32_t dice_buffer_pos;

	uint32_t * result_buffer_page_table;
	uint32_t result_buffer_pos;

	uint32_t seed;
	bool enabled;
};

static void new_set(struct buffer_slot * slot, bool increment_seed)
{
	if (increment_seed)
	{
		slot->seed += 1;
	}
	srand(slot->seed);
	
	for (int i = 0; i < MAX_DICE_BUFFER_SIZE; ++i)
	{
		slot->dice_buffer[i].value = rand();
		slot->dice_buffer[i].type = (uint32_t) -1;
	}
	
	slot->dice_buffer_pos = 0;
}

static struct dice * get_dice(struct buffer_slot * slot, uint64_t dtype, enum output_type otype, bool increment_seed)
{
	if (slot->dice_buffer_pos == MAX_DICE_BUFFER_SIZE)
	{
		new_set(slot, increment_seed);
	}

	slot->dice_buffer_pos += 1;
	struct dice * dice = &slot->dice_buffer[slot->dice_buffer_pos];
	dice->type = dtype;	
	dice->value = dice->value % (dice->type + 1);

	if (otype == SNAKE_EYES)
	{
		dice->value = 0;	
	}
	else if (otype == CHEAT_DICE)
	{
		if (dice->value < (dice->type + 1) / 2)
		{
			dice->value += (dice->type + 1) / 2;
		}
	}

	return dice;
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
	enum processing_state proc_state;
	uint32_t proc_regs[4];
	uint32_t last_allowed;
	/* Slots */
	struct buffer_slot slots[16];
} DiceDevState;

static void dicedev_status_update(DiceDevState *d)
{
	pci_set_irq(PCI_DEVICE(d), !!(d->intr & d->intr_enable));
}

static void cmds_insert(uint32_t val, DiceDevState *d) {
	if ((d->cmds_end + 1) % CMDS_BUFFER_SIZE == d->cmds_start)
	{
		d->intr |= DICEDEV_INTR_FEED_ERROR;
	}
	else
	{
		d->cmds[d->cmds_end] = val;
		d->cmds_end = (d->cmds_end + 1) % CMDS_BUFFER_SIZE;
	}
}

static uint32_t cmds_get_one(DiceDevState *d)
{
	return d->cmds[d->cmds_start];
}

static void cmds_move_one(DiceDevState *d)
{
	if (d->cmds_start == d->cmds_end)
	{
		d->intr |= DICEDEV_INTR_FEED_ERROR;
	}
	else
	{
		d->cmds_start = (d->cmds_start + 1) % CMDS_BUFFER_SIZE;
	}
}


static uint32_t cmds_to_process(DiceDevState *d) {
	if(d->cmds_end >= d->cmds_start)
	{
		return d->cmds_end - d->cmds_start;
	}
	return CMDS_BUFFER_SIZE + d->cmds_end - d->cmds_start;
}

static uint32_t dicedev_cmd_manual_free(DiceDevState *d) {
	uint32_t free = CMDS_BUFFER_SIZE - cmds_to_process(d);
	return free;
}

static uint64_t dicedev_mmio_read(void *opaque, hwaddr addr, unsigned size) {
	uint64_t res = ~0ULL;
	DiceDevState *d = opaque;
	qemu_mutex_lock(&d->mutex);
	if (size == 4 && addr == DICEDEV_INTR) {
		res = d->intr;
	} else if (size == 4 && addr == DICEDEV_INTR_ENABLE) {
		res = d->intr_enable;
	} else if (size == 4 && addr == DICEDEV_ENABLE) {
		res = d->enable;
	} else if (size == 4 && addr == DICEDEV_INCREMENT_SEED) {
		res = d->increment_seed;
	} else if (size == 4 && addr == DICEDEV_CMD_FENCE_LAST) {
		res = d->cmd_fence_last;
	} else if (size == 4 && addr == DICEDEV_CMD_FENCE_WAIT) {
		res = d->cmd_fence_wait;
	} else if (size == 4 && addr == CMD_MANUAL_FREE) {
		res = dicedev_cmd_manual_free(d);
	} else {
		d->intr |= DICEDEV_INTR_MEM_ERROR;
	}

	dicedev_status_update(d);
	qemu_cond_signal(&d->cond);
	qemu_mutex_unlock(&d->mutex);
	return res;
}

static void dicedev_cmd_manual_feed(DiceDevState *d, uint32_t val) {
	cmds_insert(val, d);
}

static void dicedev_mmio_write(void *opaque, hwaddr addr, uint64_t val,
                unsigned size)
{
	DiceDevState *d = opaque;
	qemu_mutex_lock(&d->mutex);
	if (size == 4 && addr == DICEDEV_INTR) {
		d->intr &= ~val;
	} else if (size == 4 && addr == DICEDEV_INTR_ENABLE) {
		d->intr_enable = val;
	} else if (size == 4 && addr == DICEDEV_ENABLE) {
		d->enable = val;
	} else if (size == 4 && addr == DICEDEV_INCREMENT_SEED) {
		d->increment_seed = val;
	} else if (size == 4 && addr == DICEDEV_PROC_STATE) {
		d->proc_state = val;
	} else if (size == 4 && addr == DICEDEV_CMD_FENCE_LAST) {
		d->cmd_fence_last = val;
	} else if (size == 4 && addr == DICEDEV_CMD_FENCE_WAIT) {
		d->cmd_fence_wait = val;
	} else if (size == 4 && addr == CMD_MANUAL_FEED) {
		dicedev_cmd_manual_feed(d, val);
	} else {
		d->intr |= DICEDEV_INTR_MEM_ERROR;
	}

	dicedev_status_update(d);
	qemu_cond_signal(&d->cond);
	qemu_mutex_unlock(&d->mutex);
}

static const MemoryRegionOps dicedev_mmio_ops = {
	.read = dicedev_mmio_read,
	.write = dicedev_mmio_write,
	.endianness = DEVICE_LITTLE_ENDIAN,
};

/* Power-up reset of the device.  */
static void dicedev_power_reset(DeviceState *ds)
{
	DiceDevState *d = DICEDEV_DEV(ds);
	qemu_mutex_lock(&d->mutex);
	d->intr = mrand48() & 1;
	d->intr_enable = 0;
	d->enable = 0;
	d->increment_seed = 0;
	qemu_mutex_unlock(&d->mutex);
}

static bool is_in_set(uint32_t val, uint32_t set)
{
	if (val >= MAX_PIPS)
	{
		return false;
    	}

	uint64_t mask = 1UL << val;
	return (set & mask) != 0;
}

static void run_one_cmd(DiceDevState *d)
{
	uint32_t one_cmd = cmds_get_one(d);
	if (d->proc_state == NONE)
	{
		if ((one_cmd & 0xF) == DICEDEV_USER_CMD_TYPE_NOP)
		{
			// NOP
		}
		else if ((one_cmd & 0xF) == DICEDEV_USER_CMD_TYPE_BIND_SLOT)
		{
			d->proc_regs[0] = one_cmd;
			d->proc_state = BIND_SLOT_0;
		}
		else if ((one_cmd & 0xF) == DICEDEV_USER_CMD_TYPE_GET_DIE)
		{
			d->proc_regs[0] = one_cmd;
			d->proc_state = GET_DIE_0;
		}
		else if ((one_cmd & 0xF) == DICEDEV_USER_CMD_TYPE_FENCE)
		{
			d->cmd_fence_last = one_cmd >> 4;
			if (d->cmd_fence_wait == d->cmd_fence_last) {
				d->intr |= DICEDEV_INTR_FENCE_WAIT;
			}
		}
		else if ((one_cmd & 0xF) == DICEDEV_USER_CMD_NEW_SET)
		{
			uint32_t slot_id = (one_cmd >> 4) & 0xFF;
			new_set(&d->slots[slot_id], d->increment_seed);
		}
		else if ((one_cmd & 0xF) == DICEDEV_USER_CMD_TYPE_UNBIND_SLOT)
		{
			uint32_t slot_id = (one_cmd >> 4) & 0xFF;
			d->slots[slot_id].enabled = 0;
		}
		else {
			d->intr |= DICEDEV_INTR_CMD_ERROR;
		}
	}
	else if (d->proc_state == BIND_SLOT_0)
	{
		d->proc_regs[1] = one_cmd; //allowed
		d->proc_state = BIND_SLOT_1;
	}
	else if (d->proc_state == BIND_SLOT_1)
	{
		d->proc_regs[2] = one_cmd;
		d->proc_state = BIND_SLOT_2;
	}
	else if (d->proc_state == BIND_SLOT_2)
	{
		d->proc_regs[3] = one_cmd;
		
		uint32_t slot_id = (d->proc_regs[0] >> 4) & (0xF);
		struct buffer_slot * slot = &(d->slots[slot_id]);
		if (!slot->enabled)
		{
			slot->enabled = 1;
			slot->allowed = d->proc_regs[1];
			slot->dice_buffer_pos = MAX_DICE_BUFFER_SIZE;

			slot->result_buffer_page_table = (uint32_t *) (d->proc_regs[2] | (((dma_addr_t) d->proc_regs[3]) << 32));
			slot->result_buffer_pos = 0;

			slot->seed = (d->proc_regs[0] >> 12) & (0xFFFF);
		}
		else
		{
			d->intr |= DICEDEV_INTR_SLOT_ERROR;
		}

		d->proc_state = NONE;
	}
	else if (d->proc_state == GET_DIE_0)
	{
		d->proc_regs[1] = one_cmd;

		uint32_t i, j;
		uint32_t slot = (d->proc_regs[0] >> 24) & 0xFF;
		uint32_t * page_table = d->slots[slot].result_buffer_page_table;
		enum output_type dice_type = (d->proc_regs[0] >> 20) & 0xF;
		uint32_t num_die = (d->proc_regs[0] >> 4) & 0xFFFF;

		for (j = 0; j < MAX_PIPS; ++j)
		{
			if (!is_in_set(j, d->proc_regs[1]))
			{
				continue;
			}

			if (!is_in_set(j, d->slots[slot].allowed))
			{
				d->intr |= DICEDEV_INTR_CMD_ERROR;
			}

			for (i = 0; i < num_die; ++i)
			{
				struct dice * c = get_dice(&d->slots[slot], j, dice_type, d->increment_seed);
				uint32_t page_num = (d->slots[slot].result_buffer_pos * sizeof (struct dice)) / DICEDEV_PAGE_SIZE;
				
				dma_addr_t page_addr = 0;
				pci_dma_read(&d->dev, (dma_addr_t) (page_table + page_num), &page_addr, sizeof(uint32_t));
				if ((page_addr & 0xF) == 0x1)
				{
					page_addr = (page_addr & 0xfffffff0) << 8;
					struct dice * addr = ((struct dice *) page_addr) + d->slots[slot].result_buffer_pos % (DICEDEV_PAGE_SIZE / sizeof (struct dice));
					pci_dma_write(&d->dev, (dma_addr_t) addr, c, sizeof(struct dice));
					d->slots[slot].result_buffer_pos += 1;
				}
				else
				{
					d->intr |= DICEDEV_INTR_MEM_ERROR;
				}
			}
		}

		d->proc_state = NONE;
	}
	cmds_move_one(d);
}

static void *dicedev_thread(void *opaque)
{
	DiceDevState *d = opaque;
	qemu_mutex_lock(&d->mutex);
	while (1) {
		if (d->stopping)
			break;
		qemu_mutex_unlock(&d->mutex);
		qemu_mutex_lock_iothread();
		qemu_mutex_lock(&d->mutex);
		bool idle = (0 == cmds_to_process(d)) || (!d->enable);

		if (!idle) {
			run_one_cmd(d);
		}

		dicedev_status_update(d);
		qemu_mutex_unlock_iothread();
		if (idle) {
			qemu_cond_wait(&d->cond, &d->mutex);
		}
	}
	qemu_mutex_unlock(&d->mutex);
	return 0;
}

static void dicedev_realize(PCIDevice *pci_dev, Error **errp)
{
	DiceDevState *d = DICEDEV_DEV(pci_dev);
	uint8_t *pci_conf = d->dev.config;

	pci_config_set_interrupt_pin(pci_conf, 1);

	memory_region_init_io(&d->mmio, OBJECT(d), &dicedev_mmio_ops, d, "dicedev", DICEDEV_BAR_SIZE);
	pci_register_bar(&d->dev, 0, PCI_BASE_ADDRESS_SPACE_MEMORY, &d->mmio);

	qemu_mutex_init(&d->mutex);
	qemu_cond_init(&d->cond);

	dicedev_power_reset(&pci_dev->qdev);

	d->enable = 0;
	d->increment_seed = 0;
	d->cmds_start = 0;
	d->cmds_end = 0;

	d->stopping = false;

	d->cmd_fence_last = 0;
	d->cmd_fence_wait = 0;

	qemu_thread_create(&d->thread, "dicedev", dicedev_thread,
			d, QEMU_THREAD_JOINABLE);
}

static void dicedev_exit(PCIDevice *pci_dev)
{
	DiceDevState *d = DICEDEV_DEV(pci_dev);
	qemu_mutex_lock(&d->mutex);
	d->stopping = true;
	qemu_mutex_unlock(&d->mutex);
	qemu_cond_signal(&d->cond);
	qemu_thread_join(&d->thread);
	qemu_cond_destroy(&d->cond);
	qemu_mutex_destroy(&d->mutex);
}

static void dicedev_class_init(ObjectClass *klass, void *data)
{
	DeviceClass *dc = DEVICE_CLASS(klass);
	PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);

	k->realize = dicedev_realize;
	k->exit = dicedev_exit;
	k->vendor_id = DICEDEV_VENDOR_ID;
	k->device_id = DICEDEV_DEVICE_ID;
	k->class_id = PCI_CLASS_PROCESSOR_CO;
	dc->reset = dicedev_power_reset;
}

static InterfaceInfo dicedev_interfaces[] = {
	{ INTERFACE_CONVENTIONAL_PCI_DEVICE },
	{ },
};

static TypeInfo dicedev_info = {
	.name          = TYPE_DICEDEV,
	.parent        = TYPE_PCI_DEVICE,
	.instance_size = sizeof(DiceDevState),
	.class_init    = dicedev_class_init,
	.interfaces    = dicedev_interfaces,
};

static void dicedev_register_types(void)
{
	type_register_static(&dicedev_info);
}

type_init(dicedev_register_types)
