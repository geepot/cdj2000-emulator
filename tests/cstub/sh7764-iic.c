#include "cdj_sh7764_iic.h"
#include <assert.h>
#include <string.h>

static uint32_t read8(CdjSh7764Iic *s, uint32_t offset)
{
    uint32_t value = 0xdeadbeef;
    assert(cdj_sh7764_iic_read(s, offset, 1, &value));
    return value;
}
static void reject(CdjSh7764Iic *s, unsigned offset, unsigned size, unsigned v)
{
    CdjSh7764Iic before = *s;
    assert(!cdj_sh7764_iic_write(s, offset, size, v));
    assert(memcmp(&before, s, sizeof before) == 0);
}
typedef struct Bus {
    unsigned starts, writes, reads, stops;
    uint8_t byte;
    bool nack, unavailable;
} Bus;
static bool start(void *opaque, uint8_t address, bool read)
{
    Bus *b = opaque;
    b->starts++;
    (void)read;
    return address == 0x10;
}
static bool write_byte(void *opaque, uint8_t value)
{
    Bus *b = opaque;
    b->writes++;
    b->byte = value;
    return !b->nack;
}
static bool read_byte(void *opaque, uint8_t *value)
{
    Bus *b = opaque;
    if (b->unavailable) return false;
    b->reads++;
    *value = b->byte;
    return true;
}
static void stop(void *opaque) { ((Bus *)opaque)->stops++; }
static const CdjSh7764IicEndpoint endpoint = { start, write_byte, read_byte, stop };
static void put(CdjSh7764Iic *s, unsigned o, unsigned v)
{
    assert(cdj_sh7764_iic_write(s, o, 1, v));
}
static void transfer_tests(void)
{
    CdjSh7764Iic s;
    Bus b = {0};
    cdj_sh7764_iic_reset(&s);
    assert(cdj_sh7764_iic_attach(&s, &endpoint, &b));
    put(&s, 0x20, 0x20); put(&s, 0x24, 0xab); put(&s, 4, 0x89);
    assert(cdj_sh7764_iic_event_pending(&s));
    assert(cdj_sh7764_iic_advance(&s));
    assert(read8(&s, 0xc) == 9 && b.starts == 1 && b.writes == 0);
    assert(!cdj_sh7764_iic_event_pending(&s));
    assert(!cdj_sh7764_iic_attach(&s, &endpoint, &b));
    /* Status clear alone cannot release ESG. Clearing control then launches. */
    put(&s, 0xc, 0); assert(!cdj_sh7764_iic_event_pending(&s));
    put(&s, 4, 0x8a); assert(s.phase == CDJ_IIC_TX_READY);
    put(&s, 0xc, 0); assert(s.phase == CDJ_IIC_BYTE);
    assert(cdj_sh7764_iic_advance(&s));
    assert(b.writes == 1 && b.byte == 0xab && read8(&s, 0xc) == 4);
    assert(s.phase == CDJ_IIC_STOP);
    assert(cdj_sh7764_iic_advance(&s));
    assert(b.stops == 1 && read8(&s, 0xc) == 0x14);
    /* Real byte supplied by endpoint, not RX=TX or a controller identity. */
    put(&s, 0xc, 0); put(&s, 0x20, 0x21); put(&s, 4, 0x89);
    assert(cdj_sh7764_iic_advance(&s));
    assert(read8(&s, 0xc) == 3 && b.reads == 0);
    put(&s, 4, 0x8a); assert(s.phase == CDJ_IIC_WAIT);
    put(&s, 0xc, 0); assert(s.phase == CDJ_IIC_BYTE);
    reject(&s, 4, 1, 0x88); /* Changing active RX FSB must not be ignored. */
    b.unavailable = true;
    CdjSh7764Iic before = s;
    assert(!cdj_sh7764_iic_advance(&s));
    assert(memcmp(&before, &s, sizeof s) == 0 && b.reads == 0);
    b.unavailable = false;
    assert(cdj_sh7764_iic_advance(&s));
    assert(read8(&s, 0x24) == 0xab && read8(&s, 0xc) == 2);
    assert(!cdj_sh7764_iic_event_pending(&s));
    assert(cdj_sh7764_iic_advance(&s)); /* stretched SCL does not STOP */
    assert(b.stops == 1);
    put(&s, 0xc, 0); assert(s.phase == CDJ_IIC_STOP);
    assert(cdj_sh7764_iic_advance(&s));
    assert(b.stops == 2 && b.reads == 1);
    /* A real TX NACK terminates even without FSB. */
    put(&s, 0xc, 0); put(&s, 0x20, 0x20); put(&s, 4, 0x89);
    assert(cdj_sh7764_iic_advance(&s));
    put(&s, 0x24, 0x42); put(&s, 4, 0x88); put(&s, 0xc, 0);
    put(&s, 0xc, 0);
    b.nack = true;
    assert(cdj_sh7764_iic_advance(&s));
    assert(read8(&s, 0xc) == 0x44 && s.phase == CDJ_IIC_STOP);
    assert(cdj_sh7764_iic_advance(&s));
    assert(b.stops == 3);
    cdj_sh7764_iic_reset(&s);
    assert(!s.endpoint && !cdj_sh7764_iic_event_pending(&s));
    /* An absent receiver also NACKs, never returns synthetic RX data. */
    put(&s, 0x20, 0x21); put(&s, 4, 0x89);
    assert(cdj_sh7764_iic_advance(&s));
    assert(read8(&s, 0xc) == 0x43 && read8(&s, 0x24) == 0);
}
static void tx_staging_tests(void)
{
    CdjSh7764Iic s;
    Bus b = {0};
    for (unsigned late = 0; late < 2; ++late) {
        cdj_sh7764_iic_reset(&s);
        assert(cdj_sh7764_iic_attach(&s, &endpoint, &b));
        put(&s, 0xc, 0); put(&s, 0x20, 0x20); put(&s, 0x24, 0x63);
        put(&s, 4, 0x89); assert(cdj_sh7764_iic_advance(&s));
        /* Exact firmware sequence 0427fd54..66: ESG, MAT, MDE clears. */
        put(&s, 4, 0x88); put(&s, 0xc, 8); put(&s, 0xc, 0);
        assert(s.phase == CDJ_IIC_TX_READY && read8(&s, 0xc) == 8);
        assert(s.tx_shift == 0x63 && !s.tx_pending);
        unsigned writes = b.writes;
        for (unsigned i = 0; i < 1000 * late; ++i) {
            assert(!cdj_sh7764_iic_event_pending(&s));
            assert(cdj_sh7764_iic_advance(&s));
            assert(b.writes == writes && !(read8(&s, 0xc) & 0x14));
        }
        /* 0427fdd6..e8: last-byte FSB after seeing MDE, then clear it. */
        put(&s, 4, 0x8a);
        for (unsigned i = 0; i < 1000 * late; ++i) {
            assert(cdj_sh7764_iic_advance(&s));
            assert(s.phase == CDJ_IIC_TX_READY && b.writes == writes);
        }
        put(&s, 0xc, read8(&s, 0xc) & 0xfe);
        put(&s, 0xc, read8(&s, 0xc) & 0xf7);
        assert(s.phase == CDJ_IIC_BYTE && cdj_sh7764_iic_event_pending(&s));
        assert(cdj_sh7764_iic_advance(&s));
        assert(s.phase == CDJ_IIC_STOP && b.writes == writes + 1);
        assert(b.byte == 0x63 && read8(&s, 0xc) == 4);
        assert(cdj_sh7764_iic_advance(&s));
        assert(s.phase == CDJ_IIC_IDLE && read8(&s, 0xc) == 0x14);
        assert(b.writes == writes + 1); /* No stale TXD duplication. */
    }
    cdj_sh7764_iic_reset(&s);
    assert(cdj_sh7764_iic_attach(&s, &endpoint, &b));
    put(&s, 0x20, 0x20); put(&s, 0x24, 0x11); put(&s, 4, 0x89);
    assert(cdj_sh7764_iic_advance(&s));
    put(&s, 4, 0x88); put(&s, 0xc, 0);
    put(&s, 0x24, 0x22); put(&s, 0xc, 0);
    assert(cdj_sh7764_iic_advance(&s));
    assert(b.byte == 0x11); /* Buffered new TXD cannot overwrite shifter. */
    assert(s.phase == CDJ_IIC_TX_READY && s.tx_shift == 0x22);
    put(&s, 4, 0x8a); put(&s, 0xc, 0);
    assert(cdj_sh7764_iic_advance(&s));
    assert(b.byte == 0x22 && s.phase == CDJ_IIC_STOP);
}
int main(void)
{
    transfer_tests();
    tx_staging_tests();
    CdjSh7764Iic s;
    cdj_sh7764_iic_reset(&s);
    assert(read8(&s, 4) == 0x40); /* pulled-up idle bus, not reset constant */
    for (unsigned o = 0; o <= 0x24; o += 4)
        if (o != 4) assert(read8(&s, o) == 0);
    assert(cdj_sh7764_iic_write(&s, 0x24, 1, 0xa5));
    assert(read8(&s, 0x24) == 0 && s.tx == 0xa5);
    /* Firmware preserves SCGD across its second read-modify-write. A
     * zero-read trap incorrectly turns this into 2 instead of 0x0e. */
    assert(cdj_sh7764_iic_write(&s, 0x18, 1, read8(&s, 0x18) | 0x0c));
    assert(cdj_sh7764_iic_write(&s, 0x18, 1,
                               (read8(&s, 0x18) & 0xfc) | 2));
    assert(read8(&s, 0x18) == 0x0e);
    assert(cdj_sh7764_iic_scl_period(&s) == 132);
    for (unsigned cdf = 0; cdf < 4; ++cdf) {
        for (unsigned scgd = 0; scgd < 64; ++scgd) {
            assert(cdj_sh7764_iic_write(&s, 0x18, 1, scgd * 4 + cdf));
            assert(cdj_sh7764_iic_scl_period(&s) ==
                   (cdf + 1) * (20 + scgd * 8));
        }
    }
    for (unsigned address = 0; address < 128; address++) {
        cdj_sh7764_iic_reset(&s);
        assert(cdj_sh7764_iic_write(&s, 0x18, 1, 0xc));
        assert(read8(&s, 0x18) == 0xc);
        assert(cdj_sh7764_iic_write(&s, 0x18, 1, 2));
        assert(cdj_sh7764_iic_write(&s, 0x20, 1, address << 1));
        assert(cdj_sh7764_iic_write(&s, 0x24, 1, 0));
        assert(cdj_sh7764_iic_write(&s, 4, 1, 0x89));
        assert(s.issued_address == address << 1);
        assert(read8(&s, 0xc) == 0);
        CdjSh7764Iic before = s;
        for (int i = 0; i < 100; i++) (void)read8(&s, 4);
        assert(memcmp(&before, &s, sizeof s) == 0);
        reject(&s, 4, 1, 0x89); /* repeated START */
        reject(&s, 4, 1, 0); /* active disable */
        reject(&s, 4, 1, 8); /* active transfer-mode change */
        reject(&s, 0x18, 1, 0);
        reject(&s, 0x20, 1, 0);
        reject(&s, 0x24, 1, 1);
        assert(cdj_sh7764_iic_advance(&s));
        assert(read8(&s, 0xc) == 0x49); /* MAT/MDE/MNR; no MDT/MDR */
        assert(cdj_sh7764_iic_write(&s, 4, 1, 0x88));
        assert(cdj_sh7764_iic_advance(&s));
        assert(read8(&s, 0xc) == 0x59);
        assert(read8(&s, 4) == 0xc8);
        assert(read8(&s, 0x24) == 0);
        assert(cdj_sh7764_iic_write(&s, 0xc, 1, 0x7f));
        assert(read8(&s, 0xc) == 0x59);
        assert(cdj_sh7764_iic_write(&s, 0xc, 1, 0x3f));
        assert(read8(&s, 0xc) == 0x19);
        assert(cdj_sh7764_iic_write(&s, 0xc, 1, 0));
        assert(read8(&s, 0xc) == 0);
        assert(cdj_sh7764_iic_write(&s, 4, 1, 0x8a));
        assert(cdj_sh7764_iic_advance(&s));
        assert(read8(&s, 0xc) == 0); /* idle FSB isn't a transfer */
    }
    cdj_sh7764_iic_reset(&s);
    reject(&s, 0, 1, 4); reject(&s, 0x10, 1, 1);
    reject(&s, 0x14, 1, 1); reject(&s, 4, 1, 0x10);
    reject(&s, 4, 1, 0xc); reject(&s, 4, 1, 0xb);
    reject(&s, 4, 1, 1); reject(&s, 0xc, 1, 128);
    reject(&s, 0x25, 1, 0); reject(&s, 0x18, 2, 0);
    reject(&s, 0x18, 1, 256);
    assert(cdj_sh7764_iic_write(&s, 0x20, 1, 0x21));
    reject(&s, 4, 1, 9); /* receiver unsupported, no invented RX data */
    uint32_t value = 0xdeadbeef;
    assert(!cdj_sh7764_iic_read(&s, 1, 1, &value));
    assert(!cdj_sh7764_iic_read(&s, 4, 4, &value));
    assert(value == 0xdeadbeef);
    s.phase = 255;
    CdjSh7764Iic before = s;
    assert(!cdj_sh7764_iic_advance(&s));
    assert(memcmp(&before, &s, sizeof s) == 0);
    return 0;
}
