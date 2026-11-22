/*
 * Zratro OS - VM-Exit Handler Header
 */

#ifndef VMEXIT_H
#define VMEXIT_H

#include <stdint.h>
#include <stddef.h>
#include "idt.h"

/* VM-Exit 统计信息 */
struct vmexit_stats {
    uint64_t total;
    uint64_t exceptions;
    uint64_t cpuid;
    uint64_t hlt;
    uint64_t io;
    uint64_t msr_read;
    uint64_t msr_write;
    uint64_t ept_violation;
    uint64_t invalid_state;
    uint64_t vmcall;
    uint64_t unhandled;
};

void vmexit_handler(struct interrupt_frame *frame);
void vmexit_get_stats(struct vmexit_stats *s);
void vmexit_print_stats(void);

#endif
