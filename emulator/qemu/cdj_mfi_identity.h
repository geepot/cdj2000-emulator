#ifndef CDJ_MFI_IDENTITY_H
#define CDJ_MFI_IDENTITY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Stateless Apple Authentication Coprocessor 2.0C identity subset only.
 * Exact one-byte register 0/1 reads are supported. All other requests return
 * false without changing output. This is NOT an authentication implementation:
 * no certificate, key, self-test result, error/status or crypto is supplied.
 * The caller owns bus attachment, addressing, selection, ACK and timing.
 * A false result is an unsupported-model operation, not a hardware NACK claim.
 */
bool cdj_mfi_identity_read(uint32_t reg, size_t length, uint8_t *output);

#endif
