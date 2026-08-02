#pragma once

/* ============================================================================
 * PD-Kernel  —  Core type definitions and macros
 * ============================================================================ */

/* Fixed-width integer types (no stdint.h in freestanding mode) */
typedef unsigned char       uint8_t;
typedef unsigned short      uint16_t;
typedef unsigned int        uint32_t;
typedef unsigned long long  uint64_t;
typedef signed char         int8_t;
typedef signed short        int16_t;
typedef signed int          int32_t;
typedef signed long long    int64_t;
typedef unsigned int        size_t;
typedef unsigned int        uintptr_t;

#define NULL   ((void*)0)
#define TRUE   1
#define FALSE  0

/* Panic declaration */
void kernel_panic(const char *msg);
