// SPDX-License-Identifier: GPL-2.0-only
#ifndef TURING_UNLOCK_FIRMWARE_H
#define TURING_UNLOCK_FIRMWARE_H

#include <linux/device.h>
#include <linux/firmware.h>
#include <linux/slab.h>

/* CONFIG_FW_CACHE attaches name records to devres even though this is not
 * devm_request_firmware(). release_firmware() does not remove them, and
 * really_probe() refuses a device with any existing devres. Scope each
 * synchronous request so only its cache records are released; the returned
 * firmware remains owned by the caller.
 */
static inline int turing_unlock_request_scoped(struct device *dev,
					    const struct firmware **fw, const char *name)
{
	void *group;
	int ret, released;

	group = devres_open_group(dev, NULL, GFP_KERNEL);
	if (!group)
		return -ENOMEM;
	dev_info(dev, "asset request: %s\n", name);
	ret = request_firmware(fw, name, dev);
	released = devres_release_group(dev, group);
	dev_info(dev, "asset result: %s error=%d bytes=%zu\n",
		name, ret, ret ? (size_t)0 : (*fw)->size);
	dev_info(dev, "asset cache cleanup: %s devres_released=%d\n", name, released);
	return ret;
}

static inline int turing_unlock_check_devres(struct device *dev, const char *phase)
{
	unsigned long flags;
	bool empty;

	spin_lock_irqsave(&dev->devres_lock, flags);
	empty = list_empty(&dev->devres_head);
	spin_unlock_irqrestore(&dev->devres_lock, flags);
	dev_info(dev, "driver probe resources: phase=%s empty=%u\n", phase, empty);
	return empty ? 0 : -EBUSY;
}

#endif
