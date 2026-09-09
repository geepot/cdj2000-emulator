#include "cdj_sh7764_iic.h"

enum { MIE = 8, FSB = 2, ESG = 1, MDBS = 128,
       MAT = 1, MDR = 2, MDT = 4, MDE = 8, MST = 16, MNR = 64 };

void cdj_sh7764_iic_reset(CdjSh7764Iic *s)
{
    *s = (CdjSh7764Iic){0};
}

bool cdj_sh7764_iic_attach(CdjSh7764Iic *s,
                           const CdjSh7764IicEndpoint *endpoint, void *opaque)
{
    if (s->phase != CDJ_IIC_IDLE) return false;
    if (endpoint && (!endpoint->start || !endpoint->write_byte ||
                     !endpoint->read_byte || !endpoint->stop)) return false;
    s->endpoint = endpoint;
    s->opaque = opaque;
    return true;
}

bool cdj_sh7764_iic_event_pending(const CdjSh7764Iic *s)
{
    return s->phase == CDJ_IIC_ADDRESS || s->phase == CDJ_IIC_BYTE ||
           s->phase == CDJ_IIC_STOP;
}

static void release_byte(CdjSh7764Iic *s)
{
    /* Single-buffer programming examples 16.6.1/16.6.2: address/data
     * completion stretches SCL until ESG and the data event are cleared.
     * MAT is independently W0C, not itself the clock-stretch condition. */
    unsigned flag = (s->issued_address & 1) ? MDR : MDE;
    if (s->phase == CDJ_IIC_WAIT_STOP && !(s->status & MDR)) {
        s->phase = CDJ_IIC_STOP;
    } else if (s->phase == CDJ_IIC_TX_READY && !(s->status & MDE)) {
        s->stop_after_byte = (s->control & FSB) != 0;
        s->phase = CDJ_IIC_BYTE;
    } else if (s->phase == CDJ_IIC_WAIT && !(s->control & ESG) &&
               !(s->status & flag)) {
        if (!(s->issued_address & 1)) {
            if (!s->tx_pending) {
                /* Last byte may have completed before software requested
                 * STOP. Clearing an empty buffer must not resend old TXD. */
                if (s->control & FSB) s->phase = CDJ_IIC_STOP;
                return;
            }
            s->tx_shift = s->tx;
            s->tx_pending = false;
            /* 16.3.6: MDE is buffer-to-shifter load, MDT is byte completion. */
            s->status |= MDE;
            /* Interpret 16.4.8's MDE SCL hold at each shift-loaded boundary,
             * consistent with 16.6.1 and Linux i2c-rcar irq_send's last-byte
             * SHIFT -> FSB -> clear-MDE sequence. This is reference-supported
             * event ordering, not measured SH7764 pin/cycle timing. */
            s->phase = CDJ_IIC_TX_READY;
            return;
        }
        s->stop_after_byte = (s->control & FSB) != 0;
        s->phase = CDJ_IIC_BYTE;
    }
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
            /* RX final ACK/STOP is fixed before receiving the byte. Mid-byte
             * changes need pin-phase modeling and must not silently succeed. */
            if (s->phase == CDJ_IIC_BYTE && (s->issued_address & 1) &&
                ((control ^ s->control) & FSB)) return false;
        } else if (control & ESG) {
            /* Attached transfers support the documented single-buffer path.
             * Double buffering needs separate data-register staging. */
            if (!(control & MDBS) &&
                (s->endpoint || (s->master_address & 1))) return false;
            s->issued_address = s->master_address;
            s->phase = CDJ_IIC_ADDRESS;
        }
        /* FSB while already idle generates no new physical STOP. NACK
         * already schedules STOP, so FSB during that phase is redundant. */
        s->control = control;
        if (s->phase == CDJ_IIC_BYTE && !(s->issued_address & 1)) {
            /* FSB is sampled at completion (16.4.9). Software may set it
             * after MDE announces that the last TX byte entered the shifter. */
            s->stop_after_byte = (control & FSB) != 0;
        }
        release_byte(s);
        return true;
    }
    case 0xc:
        if (value & 0x80) return false;
        s->status &= value; /* write-zero-to-clear; ones never set flags */
        release_byte(s);
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
        if (s->phase != CDJ_IIC_IDLE &&
            !((s->phase == CDJ_IIC_WAIT || s->phase == CDJ_IIC_TX_READY) &&
              !(s->issued_address & 1) &&
              (s->status & MDE))) return false;
        s->tx = value;
        s->tx_pending = true;
        return true;
    default: return false;
    }
}

bool cdj_sh7764_iic_advance(CdjSh7764Iic *s)
{
    switch (s->phase) {
    case CDJ_IIC_IDLE: case CDJ_IIC_WAIT: case CDJ_IIC_WAIT_STOP:
    case CDJ_IIC_TX_READY: return true;
    case CDJ_IIC_ADDRESS:
        s->selected = s->endpoint && s->endpoint->start(s->opaque,
            s->issued_address >> 1, (s->issued_address & 1) != 0);
        s->status |= MAT | ((s->issued_address & 1) ? MDR : MDE);
        if (s->selected) {
            s->phase = CDJ_IIC_WAIT;
        } else {
            /* Empty or non-acknowledging bus: automatic STOP (16.4.8). */
            s->status |= MNR;
            s->phase = CDJ_IIC_STOP;
        }
        return true;
    case CDJ_IIC_BYTE: {
        if (!s->selected || !s->endpoint) return false;
        bool ack = true;
        if (s->issued_address & 1) {
            uint8_t value;
            if (!s->endpoint->read_byte(s->opaque, &value)) return false;
            s->rx = value;
            s->status |= MDR;
        } else {
            ack = s->endpoint->write_byte(s->opaque, s->tx_shift);
            s->status |= MDT;
            if (!ack) s->status |= MNR;
        }
        if (s->stop_after_byte && (s->issued_address & 1)) {
            /* MDBS holds SCL low until firmware consumes/clears MDR, even
             * for the last byte (16.3.5/16.3.6 and 16.6.2). */
            s->phase = CDJ_IIC_WAIT_STOP;
        } else {
            s->phase = (!ack || s->stop_after_byte) ? CDJ_IIC_STOP : CDJ_IIC_WAIT;
            release_byte(s);
        }
        return true;
    }
    case CDJ_IIC_STOP:
        /* NACK automatically outputs STOP regardless of FSB (§16.4.8). */
        s->status |= MST;
        if (s->selected) s->endpoint->stop(s->opaque);
        s->selected = false;
        s->phase = CDJ_IIC_IDLE;
        return true;
    default: return false;
    }
}
