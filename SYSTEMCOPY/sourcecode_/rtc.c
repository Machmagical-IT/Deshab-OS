/* ===========================================================================
 *  Nutro+OS - CMOS real time clock
 * ===========================================================================
 */
#include "kernel.h"

static u8 cmos_read(u8 reg)
{
    outb(0x70, reg);
    io_wait();
    return inb(0x71);
}

static int bcd_to_bin(u8 v) { return (v & 0x0F) + ((v >> 4) * 10); }

void rtc_read(rtc_time_t *t)
{
    while (cmos_read(0x0A) & 0x80) ;        /* wait for no update in progress */

    u8 sec = cmos_read(0x00), min = cmos_read(0x02), hour = cmos_read(0x04);
    u8 day = cmos_read(0x07), mon = cmos_read(0x08), year = cmos_read(0x09);
    u8 regb = cmos_read(0x0B);

    if (!(regb & 0x04)) {                   /* BCD encoded */
        sec  = (u8)bcd_to_bin(sec);
        min  = (u8)bcd_to_bin(min);
        hour = (u8)((hour & 0x80) | bcd_to_bin(hour & 0x7F));
        day  = (u8)bcd_to_bin(day);
        mon  = (u8)bcd_to_bin(mon);
        year = (u8)bcd_to_bin(year);
    }

    int pm = 0;
    if (!(regb & 0x02)) {                   /* 12 hour clock */
        pm = hour & 0x80;
        hour = (u8)(hour & 0x7F);
    }

    t->second = sec;
    t->minute = min;
    t->hour = (int)hour + (pm ? 12 : 0);
    t->day = day;
    t->month = mon;
    t->year = 2000 + year;
}