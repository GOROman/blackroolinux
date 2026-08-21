/*
 * blackroo.ld — link script for Blackroo userspace.
 *
 * Everything lands in the fixed window that fs/binfmt_fixed.c reserves, in a
 * single PT_LOAD segment. The MIPS metadata sections are discarded: linked
 * with plain -Ttext they end up at the toolchain's default address
 * (0x00400000), which produces a second PT_LOAD outside the window and the
 * loader rightly refuses it. They carry no runtime meaning here.
 *
 * Attribution: New Blackroo work (2026, GPL v2)
 */

OUTPUT_FORMAT("elf32-littlemips")
OUTPUT_ARCH(mips)
ENTRY(_start)

SECTIONS
{
    /* BLACKROO_USER_BASE. Must equal the value in
     * blackroo/include/asm-mipsnommu/blackroo-user.h - build.sh checks it and
     * refuses to build if the two have drifted. 2 MB top (0x00200000) minus
     * CONFIG_BLACKROO_USER_RESERVE_KB. */
    . = 0x001d0000;

    .text : {
        *(.text.startup)
        *(.text)
        *(.text.*)
        *(.rodata)
        *(.rodata.*)
    }

    .data : {
        *(.data)
        *(.data.*)
        *(.sdata)
        *(.sdata.*)
    }

    __bss_start = .;
    .bss : {
        *(.bss)
        *(.bss.*)
        *(.sbss)
        *(.sbss.*)
        *(COMMON)
    }
    __bss_end = .;

    _end = .;

    /DISCARD/ : {
        *(.MIPS.abiflags)
        *(.reginfo)
        *(.comment)
        *(.pdr)
        *(.gnu.attributes)
        *(.note*)
    }
}
