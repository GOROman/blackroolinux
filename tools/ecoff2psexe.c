/*
 * ecoff2psexe.c - Convert ECOFF executable to PlayStation PS-EXE format
 *
 * PS-EXE Header format (2048 bytes):
 *   0x000: "PS-X EXE" signature (8 bytes)
 *   0x008: Zero padding (8 bytes)
 *   0x010: Initial PC (entry point)
 *   0x014: Initial GP (usually 0)
 *   0x018: Load address (destination in RAM)
 *   0x01C: File size (excluding header, must be multiple of 2048)
 *   0x020: Data section start
 *   0x024: Data section size
 *   0x028: BSS section start
 *   0x02C: BSS section size
 *   0x030: Initial SP base
 *   0x034: Initial SP offset (added to base)
 *   0x038-0x04B: Reserved (zeroed)
 *   0x04C: ASCII marker (optional, like region)
 *   0x800: Start of executable data
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* ECOFF definitions */
typedef struct {
    uint16_t f_magic;
    uint16_t f_nscns;
    int32_t  f_timdat;
    int32_t  f_symptr;
    int32_t  f_nsyms;
    uint16_t f_opthdr;
    uint16_t f_flags;
} FILHDR;

typedef struct {
    int16_t  magic;
    int16_t  vstamp;
    int32_t  tsize;
    int32_t  dsize;
    int32_t  bsize;
    int32_t  entry;
    int32_t  text_start;
    int32_t  data_start;
    int32_t  bss_start;
    int32_t  gprmask;
    int32_t  cprmask[4];
    int32_t  gp_value;
} AOUTHDR;

typedef struct {
    char     s_name[8];
    int32_t  s_paddr;
    int32_t  s_vaddr;
    int32_t  s_size;
    int32_t  s_scnptr;
    int32_t  s_relptr;
    int32_t  s_lnnoptr;
    uint16_t s_nreloc;
    uint16_t s_nlnno;
    int32_t  s_flags;
} SCNHDR;

#define MIPSELMAGIC 0x162

/* PS-EXE header */
typedef struct {
    char     magic[8];          /* "PS-X EXE" */
    uint32_t pad1[2];           /* Zero */
    uint32_t pc0;               /* Initial PC */
    uint32_t gp0;               /* Initial GP */
    uint32_t t_addr;            /* Text load address */
    uint32_t t_size;            /* Text size (data after header) */
    uint32_t d_addr;            /* Data section address */
    uint32_t d_size;            /* Data section size */
    uint32_t b_addr;            /* BSS address */
    uint32_t b_size;            /* BSS size */
    uint32_t sp_base;           /* Stack pointer base */
    uint32_t sp_offset;         /* Stack pointer offset */
    uint32_t reserved[5];       /* Reserved */
    char     marker[1972];      /* ASCII marker / padding to 2048 */
} PSEXE_HDR;

void die(const char *msg) {
    perror(msg);
    exit(1);
}

int main(int argc, char **argv) {
    FILE *in, *out;
    FILHDR fhdr;
    AOUTHDR ahdr;
    SCNHDR *shdr;
    PSEXE_HDR psexe;
    unsigned char *data;
    int32_t text_offset, data_size;
    int32_t padded_size;
    int i;

    if (argc != 3) {
        fprintf(stderr, "Usage: %s <input.ecoff> <output.exe>\n", argv[0]);
        fprintf(stderr, "Converts ECOFF executable to PlayStation PS-EXE format\n");
        exit(1);
    }

    in = fopen(argv[1], "rb");
    if (!in) die("Cannot open input file");

    /* Read ECOFF file header */
    if (fread(&fhdr, sizeof(fhdr), 1, in) != 1)
        die("Cannot read file header");

    if (fhdr.f_magic != MIPSELMAGIC) {
        fprintf(stderr, "Error: Not a MIPS little-endian ECOFF file (magic=0x%04x)\n", fhdr.f_magic);
        exit(1);
    }

    /* Read a.out header */
    if (fread(&ahdr, sizeof(ahdr), 1, in) != 1)
        die("Cannot read a.out header");

    printf("ECOFF file info:\n");
    printf("  Entry point: 0x%08x\n", ahdr.entry);
    printf("  Text start:  0x%08x, size: %d\n", ahdr.text_start, ahdr.tsize);
    printf("  Data start:  0x%08x, size: %d\n", ahdr.data_start, ahdr.dsize);
    printf("  BSS start:   0x%08x, size: %d\n", ahdr.bss_start, ahdr.bsize);
    printf("  Sections:    %d\n", fhdr.f_nscns);

    /* Read section headers */
    shdr = malloc(fhdr.f_nscns * sizeof(SCNHDR));
    if (!shdr) die("Cannot allocate section headers");

    if (fread(shdr, sizeof(SCNHDR), fhdr.f_nscns, in) != fhdr.f_nscns)
        die("Cannot read section headers");

    /* Find text section offset and total data size */
    text_offset = 0;
    data_size = 0;
    for (i = 0; i < fhdr.f_nscns; i++) {
        printf("  Section %d: %.8s addr=0x%08x size=%d fileoff=%d\n",
               i, shdr[i].s_name, shdr[i].s_vaddr, shdr[i].s_size, shdr[i].s_scnptr);
        if (shdr[i].s_scnptr > 0) {
            if (text_offset == 0 || shdr[i].s_scnptr < text_offset)
                text_offset = shdr[i].s_scnptr;
            if (shdr[i].s_scnptr + shdr[i].s_size > data_size)
                data_size = shdr[i].s_scnptr + shdr[i].s_size - text_offset;
        }
    }

    /* Calculate total data to copy (text + data sections) */
    data_size = ahdr.tsize + ahdr.dsize;

    /* Pad to 2048 byte boundary */
    padded_size = (data_size + 2047) & ~2047;

    printf("\nPS-EXE output:\n");
    printf("  Load address: 0x%08x\n", ahdr.text_start);
    printf("  Entry point:  0x%08x\n", ahdr.entry);
    printf("  Data size:    %d bytes (padded to %d)\n", data_size, padded_size);

    /* Prepare PS-EXE header */
    memset(&psexe, 0, sizeof(psexe));
    memcpy(psexe.magic, "PS-X EXE", 8);
    psexe.pc0 = ahdr.entry;
    psexe.gp0 = ahdr.gp_value;
    psexe.t_addr = ahdr.text_start;
    psexe.t_size = padded_size;
    psexe.d_addr = ahdr.data_start;
    psexe.d_size = ahdr.dsize;
    psexe.b_addr = ahdr.bss_start;
    psexe.b_size = ahdr.bsize;
    psexe.sp_base = 0x801fff00;  /* Default stack for PS1 */
    psexe.sp_offset = 0;

    /* Add region marker (optional) */
    strcpy(psexe.marker, "Sony Computer Entertainment Inc. for North America area");

    /* Read executable data */
    data = calloc(1, padded_size);
    if (!data) die("Cannot allocate data buffer");

    fseek(in, text_offset, SEEK_SET);
    if (fread(data, 1, data_size, in) != data_size) {
        fprintf(stderr, "Warning: Could not read all data (%d bytes)\n", data_size);
    }
    fclose(in);

    /* Write PS-EXE file */
    out = fopen(argv[2], "wb");
    if (!out) die("Cannot create output file");

    if (fwrite(&psexe, sizeof(psexe), 1, out) != 1)
        die("Cannot write PS-EXE header");

    if (fwrite(data, padded_size, 1, out) != 1)
        die("Cannot write executable data");

    fclose(out);
    free(data);
    free(shdr);

    printf("\nCreated: %s (%ld bytes)\n", argv[2], sizeof(psexe) + padded_size);

    return 0;
}
