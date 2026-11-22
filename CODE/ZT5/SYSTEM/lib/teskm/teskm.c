/*
 * ZratroOS Task Manager (teskm)
 *
 * Process management, system monitoring, and network testing
 */

#include "task.h"
#include "console.h"
#include "string.h"
#include "e1000.h"
#include "net.h"
#include "teskm.h"

/* ===== System Information ===== */

void teskm_show_sysinfo(void) {
    printk("\n========== ZratroOS System Info ==========\n");
    printk("Kernel:    Ztknl (Main C Kernel)\n");
    printk("Self-Heal: rustkernel (Rust)\n");
    printk("TaskMgr:   teskm\n");
    printk("Processes: %u\n", get_process_count());
    printk("Threads:   %u\n", get_thread_count());
    printk("\n");
}

/* ===== Process List ===== */

void teskm_list_tasks(void) {
    printk("\n========== Process List ==========\n");
    printk("PID\tPPID\tSTATE\t\tTHREADS\tNAME\n");
    printk("------------------------------------------------\n");

    /* Iterate processes via process_get_by_pid */
    uint32_t total = get_process_count();
    for (uint32_t pid = 0; pid < MAX_PROCESSES && total > 0; pid++) {
        struct process *p = process_get_by_pid(pid);
        if (!p || p->state == PROCESS_STATE_UNUSED)
            continue;

        const char *state_str;
        switch (p->state) {
            case PROCESS_STATE_RUNNING:    state_str = "RUNNING";   break;
            case PROCESS_STATE_ZOMBIE:     state_str = "ZOMBIE";    break;
            case PROCESS_STATE_TERMINATED: state_str = "TERM";      break;
            default:                       state_str = "???";       break;
        }

        printk("%u\t%u\t%s\t\t%u\t%s\n",
               p->pid, p->parent_pid, state_str,
               p->thread_count, p->name[0] ? p->name : "(unnamed)");
        total--;
    }

    printk("\nThread detail:\n");
    printk("TID\tPID\tSTATE\t\tPRIO\tPOLICY\tNAME\n");
    printk("--------------------------------------------------------\n");

    /* Show thread info from current process and its threads */
    struct process *cur = process_get_current();
    if (cur && cur->threads) {
        struct thread *t = cur->threads;
        while (t) {
            const char *tstate;
            switch (t->state) {
                case THREAD_STATE_READY:      tstate = "READY";     break;
                case THREAD_STATE_RUNNING:    tstate = "RUNNING";   break;
                case THREAD_STATE_BLOCKED:    tstate = "BLOCKED";   break;
                case THREAD_STATE_SLEEPING:   tstate = "SLEEPING";  break;
                case THREAD_STATE_TERMINATED: tstate = "TERM";      break;
                default:                      tstate = "UNUSED";    break;
            }

            const char *policy;
            switch (t->sched_policy) {
                case SCHED_NORMAL:  policy = "NORMAL";  break;
                case SCHED_FIFO:    policy = "FIFO";    break;
                case SCHED_RR:      policy = "RR";      break;
                case SCHED_BATCH:   policy = "BATCH";   break;
                case SCHED_IDLE:    policy = "IDLE";    break;
                case SCHED_DEADLINE:policy = "DEADLINE";break;
                default:            policy = "?";       break;
            }

            printk("%u\t%u\t%s\t\t%d\t%s\t%s\n",
                   t->tid, t->pid, tstate,
                   t->priority, policy,
                   t->name[0] ? t->name : "-");
            t = t->process_next;
        }
    }
}

/* ===== Kill Process ===== */

void teskm_kill_task(uint32_t pid) {
    if (pid == 0) {
        printk("Error: Invalid PID 0\n");
        return;
    }

    struct process *p = process_get_by_pid(pid);
    if (!p || p->state == PROCESS_STATE_UNUSED) {
        printk("Error: Process %u not found\n", pid);
        return;
    }

    if (pid == 1) {
        printk("Error: Cannot kill init (PID 1)\n");
        return;
    }

    printk("Terminating process %u (%s)...\n", pid, p->name);
    process_exit(-1);
}

/* ===== Network Status ===== */

void teskm_network_status(void) {
    printk("\n========== Network Status ==========\n");

    /* Check e1000 adapter */
    extern struct e1000_adapter e1000_adapter;

    if (e1000_adapter.mmio_virt != 0) {
        printk("e1000: Intel Gigabit Ethernet\n");
        printk("  Device ID: 0x%04x\n", e1000_adapter.device_id);
        printk("  PCI: %02x:%02x.%x  IRQ: %u\n",
               e1000_adapter.pci_bus, e1000_adapter.pci_dev,
               e1000_adapter.pci_func, e1000_adapter.irq_line);
        printk("  MAC: %02x:%02x:%02x:%02x:%02x:%02x\n",
               e1000_adapter.mac.addr[0], e1000_adapter.mac.addr[1],
               e1000_adapter.mac.addr[2], e1000_adapter.mac.addr[3],
               e1000_adapter.mac.addr[4], e1000_adapter.mac.addr[5]);
        printk("  Link: %s\n", e1000_adapter.link_up ? "UP" : "DOWN");
        printk("  Speed: %d Mbps\n", e1000_adapter.speed_mbps);
        printk("  Full Duplex: %s\n", e1000_adapter.full_duplex ? "Yes" : "No");

        printk("\nStatistics:\n");
        printk("  TX packets: %lu  bytes: %lu  errors: %lu\n",
               e1000_adapter.tx_packets, e1000_adapter.tx_bytes,
               e1000_adapter.tx_errors);
        printk("  RX packets: %lu  bytes: %lu  errors: %lu\n",
               e1000_adapter.rx_packets, e1000_adapter.rx_bytes,
               e1000_adapter.rx_errors);

        printk("\nRing buffers:\n");
        printk("  TX: next_use=%u next_clean=%u (size=%d)\n",
               e1000_adapter.tx_next_to_use,
               e1000_adapter.tx_next_to_clean,
               E1000_TX_RING_SIZE);
        printk("  RX: next_use=%u next_clean=%u (size=%d)\n",
               e1000_adapter.rx_next_to_use,
               e1000_adapter.rx_next_to_clean,
               E1000_RX_RING_SIZE);
    } else {
        printk("e1000: Not initialized\n");
        printk("  Check: PCI bus scan, MMIO mapping\n");
    }
}

/* ===== Network Connectivity Test ===== */

void teskm_net_test(void) {
    printk("\n========== Network Test ==========\n");

    extern struct e1000_adapter e1000_adapter;

    /* Test 1: Adapter presence */
    if (e1000_adapter.mmio_virt == 0) {
        printk("[FAIL] e1000 adapter not found\n");
        return;
    }
    printk("[PASS] e1000 adapter present\n");

    /* Test 2: Link status */
    if (!e1000_adapter.link_up) {
        printk("[WARN] Link is DOWN - check cable/QEMU config\n");
        printk("  QEMU: use -netdev user,id=net0 -device e1000,netdev=net0\n");
    } else {
        printk("[PASS] Link is UP (%d Mbps, %s)\n",
               e1000_adapter.speed_mbps,
               e1000_adapter.full_duplex ? "Full Duplex" : "Half Duplex");
    }

    /* Test 3: MAC address */
    if (e1000_adapter.mac.addr[0] | e1000_adapter.mac.addr[1] |
        e1000_adapter.mac.addr[2] | e1000_adapter.mac.addr[3] |
        e1000_adapter.mac.addr[4] | e1000_adapter.mac.addr[5]) {
        printk("[PASS] MAC address assigned\n");
    } else {
        printk("[FAIL] MAC address is zero\n");
    }

    /* Test 4: TX/RX ring allocation */
    if (e1000_adapter.tx_ring && e1000_adapter.rx_ring) {
        printk("[PASS] TX/RX rings allocated\n");
    } else {
        printk("[FAIL] Ring buffer allocation failed\n");
    }

    /* Test 5: DHCP status */
    extern uint32_t dhcp_ip_address;
    if (dhcp_ip_address != 0) {
        printk("[PASS] DHCP: IP %d.%d.%d.%d\n",
               (dhcp_ip_address >> 0) & 0xFF,
               (dhcp_ip_address >> 8) & 0xFF,
               (dhcp_ip_address >> 16) & 0xFF,
               (dhcp_ip_address >> 24) & 0xFF);
    } else {
        printk("[WARN] No DHCP address - run 'dhcp' to request\n");
    }

    printk("\n===================================\n");
}

/* ===== Ping Test ===== */

void teskm_ping_test(const char *ip_str) {
    printk("\nPING %s (4 packets)\n", ip_str);
    printk("Note: Ping requires ICMP stack - use nettest for basic connectivity check\n");

    /* Basic connectivity: check if link is up */
    extern struct e1000_adapter e1000_adapter;
    if (!e1000_adapter.link_up) {
        printk("Error: Link is DOWN - cannot ping\n");
        return;
    }

    if (e1000_adapter.rx_packets == 0 && e1000_adapter.tx_packets == 0) {
        printk("Warning: No packets transmitted yet - network may not be configured\n");
    } else {
        printk("Network appears active (TX:%lu RX:%lu)\n",
               e1000_adapter.tx_packets, e1000_adapter.rx_packets);
    }
}

/* ===== Driver Status ===== */

void teskm_driver_status(void) {
    printk("\n========== Driver Status ==========\n");

    /* e1000 network */
    extern struct e1000_adapter e1000_adapter;
    printk("e1000 (Ethernet): %s\n",
           e1000_adapter.mmio_virt ? "LOADED" : "NOT FOUND");

    /* Display basic driver info */
    printk("\nLoaded drivers:\n");
    printk("  console    - VGA text mode console\n");
    printk("  keyboard   - PS/2 keyboard\n");
    printk("  e1000      - Intel Gigabit Ethernet\n");
    printk("  pci        - PCI bus driver\n");
    printk("  apic       - Advanced PIC\n");
    printk("  pit/hpet   - Timer\n");
    printk("  pmm/vmm    - Memory management\n");
}

/* ===== Helpers ===== */

static int teskm_atoi(const char *s) {
    int n = 0;
    while (*s >= '0' && *s <= '9')
        n = n * 10 + (*s++ - '0');
    return n;
}

/* ===== Main Entry ===== */

void teskm_init(void) {
    printk("[TESKM] Task Manager initialized\n");
}

void teskm_run(void) {
    printk("\n======= ZratroOS Task Manager =======\n");
    printk("  info     System information\n");
    printk("  ps       List processes/threads\n");
    printk("  kill PID Kill process\n");
    printk("  net      Network status\n");
    printk("  nettest  Network connectivity test\n");
    printk("  driver   Driver status\n");
    printk("  ping IP  Ping test\n");
    printk("  quit     Exit\n");
    printk("======================================\n\n");

    char cmd[128];
    while (1) {
        printk("teskm> ");
        if (!kgets(cmd, sizeof(cmd)))
            continue;

        char *argv[4];
        int argc = kstr_split(cmd, argv, 4, " \t");
        if (argc == 0) continue;

        if (strcmp(argv[0], "info") == 0) {
            teskm_show_sysinfo();
        } else if (strcmp(argv[0], "ps") == 0) {
            teskm_list_tasks();
        } else if (strcmp(argv[0], "kill") == 0 && argc >= 2) {
            teskm_kill_task((uint32_t)teskm_atoi(argv[1]));
        } else if (strcmp(argv[0], "net") == 0) {
            teskm_network_status();
        } else if (strcmp(argv[0], "nettest") == 0) {
            teskm_net_test();
        } else if (strcmp(argv[0], "driver") == 0 || strcmp(argv[0], "drivers") == 0) {
            teskm_driver_status();
        } else if (strcmp(argv[0], "ping") == 0 && argc >= 2) {
            teskm_ping_test(argv[1]);
        } else if (strcmp(argv[0], "quit") == 0 || strcmp(argv[0], "exit") == 0) {
            break;
        } else {
            printk("Unknown command: %s\n", argv[0]);
        }
    }

    printk("[TESKM] Exiting\n");
}
