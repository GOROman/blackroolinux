/*
 * addpsexe_initrd.c - Append initrd to a PS-EXE file
 *
 * The Linux kernel for PlayStation looks for the initrd at a page-aligned
 * boundary after the kernel image. The initrd is prefixed with:
 *   - Magic: 0x494E5244 ("INRD")
 *   - Size: 32-bit little-endian size of initrd data
 *
 * This tool appends the initrd to an existing PS-EXE and updates
 * the size field in the PS-EXE header.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/stat.h>

#define PSEXE_HEADER_SIZE 2048
#define PAGE_SIZE 4096
#define PAGE_MASK (PAGE_SIZE - 1)

/* PS-EXE header - only fields we need */
typedef struct {
    char     magic[8];          /* "PS-X EXE" */
    uint8_t  pad1[8];           /* Zero */
    uint32_t pc0;               /* Entry point */
    uint32_t gp0;               /* GP */
    uint32_t t_addr;            /* Load address */
    uint32_t t_size;            /* Data size (after header) */
    /* Rest of header follows... */
} PSEXE_HDR_PARTIAL;

void die(const char *msg) {
    perror(msg);
    exit(1);
}

int main(int argc, char **argv) {
    FILE *fin_exe, *fin_initrd, *fout;
    struct stat st_exe, st_initrd;
    uint8_t *exe_data, *initrd_data, *output;
    uint32_t exe_size, initrd_size;
    uint32_t kernel_end, initrd_offset;
    uint32_t initrd_header[2];
    uint32_t new_t_size;
    PSEXE_HDR_PARTIAL *hdr;

    if (argc != 4 && argc != 5) {
        fprintf(stderr, "Usage: %s <input.exe> <initrd.img> <output.exe> [kernel_end]\n", argv[0]);
        fprintf(stderr, "  kernel_end: the kernel's _end symbol (e.g. 0x8016acd0).\n");
        fprintf(stderr, "  Without it, placement is based on t_addr+t_size, which is\n");
        fprintf(stderr, "  the LOADED image and excludes BSS — the kernel searches\n");
        fprintf(stderr, "  after _end, and head.S zeroes BSS at startup, so an initrd\n");
        fprintf(stderr, "  placed in that gap is both missed and erased.\n");
        fprintf(stderr, "\nAppends initrd to a PS-EXE kernel image.\n");
        fprintf(stderr, "The initrd is placed at a page-aligned boundary after the kernel.\n");
        exit(1);
    }

    /* Get file sizes */
    if (stat(argv[1], &st_exe) < 0) die("Cannot stat PS-EXE file");
    if (stat(argv[2], &st_initrd) < 0) die("Cannot stat initrd file");

    exe_size = st_exe.st_size;
    initrd_size = st_initrd.st_size;

    printf("Input PS-EXE: %s (%u bytes)\n", argv[1], exe_size);
    printf("Input initrd: %s (%u bytes)\n", argv[2], initrd_size);

    /* Read PS-EXE */
    exe_data = malloc(exe_size);
    if (!exe_data) die("Cannot allocate memory for PS-EXE");

    fin_exe = fopen(argv[1], "rb");
    if (!fin_exe) die("Cannot open PS-EXE file");
    if (fread(exe_data, 1, exe_size, fin_exe) != exe_size)
        die("Cannot read PS-EXE file");
    fclose(fin_exe);

    /* Verify PS-EXE magic */
    if (memcmp(exe_data, "PS-X EXE", 8) != 0) {
        fprintf(stderr, "Error: Input is not a valid PS-EXE file\n");
        exit(1);
    }

    hdr = (PSEXE_HDR_PARTIAL *)exe_data;
    printf("\nPS-EXE header:\n");
    printf("  Load address: 0x%08x\n", hdr->t_addr);
    printf("  Entry point:  0x%08x\n", hdr->pc0);
    printf("  Data size:    %u bytes\n", hdr->t_size);

    /*
     * Where does the kernel actually end in memory?
     *
     * t_addr + t_size is the end of the LOADED image: text and data only.
     * The kernel's own search (arch/mipsnommu/kernel/setup.c) looks for the
     * INRD magic after the `_end` symbol, which sits past BSS — and head.S
     * clears BSS on entry. Place the initrd by the image size and it lands
     * inside BSS: wiped at boot, and looked for in the wrong place anyway.
     *
     * So take _end from the caller when given (build.sh reads it out of the
     * ELF); fall back to the old behaviour otherwise.
     */
    kernel_end = hdr->t_addr + hdr->t_size;
    printf("  Image end:    0x%08x  (t_addr + t_size)\n", kernel_end);

    if (argc == 5) {
        uint32_t sym_end = (uint32_t)strtoul(argv[4], NULL, 0);

        if (sym_end < 0x80000000)
            sym_end |= 0x80000000;      /* accept a bare physical _end */

        if (sym_end > kernel_end) {
            printf("  BSS gap:      %u bytes\n", sym_end - kernel_end);
            kernel_end = sym_end;
        }
    }
    printf("  Kernel end:   0x%08x\n", kernel_end);

    /* Initrd must be at page-aligned boundary - 8 (for header) */
    initrd_offset = ((kernel_end + PAGE_SIZE - 1) & ~PAGE_MASK) - 8;
    if (initrd_offset < kernel_end)
        initrd_offset += PAGE_SIZE;

    printf("\nInitrd placement:\n");
    printf("  Offset in memory: 0x%08x\n", initrd_offset);
    printf("  Offset in file:   %u (0x%x)\n",
           PSEXE_HEADER_SIZE + (initrd_offset - hdr->t_addr),
           PSEXE_HEADER_SIZE + (initrd_offset - hdr->t_addr));

    /* Read initrd */
    initrd_data = malloc(initrd_size);
    if (!initrd_data) die("Cannot allocate memory for initrd");

    fin_initrd = fopen(argv[2], "rb");
    if (!fin_initrd) die("Cannot open initrd file");
    if (fread(initrd_data, 1, initrd_size, fin_initrd) != initrd_size)
        die("Cannot read initrd file");
    fclose(fin_initrd);

    /* Prepare initrd header */
    initrd_header[0] = 0x494E5244;  /* "INRD" magic */
    initrd_header[1] = initrd_size;

    /*
     * Padding runs from the end of the data actually in the file to where the
     * initrd goes. That is NOT measured from kernel_end: once kernel_end is
     * the _end symbol it sits past BSS, which occupies no file space, so
     * measuring from it under-counts the padding by the size of BSS and the
     * output buffer overflows.
     */
    uint32_t image_end = hdr->t_addr + hdr->t_size;
    uint32_t padding_size = initrd_offset - image_end;
    new_t_size = hdr->t_size + padding_size + 8 + initrd_size;

    /* Pad to 2048 byte boundary */
    new_t_size = (new_t_size + 2047) & ~2047;

    printf("\nOutput layout:\n");
    printf("  Padding:          %u bytes\n", padding_size);
    printf("  Initrd header:    8 bytes\n");
    printf("  Initrd data:      %u bytes\n", initrd_size);
    printf("  New data size:    %u bytes (padded)\n", new_t_size);

    /* Allocate output buffer */
    uint32_t output_size = PSEXE_HEADER_SIZE + new_t_size;
    output = calloc(1, output_size);
    if (!output) die("Cannot allocate output buffer");

    /* Copy PS-EXE header */
    memcpy(output, exe_data, PSEXE_HEADER_SIZE);

    /* Update t_size in header */
    PSEXE_HDR_PARTIAL *out_hdr = (PSEXE_HDR_PARTIAL *)output;
    out_hdr->t_size = new_t_size;

    /* Copy original kernel data */
    memcpy(output + PSEXE_HEADER_SIZE, exe_data + PSEXE_HEADER_SIZE, hdr->t_size);

    /* Add initrd header and data at the right offset */
    uint32_t file_offset = PSEXE_HEADER_SIZE + (initrd_offset - hdr->t_addr);
    memcpy(output + file_offset, initrd_header, 8);
    memcpy(output + file_offset + 8, initrd_data, initrd_size);

    /* Write output file */
    fout = fopen(argv[3], "wb");
    if (!fout) die("Cannot create output file");
    if (fwrite(output, 1, output_size, fout) != output_size)
        die("Cannot write output file");
    fclose(fout);

    printf("\nCreated: %s (%u bytes)\n", argv[3], output_size);
    printf("\nInitrd will be found by kernel at runtime:\n");
    printf("  Header at: 0x%08x\n", initrd_offset);
    printf("  Data at:   0x%08x\n", initrd_offset + 8);
    printf("  End at:    0x%08x\n", initrd_offset + 8 + initrd_size);

    free(exe_data);
    free(initrd_data);
    free(output);

    return 0;
}
