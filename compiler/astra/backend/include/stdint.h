#ifndef _ASTRA_STDINT_H
#define _ASTRA_STDINT_H

/* Minimal freestanding stdint.h for the Astra toolchain.
 * LP64 layout (matches glibc on x86_64 Linux). */

typedef signed char int8_t;
typedef short int16_t;
typedef int int32_t;
typedef long int64_t;
typedef unsigned char uint8_t;
typedef unsigned short uint16_t;
typedef unsigned int uint32_t;
typedef unsigned long uint64_t;

typedef int64_t intmax_t;
typedef uint64_t uintmax_t;
typedef long intptr_t;
typedef unsigned long uintptr_t;

#define INT8_MAX 127
#define INT8_MIN (-128)
#define INT16_MAX 32767
#define INT16_MIN (-32768)
#define INT32_MAX 2147483647
#define INT32_MIN (-2147483647 - 1)
#define INT64_MAX 9223372036854775807L
#define INT64_MIN (-INT64_MAX - 1L)

#define UINT8_MAX 255
#define UINT16_MAX 65535U
#define UINT32_MAX 4294967295U
#define UINT64_MAX 18446744073709551615UL

#define INTMAX_MAX INT64_MAX
#define INTMAX_MIN INT64_MIN
#define UINTMAX_MAX UINT64_MAX

#define INT8_C(c) (c)
#define INT16_C(c) (c)
#define INT32_C(c) (c ## L)
#define INT64_C(c) (c ## L)
#define UINT8_C(c) (c)
#define UINT16_C(c) (c ## U)
#define UINT32_C(c) (c ## UL)
#define UINT64_C(c) (c ## UL)
#define INTMAX_C(c) (c ## L)
#define UINTMAX_C(c) (c ## UL)

#endif /* _ASTRA_STDINT_H */
