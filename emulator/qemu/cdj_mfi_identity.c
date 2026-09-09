#include "cdj_mfi_identity.h"

/* Independent primary source (read 2026-09-09): Apple HomeKitADK,
 * HAP/HAPMFiHWAuth+Types.h, register declarations at lines 27-44 and
 * HAPMFiHWAuthDeviceVersion_2_0C at line 392. These cite Accessory Interface
 * Specification R29 section 69.8.1. The documented read-only device version
 * is 0x05 and 2.0C firmware-version power-up value is 0x01.
 * https://github.com/apple/HomeKitADK/blob/master/HAP/HAPMFiHWAuth%2BTypes.h
 * Archive master head observed in GitHub commit history:
 * fb201f98f5fdc7fef6a455054f08b59cca5d1ec8 (2021-10-23).
 * Pinned-file fetch was unavailable; the source above was read at master.
 * No values are inferred solely from the Pioneer firmware's expectations.
 */
bool cdj_mfi_identity_read(uint32_t reg, size_t length, uint8_t *output)
{
    if (!output || length != 1 || reg > 1) {
        return false;
    }
    *output = reg == 0 ? 0x05 : 0x01;
    return true;
}
