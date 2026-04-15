#pragma once

/* ============================================================================
 * PD-OS DE SDK  —  pdos_types.h
 * Primitive type definitions identical to the kernel's kernel.h.
 * Include this first if you aren't also including de_api.h.
 * ============================================================================ */

typedef unsigned char       uint8_t;
typedef unsigned short      uint16_t;
typedef unsigned int        uint32_t;
typedef unsigned long long  uint64_t;
typedef signed char         int8_t;
typedef signed short        int16_t;
typedef signed int          int32_t;
typedef signed long long    int64_t;
typedef unsigned int        size_t;

#define NULL  ((void*)0)
