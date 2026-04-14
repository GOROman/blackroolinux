/*
 * mkmemcard.c — Create PlayStation 1 memory card images for Blackroo Linux
 *
 * Generates a raw 128KB .mcd file that:
 *   - Has the Blackroo header in block 0 (BU_ID=0x1234)
 *   - Can optionally contain an ext2 filesystem starting at block 8
 *   - Works in DuckStation and other PS1 emulators
 *   - Works with the bu.c kernel driver on real hardware
 *
 * Usage:
 *   mkmemcard output.mcd                        # Empty Blackroo card
 *   mkmemcard output.mcd 0                      # Card 0 in RAID set
 *   mkmemcard output.mcd 0 filesystem.img       # Card with filesystem data
 *   mkmemcard --raid 4 prefix                   # Create 4-card RAID set
 *
 * File format: Raw 131072 bytes (1024 blocks x 128 bytes)
 *   Block 0: Blackroo header (bu_first_block_t)
 *   Blocks 1-7: Reserved (BU_FIRST_BLOCKS = 8)
 *   Blocks 8-1023: Filesystem data
 *
 * Attribution: New Blackroo work (2026, GPL v2)
 * Based on bu.h data structures from Runix project
 * Reference: psx-spx Controllers and Memory Cards
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>

#define BLOCK_SIZE      128
#define CARD_BLOCKS     1024
#define CARD_SIZE       (CARD_BLOCKS * BLOCK_SIZE)  /* 131072 = 128KB */
#define FIRST_BLOCKS    8       /* Reserved blocks (from bu.h BU_FIRST_BLOCKS) */
#define DATA_START      (FIRST_BLOCKS * BLOCK_SIZE) /* Byte offset where data starts */
#define BU_ID           0x1234

/* Matches bu_first_block_t in bu.h */
typedef struct {
    uint32_t id;        /* Must be 0x1234 (BU_ID) */
    uint32_t size;      /* Card size in 128-byte blocks (1024) */
    uint32_t serial;    /* Unique serial number */
    uint32_t number;    /* Card sequence in RAID (0, 1, 2, ...) */
    uint8_t  pad[112];  /* Pad to 128 bytes */
} __attribute__((packed)) blackroo_header_t;

static uint32_t make_serial(void) {
    srand(time(NULL) ^ getpid());
    return (uint32_t)rand() ^ ((uint32_t)rand() << 16);
}

static int create_card(const char *filename, int card_number,
                       const char *data_file) {
    FILE *fout;
    blackroo_header_t hdr;
    uint8_t card[CARD_SIZE];

    /* Zero the card */
    memset(card, 0, CARD_SIZE);

    /* Write Blackroo header to block 0 */
    memset(&hdr, 0, sizeof(hdr));
    hdr.id     = BU_ID;             /* 0x1234 */
    hdr.size   = CARD_BLOCKS;       /* 1024 blocks */
    hdr.serial = make_serial();
    hdr.number = card_number;

    memcpy(card, &hdr, sizeof(hdr));

    /* If a data file is provided, copy it starting at block 8 */
    if (data_file) {
        FILE *fdata;
        struct stat st;
        size_t max_data = CARD_SIZE - DATA_START;
        size_t read_size;

        if (stat(data_file, &st) < 0) {
            perror("Cannot stat data file");
            return 1;
        }

        if ((size_t)st.st_size > max_data) {
            fprintf(stderr, "Warning: data file (%ld bytes) exceeds card "
                    "data area (%zu bytes), truncating\n",
                    (long)st.st_size, max_data);
        }

        fdata = fopen(data_file, "rb");
        if (!fdata) {
            perror("Cannot open data file");
            return 1;
        }

        read_size = fread(card + DATA_START, 1, max_data, fdata);
        fclose(fdata);

        printf("  Data: %zu bytes from %s\n", read_size, data_file);
    }

    /* Write the card image */
    fout = fopen(filename, "wb");
    if (!fout) {
        perror("Cannot create output file");
        return 1;
    }

    if (fwrite(card, 1, CARD_SIZE, fout) != CARD_SIZE) {
        perror("Cannot write card image");
        fclose(fout);
        return 1;
    }

    fclose(fout);

    printf("Created: %s (128KB)\n", filename);
    printf("  ID:     0x%04x\n", hdr.id);
    printf("  Size:   %u blocks\n", hdr.size);
    printf("  Serial: 0x%08x\n", hdr.serial);
    printf("  Number: %u (card %u in RAID set)\n", hdr.number, hdr.number);
    printf("  Data:   blocks %d-%d (%d KB usable)\n",
           FIRST_BLOCKS, CARD_BLOCKS - 1,
           (CARD_BLOCKS - FIRST_BLOCKS) * BLOCK_SIZE / 1024);

    return 0;
}

static void usage(const char *prog) {
    fprintf(stderr, "mkmemcard — Create PS1 memory card images for Blackroo Linux\n");
    fprintf(stderr, "\nUsage:\n");
    fprintf(stderr, "  %s <output.mcd> [card_number] [data_file]\n", prog);
    fprintf(stderr, "  %s --raid <count> <prefix>\n", prog);
    fprintf(stderr, "\nExamples:\n");
    fprintf(stderr, "  %s card0.mcd              # Empty card, number 0\n", prog);
    fprintf(stderr, "  %s card0.mcd 0 fs.img     # Card 0 with filesystem\n", prog);
    fprintf(stderr, "  %s --raid 4 blackroo       # Create blackroo_0.mcd .. blackroo_3.mcd\n", prog);
    fprintf(stderr, "\nOutput: Raw 131072-byte .mcd files for DuckStation/real hardware\n");
    fprintf(stderr, "Card format: Blackroo header (block 0) + data (blocks 8-1023)\n");
}

int main(int argc, char **argv) {
    if (argc < 2) {
        usage(argv[0]);
        return 1;
    }

    /* RAID mode: create multiple cards */
    if (strcmp(argv[1], "--raid") == 0) {
        int count, i;
        char filename[256];

        if (argc < 4) {
            fprintf(stderr, "Usage: %s --raid <count> <prefix>\n", argv[0]);
            return 1;
        }

        count = atoi(argv[2]);
        if (count < 1 || count > 8) {
            fprintf(stderr, "Card count must be 1-8\n");
            return 1;
        }

        printf("Creating %d-card RAID set:\n\n", count);
        for (i = 0; i < count; i++) {
            snprintf(filename, sizeof(filename), "%s_%d.mcd", argv[3], i);
            if (create_card(filename, i, NULL) != 0)
                return 1;
            printf("\n");
        }

        printf("RAID set created: %d cards, %d KB total usable\n",
               count, count * (CARD_BLOCKS - FIRST_BLOCKS) * BLOCK_SIZE / 1024);
        printf("\nTo use in DuckStation:\n");
        printf("  Settings > Memory Cards > Card 1 > Browse > select %s_0.mcd\n", argv[3]);
        printf("  Settings > Memory Cards > Card 2 > Browse > select %s_1.mcd\n", argv[3]);
        return 0;
    }

    /* Single card mode */
    {
        const char *output = argv[1];
        int card_num = 0;
        const char *data_file = NULL;

        if (argc >= 3) card_num = atoi(argv[2]);
        if (argc >= 4) data_file = argv[3];

        return create_card(output, card_num, data_file);
    }
}
