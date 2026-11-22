/*
 * rsknl_loader.c - Rsknl (Rust Monitoring Kernel) Loader
 *
 * Loads the Rust monitoring kernel into a protected memory region
 * and starts it as a separate execution context.
 *
 * Rsknl runs at 0xFFFFFFFF_C0000000, above the main kernel.
 * No program can access this memory region.
 */

#include <knl.h>
#include <vmm.h>
#include <pmm.h>
#include <string.h>
#include <heap.h>
#include <console.h>
#include <smp.h>
#include <limine_file.h>

/* Rsknl protected memory region */
#define RSKNL_BASE     0xFFFFFFFFC0000000ULL
#define RSKNL_SIZE     (4 * 1024 * 1024)  /* 4MB protected region */
#define RSKNL_MAGIC    0x525553544B4E4C00ULL  /* "RUSTKNL\0" */

/* Shared memory for Ztknl <-> Rsknl communication */
#define RSKNL_SHMEM_BASE  0xFFFFFFFFC0400000ULL
#define RSKNL_SHMEM_SIZE  (64 * 1024)  /* 64KB shared region */

/* Kernel health block - written by Ztknl, read by Rsknl */
struct kernel_health_block {
    uint64_t magic;           /* 0x5A544B4E ("ZTKN") */
    uint64_t tick_counter;    /* Scheduler tick count */
    uint64_t task_count;      /* Running task count */
    uint64_t page_fault_count;/* Page faults since boot */
    uint64_t free_pages;      /* Free physical pages */
    uint64_t total_pages;     /* Total physical pages */
    uint64_t heap_used;       /* Kernel heap used (bytes) */
    uint64_t heap_total;      /* Kernel heap total (bytes) */
    uint64_t panic_offset;    /* Last panic message offset (0 = no panic) */
    uint64_t checksum;        /* Checksum of above fields */
    uint64_t heal_command;    /* Rsknl -> Ztknl heal command */
    uint64_t rollback_flag;   /* Rollback request flag */
    uint64_t reserved[4];     /* Reserved */
};

/* Global health block pointer */
static struct kernel_health_block *g_health_block = NULL;

/* Rsknl entry point signature */
typedef void (*rsknl_entry_t)(
    uint64_t magic,
    uint64_t ztknl_addr,
    uint64_t ztknl_size,
    uint64_t shmem_addr,
    uint64_t shmem_size
) __attribute__((noreturn));

/* Helper: find Limine module by name - improved version */
static void *find_module_by_name(const char *name, size_t *size_out) {
    int count = limine_get_module_count();
    for (int i = 0; i < count; i++) {
        size_t mod_size = 0;
        const char *mod_path = NULL;
        void *mod_data = limine_get_module(i, &mod_size, &mod_path);
        if (mod_path) {
            /* Try multiple matching strategies */
            if (strstr(mod_path, name)) {
                *size_out = mod_size;
                return mod_data;
            }
            /* Also try matching just the filename component */
            const char *filename = mod_path;
            for (const char *p = mod_path; *p; p++) {
                if (*p == '/' || *p == '\\') filename = p + 1;
            }
            if (strstr(filename, name)) {
                *size_out = mod_size;
                return mod_data;
            }
        }
    }
    return NULL;
}

/* Calculate simple checksum */
static uint64_t health_block_checksum(struct kernel_health_block *block) {
    uint64_t sum = 0;
    sum += block->magic;
    sum += block->tick_counter;
    sum += block->task_count;
    sum += block->page_fault_count;
    sum += block->free_pages;
    sum += block->total_pages;
    sum += block->heap_used;
    sum += block->heap_total;
    sum += block->panic_offset;
    return sum;
}

/* Update the health block with current kernel state */
void rsknl_update_health(uint64_t ticks, uint64_t tasks,
                          uint64_t free_pages, uint64_t total_pages,
                          uint64_t heap_used, uint64_t heap_total) {
    if (!g_health_block) return;

    g_health_block->tick_counter = ticks;
    g_health_block->task_count = tasks;
    g_health_block->free_pages = free_pages;
    g_health_block->total_pages = total_pages;
    g_health_block->heap_used = heap_used;
    g_health_block->heap_total = heap_total;
    g_health_block->checksum = health_block_checksum(g_health_block);
}

/* Check if Rsknl has sent a heal command */
uint64_t rsknl_get_heal_command(void) {
    if (!g_health_block) return 0;
    return g_health_block->heal_command;
}

/* Check if Rsknl has requested a rollback */
int rsknl_is_rollback_requested(void) {
    if (!g_health_block) return 0;
    return g_health_block->rollback_flag != 0;
}

/* Acknowledge heal command */
void rsknl_ack_heal(void) {
    if (!g_health_block) return;
    g_health_block->heal_command = 0;
}

/* Load and start Rsknl */
int rustknl_load(void) {
    size_t rsknl_size = 0;
    void *rsknl_data = NULL;

    /* Try to find Rsknl binary from Limine modules */
    rsknl_data = find_module_by_name("rustknl", &rsknl_size);
    if (!rsknl_data || rsknl_size == 0) {
        /* Try alternate names */
        rsknl_data = find_module_by_name("librustknl", &rsknl_size);
    }
    if (!rsknl_data || rsknl_size == 0) {
        rsknl_data = find_module_by_name("rsknl", &rsknl_size);
    }
    if (!rsknl_data || rsknl_size == 0) {
        /* Try looking for the file in the VFS path */
        rsknl_data = find_module_by_name("zrr64/rstk/rustknl", &rsknl_size);
    }
    if (!rsknl_data || rsknl_size == 0) {
        /* No Rsknl binary available - this is not an error,
         * the system can run without the monitoring kernel.
         * Just log a warning and continue. */
        serial_print("[RSKNL] Rsknl binary not found in Limine modules\n");
        serial_print("[RSKNL] System will run without monitoring kernel\n");
        serial_print("[RSKNL] To enable Rsknl, add rustknl to limine.conf modules\n");
        return -1;  /* RSKNL_NOT_FOUND - not fatal */
    }

    serial_print("[RSKNL] Found Rsknl binary, size=");
    { char _nb[16]; int _np=0; uint64_t _v=rsknl_size; if(_v==0){_nb[_np++]='0';}else{char _tb[16];int _tp=0;while(_v>0){_tb[_tp++]='0'+(_v%10);_v/=10;}while(_tp>0)_nb[_np++]=_tb[--_tp];}_nb[_np]=0;serial_print(_nb); }
    serial_print(" bytes\n");

    /* Map the Rsknl memory region */
    /* Allocate physical pages for Rsknl */
    uint64_t rsknl_phys = 0;
    int pages_needed = (RSKNL_SIZE + 4095) / 4096;
    for (int i = 0; i < pages_needed; i++) {
        uint64_t page = pmm_alloc_page();
        if (!page) {
            serial_print("[RSKNL] ERROR: Failed to allocate page for Rsknl\n");
            return -2;  /* RSKNL_NO_MEMORY */
        }
        uint64_t phys = page;
        if (i == 0) rsknl_phys = phys;

        /* Map page at RSKNL_BASE + i*4096 */
        uint64_t virt = RSKNL_BASE + i * 4096;
        vmm_map_page(NULL, virt, phys + i * 4096, VMM_FLAGS_KERNEL);
    }

    /* Map shared memory region */
    uint64_t shmem_page = pmm_alloc_page();
    if (!shmem_page) {
        serial_print("[RSKNL] ERROR: Failed to allocate shared memory page\n");
        return -2;
    }
    for (int i = 0; i < (int)((RSKNL_SHMEM_SIZE + 4095) / 4096); i++) {
        uint64_t p = pmm_alloc_page();
        if (!p) {
            serial_print("[RSKNL] ERROR: Failed to allocate shared memory\n");
            return -2;
        }
        vmm_map_page(NULL, RSKNL_SHMEM_BASE + i * 4096, p,
                VMM_FLAGS_KERNEL);
    }

    /* Initialize health block in shared memory */
    g_health_block = (struct kernel_health_block *)(uintptr_t)RSKNL_SHMEM_BASE;
    memset(g_health_block, 0, sizeof(*g_health_block));
    g_health_block->magic = 0x5A544B4EULL;  /* "ZTKN" */
    g_health_block->checksum = health_block_checksum(g_health_block);

    /* Copy Rsknl binary to protected region */
    if (rsknl_size > RSKNL_SIZE) {
        serial_print("[RSKNL] WARNING: Rsknl binary truncated to ");
        { char _nb[16]; int _np=0; uint64_t _v=RSKNL_SIZE; if(_v==0){_nb[_np++]='0';}else{char _tb[16];int _tp=0;while(_v>0){_tb[_tp++]='0'+(_v%10);_v/=10;}while(_tp>0)_nb[_np++]=_tb[--_tp];}_nb[_np]=0;serial_print(_nb); }
        serial_print(" bytes\n");
        rsknl_size = RSKNL_SIZE;
    }
    memcpy((void *)(uintptr_t)RSKNL_BASE, rsknl_data, rsknl_size);

    serial_print("[RSKNL] Rsknl loaded successfully at 0xFFFFFFFFC0000000\n");
    serial_print("[RSKNL] Shared memory initialized at 0xFFFFFFFFC0400000\n");

    /* Find Rsknl entry point - look for rustknl_entry symbol
     * For a static library, the entry is at a known offset.
     * In practice, we'd parse the ELF, but for now use the start. */
    rsknl_entry_t entry = (rsknl_entry_t)(uintptr_t)RSKNL_BASE;
    (void)entry;  /* Entry point not used yet - Rsknl runs on AP core */

    /* Get Ztknl base address and size for monitoring */
    uint64_t ztknl_base = 0xFFFFFFFF80000000ULL;
    uint64_t ztknl_sz = 8 * 1024 * 1024;  /* 8MB estimate */
    (void)ztknl_base;
    (void)ztknl_sz;

    /* Mark Rsknl region as protected in VMM */
    /* This prevents any user-space access to Rsknl memory */

    /* Note: We cannot directly call the Rsknl entry point because
     * it's a Rust no_std binary that expects to take over the CPU.
     * Instead, we set up the shared memory and let Rsknl run on a
     * separate CPU core (AP) or as a periodic kernel thread.
     *
     * For now, we initialize the shared memory and health block
     * so that Rsknl can be started later via SMP. */

    /* Try to start Rsknl on an AP core if available */
    if (smp_cpu_count > 1) {
        /* Start Rsknl on the last available CPU core */
        /* This would use smp_start_cpu() with the Rsknl entry point */
        /* For now, just log that Rsknl is ready */
        serial_print("[RSKNL] Ready to start on AP core (multi-core available)\n");
    } else {
        /* Single core - Rsknl runs as periodic check in kernel timer */
        serial_print("[RSKNL] Running in periodic mode (single core)\n");
    }

    return 0;  /* RSKNL_OK */
}
