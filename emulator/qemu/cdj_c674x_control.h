/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef CDJ_C674X_CONTROL_H
#define CDJ_C674X_CONTROL_H
#include <stdbool.h>
#include <stdint.h>
#include "cdj_c674x.h"
/* The architectural read view of a control register: reserved fields that
 * SPRUFE8B marks "always read as 0" are masked off here rather than being
 * kept out of storage, and the few derived fields (ISTP.HPEINT, TSR/ITSR GIE)
 * are computed from the registers they alias.  Reads only cpu->control[]. */
uint32_t cdj_c674x_control_read(const CdjC674x *cpu, unsigned id);
/* Whether MVC can name this control register id as a source / destination.
 * Ids this core does not model stay fail-closed: the execute packet rejects
 * them rather than reading or storing an invented value. */
bool cdj_c674x_control_read_supported(unsigned id);
bool cdj_c674x_control_write_supported(unsigned id);
#endif
