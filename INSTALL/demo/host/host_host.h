/* host_host.h — 宿主演示构建（DESHAB_HOST）专用接口
 *
 * 由 CODE/desktop/desktop.h 在 -DDESHAB_HOST 时包含。
 * 裸机构建不会包含本文件，桌面原始行为零变化。
 */
#ifndef DESHAB_DEMO_HOST_HOST_H
#define DESHAB_DEMO_HOST_HOST_H

void host_outb(unsigned short port, unsigned char value);
unsigned char host_inb(unsigned short port);
void host_serial_char(char c);

void host_rtc_read(int *yy, int *mo, int *dd, int *hh, int *mi, int *ss);
int  host_rtc_day_sec(void);
unsigned long long host_tsc_calibrate(void);

void host_fatal_halt(const char *sym);
void host_demo_quit(void);
void host_demo_unapp(const char *what);

#endif /* DESHAB_DEMO_HOST_HOST_H */
