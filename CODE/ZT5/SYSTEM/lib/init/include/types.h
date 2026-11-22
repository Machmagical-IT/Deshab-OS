#ifndef TYPES_H
#define TYPES_H

typedef unsigned char       uint8_t;
typedef unsigned short      uint16_t;
typedef unsigned int        uint32_t;
typedef unsigned long long  uint64_t;

typedef signed char         int8_t;
typedef signed short        int16_t;
typedef signed int          int32_t;
typedef signed long long    int64_t;

typedef unsigned long       size_t;
typedef signed long         ssize_t;
typedef signed long         intptr_t;
typedef unsigned long       uintptr_t;

typedef int                 pid_t;
typedef int                 mode_t;
typedef int                 dev_t;
typedef int                 uid_t;
typedef int                 gid_t;

#define NULL ((void *)0)

#endif
