#ifndef CDJ_SH7764_IIC_H
#define CDJ_SH7764_IIC_H

#include <stdbool.h>
#include <stdint.h>

/* SH7764 R01UH0360EJ0300, chapter 16. Pulled-up, single-master bus.
 * An unattached bus always NACKs. Endpoints supply all device-specific data.
 * Advance processes one protocol event, NOT one CPU/peripheral clock. The
 * caller must document its scheduling approximation; reads never advance time.
 * Offset API deliberately leaves P4/Area7 mapping to the integration layer.
 */
enum { CDJ_IIC_IDLE, CDJ_IIC_ADDRESS, CDJ_IIC_STOP,
       CDJ_IIC_WAIT, CDJ_IIC_BYTE, CDJ_IIC_WAIT_STOP, CDJ_IIC_TX_READY };
typedef struct CdjSh7764IicEndpoint {
    bool (*start)(void *opaque, uint8_t address7, bool read);
    bool (*write_byte)(void *opaque, uint8_t value); /* ACK / NACK */
    /* False is unsupported, not zero data or an invented slave NACK. */
    bool (*read_byte)(void *opaque, uint8_t *value);
    void (*stop)(void *opaque);
} CdjSh7764IicEndpoint;
typedef struct CdjSh7764Iic {
    uint8_t control, status, clock, slave_address, master_address;
    uint8_t tx, rx, phase, issued_address, tx_shift;
    const CdjSh7764IicEndpoint *endpoint;
    void *opaque;
    bool selected, stop_after_byte, tx_pending;
} CdjSh7764Iic;

void cdj_sh7764_iic_reset(CdjSh7764Iic *s);
/* Reset detaches; integration reattaches its reset endpoint afterward. */
bool cdj_sh7764_iic_attach(CdjSh7764Iic *s,
                           const CdjSh7764IicEndpoint *endpoint, void *opaque);
bool cdj_sh7764_iic_event_pending(const CdjSh7764Iic *s);
/* Peripheral-clock cycles per SCL period (manual 16.3.9). This is not
 * START/STOP latency and does not select an unverified board frequency. */
uint32_t cdj_sh7764_iic_scl_period(const CdjSh7764Iic *s);
/* False means unsupported access: neither state nor output is changed. */
bool cdj_sh7764_iic_read(const CdjSh7764Iic *s, uint32_t offset,
                         unsigned size, uint32_t *value);
bool cdj_sh7764_iic_write(CdjSh7764Iic *s, uint32_t offset,
                          unsigned size, uint32_t value);
bool cdj_sh7764_iic_advance(CdjSh7764Iic *s);
#endif
