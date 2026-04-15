#ifndef RTC_H
#define RTC_H

#include "kernel.h"

typedef struct {
    uint8_t second;
    uint8_t minute;
    uint8_t hour;
    uint8_t day;
    uint8_t month;
    uint16_t year;
} rtc_time_t;

/* Read current time from CMOS RTC */
void rtc_read(rtc_time_t *t);

/* Format time as "HH:MM:SS" into buf (must be >= 9 bytes) */
void rtc_format_time(const rtc_time_t *t, char *buf);

/* Format date as "YYYY-MM-DD" into buf (must be >= 11 bytes) */
void rtc_format_date(const rtc_time_t *t, char *buf);

#endif /* RTC_H */
