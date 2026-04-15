/* ============================================================================
 * PD-Kernel  —  ATA/IDE PIO Driver  (Phase 8a)
 *
 * 28-bit LBA PIO, primary channel, master drive, polling mode.
 *
 * Sector layout on the PD-OS disk image as of Phase 8:
 *   LBA  0        Stage 1 bootloader   (1 sector)
 *   LBA  1-5      Stage 2 bootloader   (5 sectors)
 *   LBA  6-68     Kernel image         (≤63 sectors @ 32 KB load window)
 *   LBA  69+      Available for data / future filesystem
 * ============================================================================ */

#include "ata.h"

/* ---- Port base addresses -------------------------------------------------- */
#define ATA_BASE  0x1F0u    /* primary channel data/register base             */
#define ATA_CTRL  0x3F6u    /* alternate status / device-control register     */

/* ---- Register offsets from ATA_BASE --------------------------------------- */
#define ATA_R_DATA      0   /* 16-bit data                                    */
#define ATA_R_ERROR     1   /* R: error register                              */
#define ATA_R_FEATURES  1   /* W: features register                           */
#define ATA_R_SECCOUNT  2   /* sector count                                   */
#define ATA_R_LBA_LO    3   /* LBA bits  0-7                                  */
#define ATA_R_LBA_MID   4   /* LBA bits  8-15                                 */
#define ATA_R_LBA_HI    5   /* LBA bits 16-23                                 */
#define ATA_R_DRVHD     6   /* drive/head + LBA24-27                          */
#define ATA_R_STATUS    7   /* R: status                                      */
#define ATA_R_CMD       7   /* W: command                                     */

/* ---- Status register bits ------------------------------------------------- */
#define ATA_SR_ERR   0x01u  /* error occurred                                 */
#define ATA_SR_DRQ   0x08u  /* data request (ready to transfer)               */
#define ATA_SR_DRDY  0x40u  /* drive ready                                    */
#define ATA_SR_BSY   0x80u  /* drive busy                                     */

/* ---- Commands ------------------------------------------------------------- */
#define ATA_CMD_READ_SECTORS  0x20u
#define ATA_CMD_WRITE_SECTORS 0x30u
#define ATA_CMD_CACHE_FLUSH   0xE7u
#define ATA_CMD_IDENTIFY      0xECu

/* ---- Poll timeout (spin iterations before giving up) ---------------------- */
#define ATA_TIMEOUT  0x100000u

/* ---- Module state --------------------------------------------------------- */
static ata_drive_t g_drive;

/* ---- Low-level I/O helpers ------------------------------------------------ */

static inline void outb(uint16_t port, uint8_t val)
{
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint8_t inb(uint16_t port)
{
    uint8_t ret;
    __asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static inline uint16_t inw(uint16_t port)
{
    uint16_t ret;
    __asm__ volatile ("inw %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static inline void outw(uint16_t port, uint16_t val)
{
    __asm__ volatile ("outw %0, %1" : : "a"(val), "Nd"(port));
}

/* Read the alternate status register 4× to introduce a 400 ns delay.
 * Required after writing to the drive/head select register.           */
static void ata_delay(void)
{
    inb(ATA_CTRL);
    inb(ATA_CTRL);
    inb(ATA_CTRL);
    inb(ATA_CTRL);
}

/* ---- Polling helpers ------------------------------------------------------ */

/* Wait until BSY clears.  Returns 0 on success, -1 on timeout. */
static int ata_wait_bsy(void)
{
    uint32_t i;
    for (i = 0; i < ATA_TIMEOUT; i++) {
        if (!(inb(ATA_BASE + ATA_R_STATUS) & ATA_SR_BSY))
            return 0;
    }
    return -1;
}

/* Wait until BSY clears AND DRQ sets.  Returns 0 on success, -1 on error. */
static int ata_wait_drq(void)
{
    uint32_t i;
    for (i = 0; i < ATA_TIMEOUT; i++) {
        uint8_t st = inb(ATA_BASE + ATA_R_STATUS);
        if (st & ATA_SR_ERR)  return -1;
        if ((st & ATA_SR_BSY) == 0 && (st & ATA_SR_DRQ)) return 0;
    }
    return -1;
}

/* ---- IDENTIFY ------------------------------------------------------------- */

/*
 * Extract 40-char (+ NUL) model name from the IDENTIFY buffer.
 * Each pair of bytes in the identify response is byte-swapped.
 */
static void extract_model(const uint16_t *id, char *out)
{
    int i;
    for (i = 0; i < 20; i++) {
        out[i * 2]     = (char)(id[27 + i] >> 8);
        out[i * 2 + 1] = (char)(id[27 + i] & 0xFF);
    }
    out[40] = '\0';

    /* Strip trailing spaces */
    int len = 40;
    while (len > 0 && out[len - 1] == ' ') out[--len] = '\0';
}

/* ---- Public API ----------------------------------------------------------- */

void ata_init(void)
{
    uint16_t id[256];
    int i;
    uint8_t st;

    /* Zero drive info */
    g_drive.present       = 0;
    g_drive.lba_supported = 0;
    g_drive.total_sectors = 0;
    for (i = 0; i < 41; i++) g_drive.model[i] = '\0';

    /* Software reset via device-control register */
    outb(ATA_CTRL, 0x04);  /* SRST bit */
    outb(ATA_CTRL, 0x00);  /* clear SRST */

    /* Select master drive, LBA mode */
    outb(ATA_BASE + ATA_R_DRVHD, 0xA0);
    ata_delay();

    /* Check for drive presence — status 0xFF means no device */
    st = inb(ATA_BASE + ATA_R_STATUS);
    if (st == 0xFF)
        return;

    /* Send IDENTIFY */
    outb(ATA_BASE + ATA_R_SECCOUNT, 0);
    outb(ATA_BASE + ATA_R_LBA_LO,  0);
    outb(ATA_BASE + ATA_R_LBA_MID, 0);
    outb(ATA_BASE + ATA_R_LBA_HI,  0);
    outb(ATA_BASE + ATA_R_CMD, ATA_CMD_IDENTIFY);

    /* If status is 0 after IDENTIFY, no drive */
    st = inb(ATA_BASE + ATA_R_STATUS);
    if (st == 0)
        return;

    /* Wait for BSY to clear */
    if (ata_wait_bsy() != 0)
        return;

    /* Check for ATAPI signature (non-ATA device) */
    {
        uint8_t mid = inb(ATA_BASE + ATA_R_LBA_MID);
        uint8_t hi  = inb(ATA_BASE + ATA_R_LBA_HI);
        if (mid != 0 || hi != 0)
            return;   /* ATAPI or other non-ATA device */
    }

    /* Wait for DRQ */
    if (ata_wait_drq() != 0)
        return;

    /* Read 256 words of IDENTIFY data */
    for (i = 0; i < 256; i++)
        id[i] = inw(ATA_BASE + ATA_R_DATA);

    /* Parse useful fields */
    g_drive.present       = 1;
    g_drive.lba_supported = (id[49] >> 9) & 1u;
    g_drive.total_sectors = ((uint32_t)id[61] << 16) | id[60];
    extract_model(id, g_drive.model);
}

const ata_drive_t *ata_get_drive(void)
{
    return &g_drive;
}

int ata_read_sectors(uint32_t lba, uint8_t count, void *buf)
{
    uint16_t *ptr = (uint16_t *)buf;
    uint8_t i;

    if (!g_drive.present)
        return -1;
    if (count == 0)
        return 0;

    /* Wait for drive to be ready */
    if (ata_wait_bsy() != 0)
        return -1;

    /* Set up LBA and sector count */
    outb(ATA_BASE + ATA_R_DRVHD,   0xE0 | ((lba >> 24) & 0x0Fu));  /* LBA mode, master, LBA[24-27] */
    outb(ATA_BASE + ATA_R_SECCOUNT, count);
    outb(ATA_BASE + ATA_R_LBA_LO,  (uint8_t)(lba & 0xFFu));
    outb(ATA_BASE + ATA_R_LBA_MID, (uint8_t)((lba >>  8) & 0xFFu));
    outb(ATA_BASE + ATA_R_LBA_HI,  (uint8_t)((lba >> 16) & 0xFFu));
    outb(ATA_BASE + ATA_R_CMD,     ATA_CMD_READ_SECTORS);

    /* Read each sector */
    for (i = 0; i < count; i++) {
        uint16_t j;
        if (ata_wait_drq() != 0)
            return -1;
        for (j = 0; j < 256; j++)
            *ptr++ = inw(ATA_BASE + ATA_R_DATA);
    }
    return 0;
}

/* LBAs 0-1023 hold the bootloader, stage2, and kernel image — never overwrite them. */
#define ATA_RESERVED_LBA 1024u

int ata_write_sectors(uint32_t lba, uint8_t count, const void *buf)
{
    const uint16_t *ptr = (const uint16_t *)buf;
    uint8_t i;

    if (!g_drive.present)
        return -1;
    if (count == 0)
        return 0;
    if (lba < ATA_RESERVED_LBA)
        return -1;   /* refuse writes to boot/kernel sectors */

    /* Wait for drive to be ready */
    if (ata_wait_bsy() != 0)
        return -1;

    /* Set up LBA and sector count */
    outb(ATA_BASE + ATA_R_DRVHD,   0xE0 | ((lba >> 24) & 0x0Fu));
    outb(ATA_BASE + ATA_R_SECCOUNT, count);
    outb(ATA_BASE + ATA_R_LBA_LO,  (uint8_t)(lba & 0xFFu));
    outb(ATA_BASE + ATA_R_LBA_MID, (uint8_t)((lba >>  8) & 0xFFu));
    outb(ATA_BASE + ATA_R_LBA_HI,  (uint8_t)((lba >> 16) & 0xFFu));
    outb(ATA_BASE + ATA_R_CMD,     ATA_CMD_WRITE_SECTORS);

    /* Write each sector, then flush cache */
    for (i = 0; i < count; i++) {
        uint16_t j;
        if (ata_wait_drq() != 0)
            return -1;
        for (j = 0; j < 256; j++)
            outw(ATA_BASE + ATA_R_DATA, *ptr++);
        /* Flush write cache after each sector */
        outb(ATA_BASE + ATA_R_CMD, ATA_CMD_CACHE_FLUSH);
        if (ata_wait_bsy() != 0)
            return -1;
    }
    return 0;
}

/* ============================================================================
 * Multi-drive support  (primary channel master + slave)
 * ============================================================================ */

static ata_drive_t g_drives[ATA_MAX_DRIVES];

/* Probe one drive (dev_idx 0=master, 1=slave) into g_drives[dev_idx]. */
static void ata_probe_one(int dev_idx)
{
    uint8_t  drv_sel = (dev_idx == 0) ? 0xA0u : 0xB0u;
    uint16_t id[256];
    int      i;
    uint8_t  st;

    g_drives[dev_idx].present       = 0;
    g_drives[dev_idx].lba_supported = 0;
    g_drives[dev_idx].total_sectors = 0;
    for (i = 0; i < 41; i++) g_drives[dev_idx].model[i] = '\0';

    if (ata_wait_bsy() != 0) return;

    outb(ATA_BASE + ATA_R_DRVHD, drv_sel);
    ata_delay();

    st = inb(ATA_BASE + ATA_R_STATUS);
    if (st == 0xFF) return;   /* no device */

    outb(ATA_BASE + ATA_R_SECCOUNT, 0);
    outb(ATA_BASE + ATA_R_LBA_LO,  0);
    outb(ATA_BASE + ATA_R_LBA_MID, 0);
    outb(ATA_BASE + ATA_R_LBA_HI,  0);
    outb(ATA_BASE + ATA_R_CMD, ATA_CMD_IDENTIFY);

    st = inb(ATA_BASE + ATA_R_STATUS);
    if (st == 0) return;

    if (ata_wait_bsy() != 0) return;

    {
        uint8_t mid = inb(ATA_BASE + ATA_R_LBA_MID);
        uint8_t hi  = inb(ATA_BASE + ATA_R_LBA_HI);
        if (mid != 0 || hi != 0) return;   /* ATAPI */
    }

    if (ata_wait_drq() != 0) return;

    for (i = 0; i < 256; i++)
        id[i] = inw(ATA_BASE + ATA_R_DATA);

    g_drives[dev_idx].present       = 1;
    g_drives[dev_idx].lba_supported = (id[49] >> 9) & 1u;
    g_drives[dev_idx].total_sectors = ((uint32_t)id[61] << 16) | id[60];
    extract_model(id, g_drives[dev_idx].model);
}

void ata_probe_all(void)
{
    outb(ATA_CTRL, 0x04);   /* SRST */
    outb(ATA_CTRL, 0x00);   /* clear SRST */
    ata_probe_one(0);
    ata_probe_one(1);
}

const ata_drive_t *ata_get_drive_n(int n)
{
    if (n < 0 || n >= ATA_MAX_DRIVES) return (void *)0;
    return &g_drives[n];
}

int ata_read_sectors_drv(int drv, uint32_t lba, uint8_t count, void *buf)
{
    uint16_t *ptr = (uint16_t *)buf;
    uint8_t   drv_head_base = (drv == 0) ? 0xE0u : 0xF0u;
    uint8_t   i;

    if (drv < 0 || drv >= ATA_MAX_DRIVES) return -1;
    if (!g_drives[drv].present) return -1;
    if (count == 0) return 0;

    if (ata_wait_bsy() != 0) return -1;

    outb(ATA_BASE + ATA_R_DRVHD,    drv_head_base | ((lba >> 24) & 0x0Fu));
    outb(ATA_BASE + ATA_R_SECCOUNT, count);
    outb(ATA_BASE + ATA_R_LBA_LO,   (uint8_t)(lba & 0xFFu));
    outb(ATA_BASE + ATA_R_LBA_MID,  (uint8_t)((lba >>  8) & 0xFFu));
    outb(ATA_BASE + ATA_R_LBA_HI,   (uint8_t)((lba >> 16) & 0xFFu));
    outb(ATA_BASE + ATA_R_CMD,      ATA_CMD_READ_SECTORS);

    for (i = 0; i < count; i++) {
        uint16_t j;
        if (ata_wait_drq() != 0) return -1;
        for (j = 0; j < 256; j++)
            *ptr++ = inw(ATA_BASE + ATA_R_DATA);
    }
    return 0;
}

/* Raw write — no LBA guard.  Intended for the installer only. */
int ata_write_sectors_raw(int drv, uint32_t lba, uint8_t count, const void *buf)
{
    const uint16_t *ptr = (const uint16_t *)buf;
    uint8_t drv_head_base = (drv == 0) ? 0xE0u : 0xF0u;
    uint8_t i;

    if (drv < 0 || drv >= ATA_MAX_DRIVES) return -1;
    if (!g_drives[drv].present) return -1;
    if (count == 0) return 0;

    if (ata_wait_bsy() != 0) return -1;

    outb(ATA_BASE + ATA_R_DRVHD,    drv_head_base | ((lba >> 24) & 0x0Fu));
    outb(ATA_BASE + ATA_R_SECCOUNT, count);
    outb(ATA_BASE + ATA_R_LBA_LO,   (uint8_t)(lba & 0xFFu));
    outb(ATA_BASE + ATA_R_LBA_MID,  (uint8_t)((lba >>  8) & 0xFFu));
    outb(ATA_BASE + ATA_R_LBA_HI,   (uint8_t)((lba >> 16) & 0xFFu));
    outb(ATA_BASE + ATA_R_CMD,      ATA_CMD_WRITE_SECTORS);

    for (i = 0; i < count; i++) {
        uint16_t j;
        if (ata_wait_drq() != 0) return -1;
        for (j = 0; j < 256; j++)
            outw(ATA_BASE + ATA_R_DATA, *ptr++);
        outb(ATA_BASE + ATA_R_CMD, ATA_CMD_CACHE_FLUSH);
        if (ata_wait_bsy() != 0) return -1;
    }
    return 0;
}
