/* <endian.h> for LKL's host library, which newlib does not have. The Switch
 * is little-endian. */
#ifndef GX_NX_ENDIAN_H
#define GX_NX_ENDIAN_H

#include <machine/endian.h>
#include <arpa/inet.h>

#define htole16(x) (x)
#define htole32(x) (x)
#define htole64(x) (x)
#define le16toh(x) (x)
#define le32toh(x) (x)
#define le64toh(x) (x)
#define htobe16(x) __builtin_bswap16(x)
#define htobe32(x) __builtin_bswap32(x)
#define htobe64(x) __builtin_bswap64(x)
#define be16toh(x) __builtin_bswap16(x)
#define be32toh(x) __builtin_bswap32(x)
#define be64toh(x) __builtin_bswap64(x)

#endif
