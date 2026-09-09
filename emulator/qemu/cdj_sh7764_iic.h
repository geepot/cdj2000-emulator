#ifndef CDJ_SH7764_IIC_H
#define CDJ_SH7764_IIC_H

#include <stdbool.h>
#include <stdint.h>

/* SH7764 R01UH0360EJ0300, chapter 16. Empty, pulled-up, single-master bus.
 * No slave is attached: addressing ALWAYS NACKs. No authentication identity.
 * Advance processes one protocol event, NOT one CPU/peripheral clock. The
 * caller must document its scheduling approximation; reads never advance time.
 * Offset API deliberately leaves P4/Area7 mapping to the integration layer.
 */
enum { CDJ_IIC_IDLE, CDJ_IIC_ADDRESS, CDJ_IIC_STOP };
typedef struct CdjSh7764Iic {
    uint8_t control, status, clock, slave_address, master_address;
    uint8_t tx, rx, phase, issued_address;
} CdjSh7764Iic;

void cdj_sh7764_iic_reset(CdjSh7764Iic *s);
/* False means unsupported access: neither state nor output is changed. */
bool cdj_sh7764_iic_read(const CdjSh7764Iic *s, uint32_t offset,
                         unsigned size, uint32_t *value);
bool cdj_sh7764_iic_write(CdjSh7764Iic *s, uint32_t offset,
                          unsigned size, uint32_t value);
bool cdj_sh7764_iic_advance(CdjSh7764Iic *s);
#endif
