/*
 * elf2psexe.c - Convert ELF executable directly to PlayStation PS-EXE format
 *
 * This converts a statically-linked MIPS little-endian ELF to PS-EXE
 * format suitable for running on the PlayStation 1.
 *
 * PS-EXE Header format (2048 bytes total):
 *   0x000: "PS-X EXE" signature (8 bytes)
 *   0x008: Zero padding (8 bytes)
 *   0x010: Initial PC (entry point)
 *   0x014: Initial GP (global pointer, usually 0)
 *   0x018: Text/load address (destination in RAM)
 *   0x01C: File size (excluding 2048-byte header, must be multiple of 2048)
 *   0x020: Data section start
 *   0x024: Data section size
 *   0x028: BSS section start
 *   0x02C: BSS section size
 *   0x030: Initial SP base (stack pointer)
 *   0x034: Initial SP offset
 *   0x038-0x04B: Reserved
 *   0x04C-0x7FF: ASCII marker / padding
 *   0x800: Start of executable code/data
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <elf.h>

void die(const char *msg) {
    perror(msg);
    exit(1);
}

/* PS-EXE header structure */
#pragma pack(push, 1)
typedef struct {
    char     magic[8];          /* "PS-X EXE" */
    uint8_t  pad1[8];           /* Zero */
    uint32_t pc0;               /* Initial PC (entry point) */
    uint32_t gp0;               /* Initial GP */
    uint32_t t_addr;            /* Text load address */
    uint32_t t_size;            /* Text size (file size after header) */
    uint32_t d_addr;            /* Data section address */
    uint32_t d_size;            /* Data section size */
    uint32_t b_addr;            /* BSS address */
    uint32_t b_size;            /* BSS size */
    uint32_t sp_base;           /* Stack pointer base */
    uint32_t sp_offset;         /* Stack pointer offset */
    uint8_t  reserved[20];      /* Reserved */
    char     marker[1972];      /* ASCII marker / padding to 2048 */
} PSEXE_HDR;
#pragma pack(pop)

int main(int argc, char **argv) {
    FILE *in, *out;
    Elf32_Ehdr ehdr;
    Elf32_Phdr *phdrs;
    PSEXE_HDR psexe;
    uint8_t *image = NULL;
    uint32_t image_size = 0;
    uint32_t load_addr = 0xFFFFFFFF;
    uint32_t end_addr = 0;
    uint32_t bss_addr = 0, bss_size = 0;
    uint32_t data_addr = 0, data_size = 0;
    uint32_t stack_top = 0x801fff00;   /* top of a stock 2 MB console */
    int i;

    if (argc < 3) {
        fprintf(stderr, "Usage: %s <input.elf> <output.exe> [stack_top]\n", argv[0]);
        fprintf(stderr, "\nConverts MIPS ELF to PlayStation PS-EXE format.\n");
        fprintf(stderr, "stack_top defaults to 0x801fff00 (2 MB console).\n");
        fprintf(stderr, "Pass 0x807fff00 for an 8 MB machine.\n");
        exit(1);
    }

    if (argc > 3)
        stack_top = (uint32_t)strtoul(argv[3], NULL, 0);

    in = fopen(argv[1], "rb");
    if (!in) die("Cannot open input ELF file");

    /* Read ELF header */
    if (fread(&ehdr, sizeof(ehdr), 1, in) != 1)
        die("Cannot read ELF header");

    /* Verify ELF magic */
    if (memcmp(ehdr.e_ident, ELFMAG, SELFMAG) != 0) {
        fprintf(stderr, "Error: Not an ELF file\n");
        exit(1);
    }

    /* Verify it's 32-bit little-endian MIPS */
    if (ehdr.e_ident[EI_CLASS] != ELFCLASS32) {
        fprintf(stderr, "Error: Not a 32-bit ELF\n");
        exit(1);
    }
    if (ehdr.e_ident[EI_DATA] != ELFDATA2LSB) {
        fprintf(stderr, "Error: Not little-endian\n");
        exit(1);
    }
    if (ehdr.e_machine != EM_MIPS) {
        fprintf(stderr, "Error: Not a MIPS ELF (machine=%d)\n", ehdr.e_machine);
        exit(1);
    }

    printf("ELF file info:\n");
    printf("  Entry point:    0x%08x\n", ehdr.e_entry);
    printf("  Program headers: %d\n", ehdr.e_phnum);

    /* Read program headers */
    phdrs = malloc(ehdr.e_phnum * sizeof(Elf32_Phdr));
    if (!phdrs) die("Cannot allocate program headers");

    fseek(in, ehdr.e_phoff, SEEK_SET);
    if (fread(phdrs, sizeof(Elf32_Phdr), ehdr.e_phnum, in) != ehdr.e_phnum)
        die("Cannot read program headers");

    /* Find load range and BSS */
    for (i = 0; i < ehdr.e_phnum; i++) {
        if (phdrs[i].p_type == PT_LOAD) {
            printf("  Segment %d: vaddr=0x%08x filesz=%d memsz=%d\n",
                   i, phdrs[i].p_vaddr, phdrs[i].p_filesz, phdrs[i].p_memsz);

            if (phdrs[i].p_vaddr < load_addr)
                load_addr = phdrs[i].p_vaddr;

            if (phdrs[i].p_vaddr + phdrs[i].p_filesz > end_addr)
                end_addr = phdrs[i].p_vaddr + phdrs[i].p_filesz;

            /* BSS is the difference between memsz and filesz */
            if (phdrs[i].p_memsz > phdrs[i].p_filesz) {
                bss_addr = phdrs[i].p_vaddr + phdrs[i].p_filesz;
                bss_size = phdrs[i].p_memsz - phdrs[i].p_filesz;
            }

            /* Track data section (writable segments) */
            if (phdrs[i].p_flags & PF_W) {
                data_addr = phdrs[i].p_vaddr;
                data_size = phdrs[i].p_filesz;
            }
        }
    }

    /* Calculate image size */
    image_size = end_addr - load_addr;

    printf("\nMemory layout:\n");
    printf("  Load address:   0x%08x\n", load_addr);
    printf("  End address:    0x%08x\n", end_addr);
    printf("  Image size:     %d bytes\n", image_size);
    printf("  BSS:            0x%08x (%d bytes)\n", bss_addr, bss_size);

    /* Allocate image buffer */
    image = calloc(1, image_size);
    if (!image) die("Cannot allocate image buffer");

    /* Load all PT_LOAD segments */
    for (i = 0; i < ehdr.e_phnum; i++) {
        if (phdrs[i].p_type == PT_LOAD && phdrs[i].p_filesz > 0) {
            uint32_t offset = phdrs[i].p_vaddr - load_addr;
            fseek(in, phdrs[i].p_offset, SEEK_SET);
            if (fread(image + offset, 1, phdrs[i].p_filesz, in) != phdrs[i].p_filesz) {
                fprintf(stderr, "Warning: Could not read segment %d\n", i);
            }
        }
    }
    fclose(in);

    /* Pad image to 2048 byte boundary */
    uint32_t padded_size = (image_size + 2047) & ~2047;
    if (padded_size > image_size) {
        image = realloc(image, padded_size);
        memset(image + image_size, 0, padded_size - image_size);
        image_size = padded_size;
    }

    /* Prepare PS-EXE header */
    memset(&psexe, 0, sizeof(psexe));
    memcpy(psexe.magic, "PS-X EXE", 8);

    /*
     * PlayStation uses KSEG0 addresses (0x80000000 base) for cached memory.
     * The ELF may use physical addresses (0x00010000) but PS-EXE needs
     * virtual KSEG0 addresses (0x80010000).
     */
    #define KSEG0_BASE 0x80000000
    #define TO_KSEG0(addr) (((addr) < KSEG0_BASE) ? ((addr) | KSEG0_BASE) : (addr))

    psexe.pc0 = TO_KSEG0(ehdr.e_entry);
    psexe.gp0 = 0;  /* No GP for kernel */
    psexe.t_addr = TO_KSEG0(load_addr);
    psexe.t_size = image_size;
    psexe.d_addr = TO_KSEG0(data_addr);
    psexe.d_size = data_size;
    psexe.b_addr = TO_KSEG0(bss_addr);
    psexe.b_size = bss_size;

    /*
     * Initial stack pointer.
     *
     * The BIOS uses this value when it loads the PS-EXE, so it has to be
     * real RAM on the target machine: 0x807fff00 is the top of an 8 MB
     * mod and is *not* addressable on a stock 2 MB console. Optional
     * third argument overrides it; default is the safe 2 MB top.
     */
    psexe.sp_base = stack_top;
    psexe.sp_offset = 0;

    /* Add region marker */
    strcpy(psexe.marker, "Blackroo Linux for PlayStation");

    printf("\nPS-EXE header:\n");
    printf("  Entry (PC0):    0x%08x\n", psexe.pc0);
    printf("  Load addr:      0x%08x\n", psexe.t_addr);
    printf("  Size:           %d bytes\n", psexe.t_size);
    printf("  Stack:          0x%08x\n", psexe.sp_base);

    /* Write PS-EXE file */
    out = fopen(argv[2], "wb");
    if (!out) die("Cannot create output file");

    if (fwrite(&psexe, sizeof(psexe), 1, out) != 1)
        die("Cannot write PS-EXE header");

    if (fwrite(image, image_size, 1, out) != 1)
        die("Cannot write image data");

    printf("\nCreated: %s (%ld bytes)\n", argv[2], sizeof(psexe) + image_size);

    fclose(out);
    free(image);
    free(phdrs);

    return 0;
}
