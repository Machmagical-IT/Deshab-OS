#ifndef KNL_H
#define KNL_H

/* Forward declarations only - include specific headers where needed */
#include <stdint.h>

/* Kernel entry point */
void kernel_main(void);

/* Boot diagnostics */
void boot_diag_init(void);
void boot_diag_print(const char *msg);

#endif /* KNL_H */
