// SPDX-License-Identifier: GPL-2.0-only
#include <linux/async.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/pci.h>
#include <linux/string.h>

#include "unlock.h"

static char *targets;
module_param(targets, charp, 0444);
MODULE_PARM_DESC(targets, "\"all\" or comma-separated PCI addresses of supported cards to unlock");

/* TU102 GPU functions in pci.ids, excluding audio functions. Only the Dell
 * RTX 2080 Ti 22GB (10de:1e07) has been validated.
 */
static const unsigned short supported_ids[] = {
	0x1e07,
};

static ASYNC_DOMAIN_EXCLUSIVE(unlock_domain);
static DEFINE_MUTEX(tu102_lock);

/* The next supported NVIDIA card after from, like pci_get_device(). */
static struct pci_dev *next_supported(struct pci_dev *from)
{
	struct pci_dev *pdev = from;
	unsigned int i;

	while ((pdev = pci_get_device(PCI_VENDOR_ID_NVIDIA, PCI_ANY_ID, pdev)))
		for (i = 0; i < ARRAY_SIZE(supported_ids); i++)
			if (pdev->device == supported_ids[i])
				return pdev;
	return NULL;
}

/* Runs concurrently for each card; consumes the reference it is given. */
static void unlock_one(void *data, async_cookie_t cookie)
{
	struct pci_dev *pdev = data;
	int ret;

	pci_dev_lock(pdev);
	if (pdev->driver) {
		ret = -EBUSY;
	} else {
		/* TU102 transactions are serialized. */
		mutex_lock(&tu102_lock);
		ret = turing_unlock_card(pdev);
		mutex_unlock(&tu102_lock);
	}
	pci_dev_unlock(pdev);
	if (ret)
		pci_err(pdev, "turing_unlock: unlock failed (%d)\n", ret);
	else
		pci_info(pdev, "turing_unlock: unlocked\n");
	pci_dev_put(pdev);
}

/* True when the comma-separated targets list names this PCI device. */
static bool listed(const char *name)
{
	const char *entry, *end;

	for (entry = targets; *entry; entry = *end ? end + 1 : end) {
		end = strchrnul(entry, ',');
		if (strlen(name) == end - entry && !strncasecmp(name, entry, end - entry))
			return true;
	}
	return false;
}

static void report_unmatched(void)
{
	const char *entry, *end;
	struct pci_dev *pdev;

	for (entry = targets; *entry; entry = *end ? end + 1 : end) {
		end = strchrnul(entry, ',');
		pdev = NULL;
		while ((pdev = next_supported(pdev)))
			if (strlen(pci_name(pdev)) == end - entry &&
			    !strncasecmp(pci_name(pdev), entry, end - entry))
				break;
		if (pdev)
			pci_dev_put(pdev);
		else
			pr_err("turing_unlock: no TU102 card at \"%.*s\"\n",
			       (int)(end - entry), entry);
	}
}

static int __init turing_unlock_init(void)
{
	struct pci_dev *pdev = NULL;
	bool all;

	/* Loaded ahead of nvidia on every boot; the command line is the opt-in. */
	if (!targets || !*targets)
		return 0;
	all = !strcmp(targets, "all");
	if (!all)
		report_unmatched();
	while ((pdev = next_supported(pdev))) {
		if (!all && !listed(pci_name(pdev)))
			continue;
		async_schedule_domain(unlock_one, pci_dev_get(pdev), &unlock_domain);
	}
	/* nvidia loads once this returns: every card must be finished by then. */
	async_synchronize_full_domain(&unlock_domain);
	return 0;
}

static void __exit turing_unlock_exit(void)
{
}

module_init(turing_unlock_init);
module_exit(turing_unlock_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Unlock full speed FP16 tensor core");
MODULE_VERSION("2.0.0");
