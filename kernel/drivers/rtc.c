/*
 * rtc.c — CMOS Real-Time Clock reader
 *
 * Reads the RTC registers via CMOS I/O ports (0x70 index / 0x71 data).
 * Handles both BCD and binary-encoded CMOS chips, and 12/24-hour mode.
 * Waits for an update-not-in-progress window before sampling registers.
 */

#include "rtc.h"
#include "kernel.h"

static inline uint8_t inb(uint16_t port)
{
    uint8_t ret;
    __asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}
static inline void outb(uint16_t port, uint8_t val)
{
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

#define CMOS_ADDR 0x70
#define CMOS_DATA 0x71

/* CMOS register indices */
#define CMOS_SECONDS 0x00
#define CMOS_MINUTES 0x02
#define CMOS_HOURS   0x04
#define CMOS_DAY     0x07
#define CMOS_MONTH   0x08
#define CMOS_YEAR    0x09
#define CMOS_CENTIRY 0x32   /* century register (may not exist) */
#define CMOS_STATUS_A 0x0A
#define CMOS_STATUS_B 0x0B

/* Status B flags */
#define CMOS_B_24HR   0x02  /* 24-hour mode    */
#define CMOS_B_BINARY 0x04  /* binary (not BCD)*/

static uint8_t cmos_read(uint8_t reg)
{
    outb(CMOS_ADDR, reg & 0x7F);   /* NMI bit clear */
    return inb(CMOS_DATA);
}

static void wait_no_update(void)
{
    /* Poll status A "update in progress" bit (bit 7) */
    int retries = 10000;
    while (retries-- && (cmos_read(CMOS_STATUS_A) & 0x80));
}

static uint8_t bcd_to_bin(uint8_t bcd)
{
    return (uint8_t)(((bcd >> 4) & 0x0F) * 10 + (bcd & 0x0F));
}

void rtc_read(rtc_time_t *t)
{
    wait_no_update();

    uint8_t status_b = cmos_read(CMOS_STATUS_B);
    int is_binary = (status_b & CMOS_B_BINARY) ? 1 : 0;
    int is_24hr   = (status_b & CMOS_B_24HR)   ? 1 : 0;

    uint8_t sec   = cmos_read(CMOS_SECONDS);
    uint8_t min   = cmos_read(CMOS_MINUTES);
    uint8_t hour  = cmos_read(CMOS_HOURS);
    uint8_t day   = cmos_read(CMOS_DAY);
    uint8_t month = cmos_read(CMOS_MONTH);
    uint8_t year8 = cmos_read(CMOS_YEAR);
    uint8_t cent  = cmos_read(CMOS_CENTIRY);

    if (!is_binary) {
        sec   = bcd_to_bin(sec);
        min   = bcd_to_bin(min);
        /* 12-hour: top bit of hour is PM flag in BCD */
        uint8_t pm = (hour & 0x80) ? 1 : 0;
        hour  = bcd_to_bin(hour & 0x7F);
        if (!is_24hr) {
            hour = hour % 12;
            if (pm) hour += 12;
        }
        day   = bcd_to_bin(day);
        month = bcd_to_bin(month);
        year8 = bcd_to_bin(year8);
        cent  = bcd_to_bin(cent);
    } else if (!is_24hr) {
        uint8_t pm = (hour & 0x80) ? 1 : 0;
        hour &= 0x7F;
        hour  = hour % 12;
        if (pm) hour += 12;
    }

    /* Determine century */
    uint16_t full_year;
    if (cent >= 20 && cent <= 21) {
        full_year = (uint16_t)(cent * 100 + year8);
    } else {
        /* Fallback: assume 1970-2069 window */
        full_year = (uint16_t)((year8 >= 70 ? 1900 : 2000) + year8);
    }

    t->second = sec;
    t->minute = min;
    t->hour   = hour;
    t->day    = day;
    t->month  = month;
    t->year   = full_year;
}

/* ------------------------------------------------------------------ */
/* Simple formatting helpers                                           */
/* ------------------------------------------------------------------ */
static void fmt2(char *p, uint8_t v)
{
    p[0] = '0' + (v / 10);
    p[1] = '0' + (v % 10);
}

void rtc_format_time(const rtc_time_t *t, char *buf)
{
    /* "HH:MM:SS\0" = 9 bytes */
    fmt2(buf + 0, t->hour);
    buf[2] = ':';
    fmt2(buf + 3, t->minute);
    buf[5] = ':';
    fmt2(buf + 6, t->second);
    buf[8] = '\0';
}

void rtc_format_date(const rtc_time_t *t, char *buf)
{
    /* "YYYY-MM-DD\0" = 11 bytes */
    uint16_t y = t->year;
    buf[0] = '0' + (y / 1000);
    buf[1] = '0' + (y / 100 % 10);
    buf[2] = '0' + (y / 10  % 10);
    buf[3] = '0' + (y       % 10);
    buf[4] = '-';
    fmt2(buf + 5, t->month);
    buf[7] = '-';
    fmt2(buf + 8, t->day);
    buf[10] = '\0';
}
