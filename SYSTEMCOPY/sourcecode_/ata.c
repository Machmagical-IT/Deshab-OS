/* ===========================================================================
 *  Nutro+OS - ATA PIO driver (primary channel, LBA28)
 * ===========================================================================
 */
#include "kernel.h"

#define ATA_DATA    0x1F0
#define ATA_SECCNT  0x1F2
#define ATA_LBA_LO  0x1F3
#define ATA_LBA_MID 0x1F4
#define ATA_LBA_HI  0x1F5
#define ATA_DRIVE   0x1F6
#define ATA_CMD     0x1F7
#define ATA_STATUS  0x1F7
#define ATA_ALT     0x3F6

#define ST_BSY 0x80
#define ST_DRQ 0x08
#define ST_ERR 0x01

static int present;

static void ata_400ns(void)
{
    for (int i = 0; i < 4; i++) inb(ATA_ALT);
}

/* wait until the drive is not busy (used before sending a command) */
static int ata_wait_not_busy(void)
{
    int timeout = 2000000;
    while (timeout-- > 0) {
        u8 st = inb(ATA_STATUS);
        if (!(st & ST_BSY)) return 0;
    }
    return -1;
}

/* wait for data (used after a command has been issued) */
static int ata_wait_drq(void)
{
    int timeout = 2000000;
    while (timeout-- > 0) {
        u8 st = inb(ATA_STATUS);
        if (st & (ST_ERR | 0x20)) return -1;        /* error / device fault */
        if (!(st & ST_BSY) && (st & ST_DRQ)) return 0;
    }
    return -1;
}

int ata_read_sectors(u32 lba, u32 count, void *buf)
{
    if (!present) return -1;
    u16 *p = (u16 *)buf;

    while (count) {
        if (ata_wait_not_busy() != 0) return -1;

        outb(ATA_DRIVE, (u8)(0x40 | ((lba >> 24) & 0x0F)));
        ata_400ns();
        outb(ATA_SECCNT, 1);
        outb(ATA_LBA_LO, (u8)(lba & 0xFF));
        outb(ATA_LBA_MID, (u8)((lba >> 8) & 0xFF));
        outb(ATA_LBA_HI, (u8)((lba >> 16) & 0xFF));
        outb(ATA_CMD, 0x20);                    /* read sectors */

        if (ata_wait_drq() != 0) return -1;
        for (int i = 0; i < 256; i++) *p++ = inw(ATA_DATA);
        ata_400ns();

        lba++;
        count--;
    }
    return 0;
}

int ata_write_sectors(u32 lba, u32 count, const void *buf)
{
    if (!present) return -1;
    const u16 *p = (const u16 *)buf;

    while (count) {
        if (ata_wait_not_busy() != 0) return -1;

        outb(ATA_DRIVE, (u8)(0x40 | ((lba >> 24) & 0x0F)));
        ata_400ns();
        outb(ATA_SECCNT, 1);
        outb(ATA_LBA_LO, (u8)(lba & 0xFF));
        outb(ATA_LBA_MID, (u8)((lba >> 8) & 0xFF));
        outb(ATA_LBA_HI, (u8)((lba >> 16) & 0xFF));
        outb(ATA_CMD, 0x30);                    /* write sectors */

        if (ata_wait_drq() != 0) return -1;
        for (int i = 0; i < 256; i++) outw(ATA_DATA, *p++);
        ata_400ns();
        outb(ATA_CMD, 0xE7);                    /* cache flush */

        lba++;
        count--;
    }
    return 0;
}

int ata_present(void) { return present; }

int ata_init(void)
{
    present = 0;
    outb(ATA_DRIVE, 0xA0);                      /* master */
    ata_400ns();

    outb(ATA_SECCNT, 0);
    outb(ATA_LBA_LO, 0);
    outb(ATA_LBA_MID, 0);
    outb(ATA_LBA_HI, 0);
    outb(ATA_CMD, 0xEC);                        /* identify */

    u8 st = inb(ATA_STATUS);
    if (st == 0) return -1;                     /* no drive */
    if (ata_wait_not_busy() != 0) return -1;
    if (inb(ATA_LBA_MID) || inb(ATA_LBA_HI)) return -1;   /* not ATA */

    if (ata_wait_drq() != 0) return -1;
    for (int i = 0; i < 256; i++) inw(ATA_DATA);          /* discard identify data */

    present = 1;
    debug_write("ata: primary master ready\n");
    return 0;
}