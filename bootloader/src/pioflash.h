/*
 * pioflash.h — PIO Expansion Port Flash Manager
 *
 * Detect, dump, and program NOR flash on PS1 cheat cartridges
 * (GameShark, Action Replay, Xplorer, Caetla) plugged into
 * the parallel I/O expansion port at 0x1F000000.
 *
 * Attribution: New Blackroo work (2026, GPL v2)
 */
#ifndef BLACKROO_PIOFLASH_H
#define BLACKROO_PIOFLASH_H

#include <stdint.h>

/* Expansion Region 1 base (PIO port) */
#define PIO_FLASH_BASE  ((volatile uint8_t *)0x1F000000)

/* NOR flash AMD/SST command addresses (byte-wide bus) */
#define FLASH_CMD_ADDR1  0x5555
#define FLASH_CMD_ADDR2  0x2AAA

/* NOR flash commands (AMD/SST compatible) */
#define FLASH_CMD_UNLOCK1    0xAA
#define FLASH_CMD_UNLOCK2    0x55
#define FLASH_CMD_AUTOSEL    0x90   /* JEDEC ID / autoselect */
#define FLASH_CMD_RESET      0xF0   /* Return to read mode */
#define FLASH_CMD_PROGRAM    0xA0   /* Byte program */
#define FLASH_CMD_ERASE_PRE  0x80   /* Erase setup */
#define FLASH_CMD_ERASE_CHIP 0x10   /* Chip erase */
#define FLASH_CMD_ERASE_SECT 0x30   /* Sector erase */

/* Known flash chip database */
typedef struct {
    uint8_t  mfr_id;       /* JEDEC manufacturer ID */
    uint8_t  dev_id;       /* JEDEC device ID */
    const char *mfr_name;  /* Manufacturer string */
    const char *dev_name;  /* Device string */
    uint32_t size;         /* Total size in bytes */
    uint32_t sector_size;  /* Erase sector size in bytes */
} flash_chip_info_t;

/* Detected flash state */
typedef struct {
    int      present;      /* 1 if flash detected */
    uint8_t  mfr_id;       /* Raw manufacturer ID */
    uint8_t  dev_id;       /* Raw device ID */
    const flash_chip_info_t *chip;  /* Known chip info (NULL if unknown) */
    uint32_t size;         /* Flash size in bytes */
    uint32_t sector_size;  /* Sector size in bytes */
} pioflash_info_t;

/* Serial transfer protocol magic */
#define PIOFLASH_MAGIC  0x4C464B42  /* "BKFL" little-endian */

/* Transfer header sent over serial */
typedef struct {
    uint32_t magic;        /* PIOFLASH_MAGIC */
    uint32_t size;         /* Data size in bytes */
    uint8_t  mfr_id;
    uint8_t  dev_id;
    uint8_t  reserved[6];
} __attribute__((packed)) pioflash_xfer_hdr_t;

/*
 * Detect flash chip at PIO port.
 * Returns 1 if flash found, 0 if port empty.
 */
int pioflash_detect(pioflash_info_t *info);

/*
 * Read bytes from flash into buffer.
 * Trivial — flash is memory-mapped, just copies bytes.
 */
void pioflash_read(uint32_t offset, uint8_t *buf, uint32_t len);

/*
 * Erase entire flash chip.
 * Returns 1 on success, 0 on timeout.
 */
int pioflash_erase_chip(const pioflash_info_t *info);

/*
 * Erase a single sector.
 * Returns 1 on success, 0 on timeout.
 */
int pioflash_erase_sector(uint32_t sector_addr);

/*
 * Program a single byte. Assumes target byte is erased (0xFF).
 * Returns 1 on success, 0 on timeout.
 */
int pioflash_program_byte(uint32_t offset, uint8_t data);

/*
 * Program a buffer into flash. Erases sectors as needed.
 * progress_cb called every 1KB with bytes written so far.
 * Returns 1 on success, 0 on failure.
 */
int pioflash_program(const pioflash_info_t *info,
                     uint32_t offset, const uint8_t *buf, uint32_t len,
                     void (*progress_cb)(uint32_t done, uint32_t total));

/*
 * Dump flash contents over serial link.
 * Sends: BKFL header (16 bytes) + raw flash data.
 */
void pioflash_dump_serial(const pioflash_info_t *info);

/*
 * Receive data over serial and program into flash.
 * Expects: BKFL header (16 bytes) + raw data.
 * Returns 1 on success, 0 on failure.
 */
int pioflash_load_serial(const pioflash_info_t *info);

/*
 * Verify flash contents against a buffer.
 * Returns number of mismatched bytes (0 = perfect).
 */
uint32_t pioflash_verify(uint32_t offset, const uint8_t *buf, uint32_t len);

/*
 * Interactive PIO flash manager menu (called from main menu).
 */
void pioflash_manager_menu(void);

#endif
