/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef CDJ2000_NXS_HPI_H
#define CDJ2000_NXS_HPI_H
#include "system/memory.h"
void cdj_nxs_hpi_init(MemoryRegion *system, void (*hint)(void *, bool), void *opaque);
void cdj_nxs_hpi_reset_line(bool released);
void cdj_nxs_hpi_boot_phase(unsigned phase);
bool cdj_nxs_hpi_port(hwaddr address);
#endif
