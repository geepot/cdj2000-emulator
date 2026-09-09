#include "cdj_sh7764_iic.h"

enum { MIE = 8, FSB = 2, ESG = 1, MDBS = 128,
       MAT = 1, MDE = 8, MST = 16, MNR = 64 };

void cdj_sh7764_iic_reset(CdjSh7764Iic *s)
{
    *s = (CdjSh7764Iic){0};
}

uint32_t cdj_sh7764_iic_scl_period(const CdjSh7764Iic *s)
{
    return (1u + (s->clock & 3u)) * (20u + 8u * (s->clock >> 2));
}

bool cdj_sh7764_iic_read(const CdjSh7764Iic *s, uint32_t offset,
                         unsigned size, uint32_t *value)
{
    uint32_t v;
    if (size != 1) return false;
    switch (offset) {
    case 0: case 8: case 0x10: case 0x14: v = 0; break;
    /* FSDA is BUSY, not SDA electrical level (§16.3.5). Sample SCL only
     * at event boundaries: idle high; active phase is not pin-cycle accurate. */
    case 4: v = s->control | (s->phase == CDJ_IIC_IDLE ? 0x40 : 0x20); break;
    case 0xc: v = s->status; break;
    case 0x18: v = s->clock; break;
    case 0x1c: v = s->slave_address; break;
    case 0x20: v = s->master_address; break;
    case 0x24: v = s->rx; break; /* RX and TX are distinct (§16.3.10). */
    default: return false;
    }
    *value = v;
    return true;
}

bool cdj_sh7764_iic_write(CdjSh7764Iic *s, uint32_t offset,
                          unsigned size, uint32_t value)
{
    if (size != 1 || value > 255) return false;
    switch (offset) {
    case 0: case 8: case 0x10: case 0x14:
        /* Slave operation and IRQ delivery are outside this bounded model. */
        return value == 0;
    case 4: {
        uint8_t control = value & (MDBS | MIE | FSB | ESG);
        /* OBPC/test pin control and start-byte mode unsupported. FSCL/FSDA
         * writes do not drive pins without OBPC, so RMW is permitted. */
        if (value & 0x14) return false;
        if ((control & (FSB | ESG)) && !(control & MIE)) return false;
        if ((control & (FSB | ESG)) == (FSB | ESG)) return false;
        if (s->phase != CDJ_IIC_IDLE) {
            /* No aborts, repeated START or controller disable mid-transfer. */
            if (!(control & MIE) || (control & ESG)) return false;
            if ((control ^ s->control) & MDBS) return false;
        } else if (control & ESG) {
            /* Only reached master-transmit path; receive remains fail-closed. */
            if (s->master_address & 1) return false;
            s->issued_address = s->master_address;
            s->phase = CDJ_IIC_ADDRESS;
        }
        /* FSB while already idle generates no new physical STOP. NACK
         * already schedules STOP, so FSB during that phase is redundant. */
        s->control = control;
        return true;
    }
    case 0xc:
        if (value & 0x80) return false;
        s->status &= value; /* write-zero-to-clear; ones never set flags */
        return true;
    case 0x18:
        if (s->phase != CDJ_IIC_IDLE) return false;
        s->clock = value;
        return true;
    case 0x1c:
        if (s->phase != CDJ_IIC_IDLE) return false;
        s->slave_address = value;
        return true;
    case 0x20:
        if (s->phase != CDJ_IIC_IDLE) return false;
        s->master_address = value;
        return true;
    case 0x24:
        if (s->phase != CDJ_IIC_IDLE) return false;
        s->tx = value;
        return true;
    default: return false;
    }
}

bool cdj_sh7764_iic_advance(CdjSh7764Iic *s)
{
    switch (s->phase) {
    case CDJ_IIC_IDLE: return true;
    case CDJ_IIC_ADDRESS:
        /* §16.3.6 and §16.4.8: address was transmitted, NOT acknowledged.
         * Empty bus leaves SDA high on ACK clock. No data byte is sent. */
        s->status |= MAT | MDE | MNR;
        s->phase = CDJ_IIC_STOP;
        return true;
    case CDJ_IIC_STOP:
        /* NACK automatically outputs STOP regardless of FSB (§16.4.8). */
        s->status |= MST;
        s->phase = CDJ_IIC_IDLE;
        return true;
    default: return false;
    }
}
