/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef TURING_UNLOCK_UNLOCK_H
#define TURING_UNLOCK_UNLOCK_H

struct pci_dev;

int turing_unlock_card(struct pci_dev *pdev);

#endif
