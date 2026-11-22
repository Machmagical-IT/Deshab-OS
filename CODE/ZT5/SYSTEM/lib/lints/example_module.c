/*
 * ZTS Example Module - Simple Test
 *
 * This is a minimal ZTS module demonstrating the adaptation layer.
 * Compile with: gcc -nostdlib -fPIC -shared -O2 -I ../include -o example.zts example_module.c
 */

#include "zts_adapt.h"

/* Module private data */
static int example_counter = 0;
static char *example_buffer = NULL;

/*
 * Initialization function
 * Called when the module is loaded by ZTS7
 */
static int example_init(void) {
    /* Allocate some memory */
    example_buffer = (char *)kzalloc(256);
    if (!example_buffer) {
        printk("[ZTS:EXAMPLE] Failed to allocate buffer");
        return -ENOMEM;
    }
    
    /* Initialize counter */
    example_counter = 0;
    
    printk("[ZTS:EXAMPLE] Module initialized successfully");
    printk("[ZTS:EXAMPLE] Buffer allocated at: %p", example_buffer);
    
    return 0;
}

/*
 * Execute function
 * Called when the module is executed
 */
static int example_exec(void *input, void *output, void *ctx) {
    example_counter++;
    
    printk("[ZTS:EXAMPLE] Execution #%d", example_counter);
    printk("[ZTS:EXAMPLE] Input: %p, Output: %p, Context: %p", input, output, ctx);
    
    /* Write result to output buffer */
    if (output) {
        char *out = (char *)output;
        int len = __builtin_snprintf(out, 256, 
            "ZTS Example Module - Run #%d", example_counter);
        printk("[ZTS:EXAMPLE] Output written: %d bytes", len);
    }
    
    return example_counter;
}

/*
 * Cleanup function
 * Called when the module is unloaded
 */
static void example_cleanup(void) {
    if (example_buffer) {
        kfree(example_buffer);
        example_buffer = NULL;
    }
    
    printk("[ZTS:EXAMPLE] Module cleaned up");
    printk("[ZTS:EXAMPLE] Final counter value: %d", example_counter);
}

/* Export module entry points */
ZTS_MODULE_INIT(example_init)
ZTS_MODULE_EXEC(example_exec)
ZTS_MODULE_CLEANUP(example_cleanup)
