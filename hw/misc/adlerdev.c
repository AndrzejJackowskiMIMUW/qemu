/*
 * The Adler32 device
 *
 * Copyright (C) 2013-2020 Marcelina Kościelnicka
 *
 * This work is licensed under the terms of the GNU GPL, version 2.  See
 * the COPYING file in the top-level directory.
 */

#include "adlerdev.h"
#include "qemu/osdep.h"
#include "qemu/main-loop.h"
#include "hw/hw.h"
#include "hw/pci/pci.h"

#define TYPE_ADLERDEV "adlerdev"

#define ADLERDEV_DEV(obj) \
	OBJECT_CHECK(AdlerDevState, (obj), TYPE_ADLERDEV)

typedef struct {
	PCIDevice dev;
	MemoryRegion mmio;
	QemuThread thread;
	QemuMutex mutex;
	QemuCond cond;
	bool stopping;
	/* Registers.  */
	uint32_t intr;
	uint32_t intr_enable;
	uint32_t data_ptr;
	uint32_t data_size;
	uint32_t sum;
} AdlerDevState;

static uint64_t adlerdev_mmio_read(void *opaque, hwaddr addr, unsigned size) {
	uint64_t res = ~0ULL;
	AdlerDevState *d = opaque;
	qemu_mutex_lock(&d->mutex);
	if (size == 4 && addr == ADLERDEV_INTR) {
		res = d->intr;
	} else if (size == 4 && addr == ADLERDEV_INTR_ENABLE) {
		res = d->intr_enable;
	} else if (size == 4 && addr == ADLERDEV_DATA_PTR) {
		res = d->data_ptr;
	} else if (size == 4 && addr == ADLERDEV_DATA_SIZE) {
		res = d->data_size;
	} else if (size == 4 && addr == ADLERDEV_SUM) {
		res = d->sum;
	} else {
		fprintf(stderr, "adlerdev error: invalid register read of size %u at %03x\n", size, (int)addr);
	}
	qemu_cond_signal(&d->cond);
	qemu_mutex_unlock(&d->mutex);
	return res;
}

static void adlerdev_mmio_write(void *opaque, hwaddr addr, uint64_t val,
                unsigned size)
{
	AdlerDevState *d = opaque;
	qemu_mutex_lock(&d->mutex);
	if (size == 4 && addr == ADLERDEV_INTR) {
		d->intr &= ~val;
	} else if (size == 4 && addr == ADLERDEV_INTR_ENABLE) {
		d->intr_enable = val & 1;
	} else if (size == 4 && addr == ADLERDEV_DATA_PTR) {
		d->data_ptr = val;
	} else if (size == 4 && addr == ADLERDEV_DATA_SIZE) {
		d->data_size = val;
	} else if (size == 4 && addr == ADLERDEV_SUM) {
		d->sum = val;
	} else {
		fprintf(stderr, "adlerdev error: invalid register write of size %u at %03x\n", size, (int)addr);
	}
	pci_set_irq(PCI_DEVICE(d), !!(d->intr & d->intr_enable));
	qemu_cond_signal(&d->cond);
	qemu_mutex_unlock(&d->mutex);
}

static const MemoryRegionOps adlerdev_mmio_ops = {
	.read = adlerdev_mmio_read,
	.write = adlerdev_mmio_write,
	.endianness = DEVICE_LITTLE_ENDIAN,
};

/* Power-up reset of the device.  */
static void adlerdev_power_reset(DeviceState *ds)
{
	AdlerDevState *d = ADLERDEV_DEV(ds);
	qemu_mutex_lock(&d->mutex);
	d->intr = mrand48() & 1;
	d->intr_enable = 0;
	d->data_ptr = mrand48();
	d->data_size = 0;
	d->sum = mrand48();
	qemu_mutex_unlock(&d->mutex);
}

static void *adlerdev_thread(void *opaque)
{
	AdlerDevState *d = opaque;
	qemu_mutex_lock(&d->mutex);
	while (1) {
		if (d->stopping)
			break;
		qemu_mutex_unlock(&d->mutex);
		qemu_mutex_lock_iothread();
		qemu_mutex_lock(&d->mutex);
		bool idle = !d->data_size;
		if (!idle) {
			uint8_t buf;
			pci_dma_read(&d->dev, d->data_ptr, &buf, sizeof buf);
			d->data_ptr++;
			d->data_size--;
			uint16_t hi = d->sum >> 16;
			uint16_t lo = d->sum;
			lo = (lo + buf) % ADLERDEV_MOD;
			hi = (hi + lo) % ADLERDEV_MOD;
			d->sum = hi << 16 | lo;
			if (!d->data_size) {
				d->intr |= 1;
				pci_set_irq(PCI_DEVICE(d), !!(d->intr & d->intr_enable));
			}
		}
		qemu_mutex_unlock_iothread();
		if (idle) {
			qemu_cond_wait(&d->cond, &d->mutex);
		}
	}
	qemu_mutex_unlock(&d->mutex);
	return 0;
}

static void adlerdev_realize(PCIDevice *pci_dev, Error **errp)
{
	AdlerDevState *d = ADLERDEV_DEV(pci_dev);
	uint8_t *pci_conf = d->dev.config;

	pci_config_set_interrupt_pin(pci_conf, 1);

	memory_region_init_io(&d->mmio, OBJECT(d), &adlerdev_mmio_ops, d, "adlerdev", ADLERDEV_BAR_SIZE);
	pci_register_bar(&d->dev, 0, PCI_BASE_ADDRESS_SPACE_MEMORY, &d->mmio);

	qemu_mutex_init(&d->mutex);
	qemu_cond_init(&d->cond);

	adlerdev_power_reset(&pci_dev->qdev);

	d->stopping = false;
	qemu_thread_create(&d->thread, "adlerdev", adlerdev_thread,
			d, QEMU_THREAD_JOINABLE);
}

static void adlerdev_exit(PCIDevice *pci_dev)
{
	AdlerDevState *d = ADLERDEV_DEV(pci_dev);
	qemu_mutex_lock(&d->mutex);
	d->stopping = true;
	qemu_mutex_unlock(&d->mutex);
	qemu_cond_signal(&d->cond);
	qemu_thread_join(&d->thread);
	qemu_cond_destroy(&d->cond);
	qemu_mutex_destroy(&d->mutex);
}

static void adlerdev_class_init(ObjectClass *klass, void *data)
{
	DeviceClass *dc = DEVICE_CLASS(klass);
	PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);

	k->realize = adlerdev_realize;
	k->exit = adlerdev_exit;
	k->vendor_id = ADLERDEV_VENDOR_ID;
	k->device_id = ADLERDEV_DEVICE_ID;
	k->class_id = PCI_CLASS_PROCESSOR_CO;
	dc->reset = adlerdev_power_reset;
}

static InterfaceInfo adlerdev_interfaces[] = {
	{ INTERFACE_CONVENTIONAL_PCI_DEVICE },
	{ },
};

static TypeInfo adlerdev_info = {
	.name          = TYPE_ADLERDEV,
	.parent        = TYPE_PCI_DEVICE,
	.instance_size = sizeof(AdlerDevState),
	.class_init    = adlerdev_class_init,
	.interfaces    = adlerdev_interfaces,
};

static void adlerdev_register_types(void)
{
	type_register_static(&adlerdev_info);
}

type_init(adlerdev_register_types)
