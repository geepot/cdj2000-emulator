/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef CDJ2000_NXS_HPI_H
#define CDJ2000_NXS_HPI_H
#include "system/memory.h"
void cdj_nxs_hpi_init(MemoryRegion *system, void (*hint)(void *, bool), void *opaque);
bool cdj_nxs_hpi_port(hwaddr address);
#endif
