#ifndef STDINT_H
#define STDINT_H

/* 精确宽度整数类型 */
typedef unsigned char uint8_t;
typedef signed char int8_t;
typedef unsigned short uint16_t;
typedef signed short int16_t;
typedef unsigned int uint32_t;
typedef signed int int32_t;
typedef unsigned long long uint64_t;
typedef signed long long int64_t;

/* 指针宽度整数 */
typedef unsigned long uintptr_t;
typedef long intptr_t;

/* 最大最小值 */
#define UINT8_MAX   0xFF
#define INT8_MAX    0x7F
#define INT8_MIN    (-0x7F - 1)
#define UINT16_MAX  0xFFFF
#define INT16_MAX   0x7FFF
#define INT16_MIN   (-0x7FFF - 1)
#define UINT32_MAX  0xFFFFFFFFU
#define INT32_MAX   0x7FFFFFFF
#define INT32_MIN   (-0x7FFFFFFF - 1)
#define UINT64_MAX  0xFFFFFFFFFFFFFFFFULL
#define INT64_MAX   0x7FFFFFFFFFFFFFFFULL
#define INT64_MIN   (-0x7FFFFFFFFFFFFFFFLL - 1)

#endif
