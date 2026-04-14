/*
 * mkinit.c — Generate a BFLT (FLAT) init program for MIPS R3000
 *
 * Creates a /linuxrc that:
 *   1. Writes "Blackroo Linux booted!" to stdout
 *   2. Mounts /proc via mount() syscall
 *   3. Enters an infinite loop (init must never exit)
 *
 * This is the minimum viable init for a uClinux system without
 * a real shell. Once we have FLAT-format busybox, this gets replaced.
 *
 * FLAT header: see include/linux/flat.h
 * MIPS Linux syscalls: write=4004, mount=4021, exit=4001
 *
 * Usage: ./mkinit > linuxrc
 *
 * Attribution: New Blackroo work (2026, GPL v2)
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <arpa/inet.h>

/* Must match kernel include/linux/flat.h EXACTLY */
struct flat_hdr {
    char     magic[4];       /* "bFLT" */
    uint32_t rev;            /* must be 2 */
    uint32_t entry_point;    /* offset of start from beginning of text segment */
    uint32_t text_start;     /* offset of text segment from beginning of file */
    uint32_t data_start;     /* offset of data segment from beginning of file */
    uint32_t data_end;       /* offset of end of data from beginning of file */
    uint32_t bss_end;        /* offset of end of bss from beginning of file */
    uint32_t stack_size;     /* size of stack in bytes */
    uint32_t reloc_start;    /* offset of relocation records from beginning of file */
    uint32_t reloc_count;    /* number of relocation records */
    uint32_t flags;
    uint32_t filler[5];      /* reserved, must be zero */
};

/* From kernel flat.h — NOTE: 0x01000000 not 0x01! */
#define FLAT_FLAG_RAM 0x01000000

/*
 * MIPS R3000 machine code.
 *
 * Syscall convention:
 *   $v0 = syscall number
 *   $a0-$a3 = arguments
 *   syscall instruction triggers kernel
 *
 * Syscall numbers (MIPS o32):
 *   write = 4004
 *   mount = 4021
 *   getpid = 4020
 *   pause = 4029 (wait for signal)
 *   exit  = 4001
 */
static unsigned char mips_code[] = {
    /* === Prologue: set up stack frame === */
    0xa0, 0xff, 0xbd, 0x27,  /* addiu $sp, $sp, -96 */

    /* === BLACKROO: open /dev/console as stdin/stdout/stderr ===
     * Init process has no file descriptors open.
     * Must open /dev/console explicitly for write() to work.
     * open=4005, dup=4041, O_RDWR=2
     */

    /* Store "/dev/console\0" on stack at $sp+64 */
    0x2f, 0x64, 0x02, 0x3c,  /* lui $v0, 0x642f ("/d") */
    0x76, 0x65, 0x42, 0x34,  /* ori $v0, 0x6576 ("ev") */
    0x40, 0x00, 0xa2, 0xaf,  /* sw $v0, 64($sp) */
    0x2f, 0x63, 0x02, 0x3c,  /* lui $v0, 0x632f ("/c") */
    0x6e, 0x6f, 0x42, 0x34,  /* ori $v0, 0x6f6e ("on") */
    0x44, 0x00, 0xa2, 0xaf,  /* sw $v0, 68($sp) */
    0x73, 0x6f, 0x02, 0x3c,  /* lui $v0, 0x6f73 ("so") */
    0x65, 0x6c, 0x42, 0x34,  /* ori $v0, 0x6c65 ("le") */
    0x48, 0x00, 0xa2, 0xaf,  /* sw $v0, 72($sp) */
    0x4c, 0x00, 0xa0, 0xaf,  /* sw $zero, 76($sp) (null terminator) */

    /* open("/dev/console", O_RDWR) -> fd 0 */
    0xa5, 0x0f, 0x02, 0x24,  /* addiu $v0, $zero, 4005 (open) */
    0x40, 0x00, 0xa4, 0x27,  /* addiu $a0, $sp, 64 (path) */
    0x02, 0x00, 0x05, 0x24,  /* addiu $a1, $zero, 2 (O_RDWR) */
    0x00, 0x00, 0x06, 0x24,  /* addiu $a2, $zero, 0 */
    0x0c, 0x00, 0x00, 0x00,  /* syscall */

    /* dup(0) -> fd 1 (stdout) */
    0xc9, 0x0f, 0x02, 0x24,  /* addiu $v0, $zero, 4041 (dup) */
    0x00, 0x00, 0x04, 0x24,  /* addiu $a0, $zero, 0 */
    0x0c, 0x00, 0x00, 0x00,  /* syscall */

    /* dup(0) -> fd 2 (stderr) */
    0xc9, 0x0f, 0x02, 0x24,  /* addiu $v0, $zero, 4041 (dup) */
    0x00, 0x00, 0x04, 0x24,  /* addiu $a0, $zero, 0 */
    0x0c, 0x00, 0x00, 0x00,  /* syscall */

    /* === (original code continues below) === */

    /* === Store message "Blackroo Linux!\n" on stack === */
    /* "Blac" */
    0x42, 0x6c, 0x02, 0x3c,  /* lui $v0, 0x6c42 */
    0x63, 0x61, 0x42, 0x34,  /* ori $v0, 0x6163 */
    0x00, 0x00, 0xa2, 0xaf,  /* sw $v0, 0($sp) */
    /* "kroo" */
    0x6b, 0x72, 0x02, 0x3c,  /* lui $v0, 0x726b */
    0x6f, 0x6f, 0x42, 0x34,  /* ori $v0, 0x6f6f */
    0x04, 0x00, 0xa2, 0xaf,  /* sw $v0, 4($sp) */
    /* " Lin" */
    0x20, 0x4c, 0x02, 0x3c,  /* lui $v0, 0x4c20 */
    0x6e, 0x69, 0x42, 0x34,  /* ori $v0, 0x696e */
    0x08, 0x00, 0xa2, 0xaf,  /* sw $v0, 8($sp) */
    /* "ux b" */
    0x75, 0x78, 0x02, 0x3c,  /* lui $v0, 0x7875 */
    0x62, 0x20, 0x42, 0x34,  /* ori $v0, 0x2062 */
    0x0c, 0x00, 0xa2, 0xaf,  /* sw $v0, 12($sp) */
    /* "oote" */
    0x6f, 0x6f, 0x02, 0x3c,  /* lui $v0, 0x6f6f */
    0x65, 0x74, 0x42, 0x34,  /* ori $v0, 0x7465 */
    0x10, 0x00, 0xa2, 0xaf,  /* sw $v0, 16($sp) */
    /* "d!\n\0" */
    0x64, 0x21, 0x02, 0x3c,  /* lui $v0, 0x2164 */
    0x00, 0x0a, 0x42, 0x34,  /* ori $v0, 0x0a00 */
    0x14, 0x00, 0xa2, 0xaf,  /* sw $v0, 20($sp) */

    /* === write(1, msg, 23) === */
    0xa4, 0x0f, 0x02, 0x24,  /* addiu $v0, $zero, 4004 (write) */
    0x01, 0x00, 0x04, 0x24,  /* addiu $a0, $zero, 1 (stdout) */
    0x25, 0x28, 0xa0, 0x03,  /* or $a1, $sp, $zero (buf = $sp) */
    0x17, 0x00, 0x06, 0x24,  /* addiu $a2, $zero, 23 (len) */
    0x0c, 0x00, 0x00, 0x00,  /* syscall */

    /* === Store "proc\0" on stack at $sp+24 === */
    0x70, 0x72, 0x02, 0x3c,  /* lui $v0, 0x7270 ("pr") */
    0x63, 0x6f, 0x42, 0x34,  /* ori $v0, 0x6f63 ("oc") */
    0x18, 0x00, 0xa2, 0xaf,  /* sw $v0, 24($sp) */
    0x1c, 0x00, 0xa0, 0xaf,  /* sw $zero, 28($sp) (null term) */

    /* === Store "/proc\0" on stack at $sp+32 === */
    0x2f, 0x70, 0x02, 0x3c,  /* lui $v0, 0x702f ("/p") */
    0x6f, 0x72, 0x42, 0x34,  /* ori $v0, 0x726f ("ro") */
    0x20, 0x00, 0xa2, 0xaf,  /* sw $v0, 32($sp) */
    0x00, 0x63, 0x02, 0x3c,  /* lui $v0, 0x6300 ("c\0") */
    0x24, 0x00, 0xa2, 0xaf,  /* sw $v0, 36($sp) */

    /* === mount("proc", "/proc", "proc", 0, NULL) === */
    /* syscall 4021 = mount */
    0xb5, 0x0f, 0x02, 0x24,  /* addiu $v0, $zero, 4021 */
    0x18, 0x00, 0xa4, 0x27,  /* addiu $a0, $sp, 24 ("proc") */
    0x20, 0x00, 0xa5, 0x27,  /* addiu $a1, $sp, 32 ("/proc") */
    0x18, 0x00, 0xa6, 0x27,  /* addiu $a2, $sp, 24 ("proc") */
    0x00, 0x00, 0x07, 0x24,  /* addiu $a3, $zero, 0 (flags) */
    /* 5th arg goes on stack at $sp+16 for o32 */
    0x10, 0x00, 0xa0, 0xaf,  /* sw $zero, 16($sp) (data=NULL) */
    0x0c, 0x00, 0x00, 0x00,  /* syscall */

    /* === Write "Init done.\n" === */
    /* Store on stack at $sp+40 */
    0x49, 0x6e, 0x02, 0x3c,  /* lui $v0, 0x6e49 ("In") */
    0x74, 0x69, 0x42, 0x34,  /* ori $v0, 0x6974 ("it") */
    0x28, 0x00, 0xa2, 0xaf,  /* sw $v0, 40($sp) */
    0x20, 0x64, 0x02, 0x3c,  /* lui $v0, 0x6420 (" d") */
    0x6e, 0x6f, 0x42, 0x34,  /* ori $v0, 0x6f6e ("on") */
    0x2c, 0x00, 0xa2, 0xaf,  /* sw $v0, 44($sp) */
    0x65, 0x2e, 0x02, 0x3c,  /* lui $v0, 0x2e65 ("e.") */
    0x00, 0x0a, 0x42, 0x34,  /* ori $v0, 0x0a00 ("\n\0") */
    0x30, 0x00, 0xa2, 0xaf,  /* sw $v0, 48($sp) */

    0xa4, 0x0f, 0x02, 0x24,  /* addiu $v0, $zero, 4004 (write) */
    0x01, 0x00, 0x04, 0x24,  /* addiu $a0, $zero, 1 */
    0x28, 0x00, 0xa5, 0x27,  /* addiu $a1, $sp, 40 */
    0x0b, 0x00, 0x06, 0x24,  /* addiu $a2, $zero, 11 */
    0x0c, 0x00, 0x00, 0x00,  /* syscall */

    /* === Infinite loop: pause() forever === */
    /* init must NEVER exit or the kernel panics */
    /* loop: */
    0xbd, 0x0f, 0x02, 0x24,  /* addiu $v0, $zero, 4029 (pause) */
    0x0c, 0x00, 0x00, 0x00,  /* syscall */
    0xfd, 0xff, 0x00, 0x10,  /* b loop (-3 instructions) */
    0x00, 0x00, 0x00, 0x00,  /* nop (delay slot) */
};

int main(int argc, char **argv) {
    struct flat_hdr hdr;
    uint32_t header_size = sizeof(struct flat_hdr);
    uint32_t code_size = sizeof(mips_code);
    uint32_t total_size = header_size + code_size;

    memset(&hdr, 0, sizeof(hdr));
    memcpy(hdr.magic, "bFLT", 4);
    hdr.rev         = 2;                    /* kernel checks rev == 2 */
    hdr.entry_point = 0;                    /* start at beginning of text */
    hdr.text_start  = header_size;          /* text starts after 64-byte header */
    hdr.data_start  = total_size;           /* no separate data segment */
    hdr.data_end    = total_size;           /* no data */
    hdr.bss_end     = total_size + 256;     /* small bss */
    hdr.stack_size  = 8192;                 /* 8KB stack */
    hdr.reloc_start = total_size;           /* relocs at end (none) */
    hdr.reloc_count = 0;                    /* NO relocations */
    hdr.flags       = FLAT_FLAG_RAM;        /* 0x01000000 = load to RAM */

    fwrite(&hdr, sizeof(hdr), 1, stdout);
    fwrite(mips_code, code_size, 1, stdout);

    return 0;
}
