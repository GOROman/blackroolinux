/*
 *  include/asm-mipsnommu/blackroo-user.h
 *
 *  The single definition of the fixed userspace window.
 *
 *  binfmt_fixed loads statically linked programs at the address they were
 *  linked for, which only works if the kernel never hands that memory out.
 *  The window is taken by telling the page allocator there is less RAM than
 *  the machine has (arch/mipsnommu/ps/prom/memory.c), so nothing else needs
 *  to know it is special.
 *
 *  This used to be two #defines in two files, each with a comment asking the
 *  reader to keep them in step by hand, plus a third copy in the userspace
 *  link script. They are one number; it lives here now.
 *
 *  FOUR things move together when this changes:
 *    1. this header
 *    2. userland/blackroo.ld           - the ORG address userspace links at
 *    3. every userspace binary         - rebuild them, or they load outside
 *                                        the window and binfmt_fixed refuses
 *    4. the kernel                     - full rebuild; this is a header
 *
 *  The window sits at the TOP of RAM and grows DOWNWARD, so BASE is derived
 *  from the size rather than fixed. See docs/28.
 *
 *  Attribution: New Blackroo work (2026, GPL v2)
 */
#ifndef _ASM_BLACKROO_USER_H
#define _ASM_BLACKROO_USER_H

#include <linux/config.h>

/*
 * Top of RAM, which is where the window sits - so it MUST follow the machine.
 *
 * This was hardcoded to 0x00200000 (2 MB). On an 8 MB console that is wrong in
 * the worst way: prom/memory.c holds back the top of ACTUAL memory, near
 * 0x00800000, while binfmt_fixed would keep loading at 0x001d0000 - an address
 * the page allocator now owns. Not a boot failure; a program and the kernel's
 * own allocations quietly sharing pages.
 *
 * NOTE the size options and the RAM_SIZE register are separate questions.
 * CONFIG_PSX_RAM_AUTO decides what the memory CONTROLLER is told, by probing.
 * These constants decide where a BINARY was LINKED, which a probe cannot
 * influence - the address is already in the ELF by then. A config that sets
 * only RAM_AUTO therefore still needs one of these to say how big the machine
 * is, or the window silently lands at the top of 2 MB.
 *
 * KUSEG: this kernel is linked with plain physical addresses, not KSEG0 (see
 * GUARDRAILS - the uncached alias is (addr & 0x1fffffff) | 0xa0000000, NOT
 * addr | 0x20000000).
 */
#if defined(CONFIG_PSX_16MB_RAM)
#define BLACKROO_USER_TOP	0x01000000
#elif defined(CONFIG_PSX_8MB_RAM)
#define BLACKROO_USER_TOP	0x00800000
#elif defined(CONFIG_PSX_4MB_RAM)
#define BLACKROO_USER_TOP	0x00400000
#else
#define BLACKROO_USER_TOP	0x00200000
#endif

#ifndef CONFIG_BLACKROO_USER_RESERVE_KB
#define CONFIG_BLACKROO_USER_RESERVE_KB	192
#endif

#define BLACKROO_USER_SIZE	(CONFIG_BLACKROO_USER_RESERVE_KB << 10)
#define BLACKROO_USER_END	BLACKROO_USER_TOP
#define BLACKROO_USER_BASE	(BLACKROO_USER_END - BLACKROO_USER_SIZE)

#endif /* _ASM_BLACKROO_USER_H */
