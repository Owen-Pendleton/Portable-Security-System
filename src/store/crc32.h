#ifndef PSS_STORE_CRC32_H
#define PSS_STORE_CRC32_H

#include <stddef.h>
#include <stdint.h>

/* CRC-32 (IEEE 802.3, reflected, poly 0xEDB88320); crc32("123456789") == 0xCBF43926. */
uint32_t pss_crc32(const void *data, size_t len);

#endif
