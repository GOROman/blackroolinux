/*
 * memcard.h — Memory card operations for Blackroo bootloader
 * Attribution: New Blackroo work (2026, GPL v2)
 */
#ifndef BLACKROO_MEMCARD_H
#define BLACKROO_MEMCARD_H

#include <stdint.h>

/* Blackroo card header (matches kernel bu.h bu_first_block_t) */
#define BU_ID           0x1234
#define BU_FIRST_BLOCKS 8
#define BU_BLOCK_SIZE   128
#define BU_CARD_BLOCKS  1024

/*
 * Slot numbering, identical to the kernel's bu.c and BRMON:
 *
 *     slot = (port << 2) | floor          0..7
 *
 * A multitap addresses its four floors by the ADDRESS BYTE - 0x81..0x84 for
 * memory cards, 0x01..0x04 for controllers - not by anything in the control
 * register. Selecting the port alone reaches floor A and nothing else, which
 * is why this used to see only two cards. See GR-013.
 */
#define MC_SLOTS        8
#define MC_PORT(s)      (((s) >> 2) & 1)
#define MC_FLOOR(s)     ((s) & 3)

typedef struct {
    uint32_t id;        /* Must be BU_ID (0x1234) */
    uint32_t size;      /* Card size in blocks (1024) */
    uint32_t serial;    /* Unique serial number */
    uint32_t number;    /* Sequence in RAID (0, 1, ...) */
} blackroo_card_header_t;

void memcard_set_pad_active(int active);  /* Call after StartPAD() */

/* In every call below, `slot` is 0..7 as described above - NOT a port. */
int  memcard_detect(int slot);  /* 0=not present, 1=present */
int  memcard_read_block(int slot, int block, uint8_t *buf);
int  memcard_write_block(int slot, int block, const uint8_t *buf);
int  memcard_format(int slot, int sequence_number);
int  memcard_is_blackroo(int slot);
int  memcard_read_header(int slot, blackroo_card_header_t *hdr);
void memcard_manager_menu(void);
const char *memcard_slot_name(int slot);  /* "port 1 A" and so on */

#endif
