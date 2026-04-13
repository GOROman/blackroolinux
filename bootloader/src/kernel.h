/*
 * kernel.h — Kernel loading and launching for Blackroo bootloader
 * Attribution: New Blackroo work (2026, GPL v2)
 */
#ifndef BLACKROO_KERNEL_H
#define BLACKROO_KERNEL_H

#include <stdint.h>

/* PS-EXE header */
typedef struct {
    char     magic[8];      /* "PS-X EXE" */
    uint8_t  pad1[8];
    uint32_t pc0;           /* Entry point */
    uint32_t gp0;           /* GP register */
    uint32_t t_addr;        /* Load address */
    uint32_t t_size;        /* Data size */
    uint8_t  pad2[16];
    uint32_t sp;            /* Stack pointer */
} psexe_header_t;

/* Configure hardware and launch kernel at entry point */
void kernel_launch(uint32_t entry_point, uint32_t ram_kb,
                   const char *cmdline);

/* Load PS-EXE from memory buffer, returns entry point */
uint32_t kernel_load_psexe(const void *data, uint32_t size);

/* Boot kernel from CD-ROM ISO9660 filesystem */
/*
 * Boot profiles for cdrom_boot().
 *
 * Booting a kernel from the disc needs root=, init= and console= to agree with
 * WHICH kernel is being booted, and getting that wrong fails quietly rather
 * than loudly - pick KERNEL.EXE with root=/dev/psxcd and the built-in ramdisk
 * simply wins, and you are left wondering why /etc/release says ram0. So the
 * menu entry carries the settings instead of the user carrying them: each
 * profile is the one known-good combination for that path.
 *
 * A custom command line in System Settings still overrides everything, which
 * is the escape hatch for debugging.
 */
#define CDBOOT_PROFILE_SETTINGS  (-1)  /* whatever System Settings says */
#define CDBOOT_PROFILE_CDROOT      0   /* LINUX.EXE  + root on this disc */
#define CDBOOT_PROFILE_RAMDISK     1   /* KERNEL.EXE + its built-in ramdisk */

void cdrom_boot(const char *filename, int profile);

#endif
