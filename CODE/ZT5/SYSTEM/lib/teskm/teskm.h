/*
 * ZratroOS Task Manager Header
 */

#ifndef TESKM_H
#define TESKM_H

#include <stdint.h>

/* Task manager functions */
void teskm_init(void);
void teskm_run(void);

/* System info */
void teskm_show_sysinfo(void);

/* Process management */
void teskm_list_tasks(void);
void teskm_kill_task(uint32_t pid);

/* Network testing */
void teskm_network_status(void);
void teskm_net_test(void);
void teskm_ping_test(const char *ip_str);

/* Driver status */
void teskm_driver_status(void);

#endif /* TESKM_H */
