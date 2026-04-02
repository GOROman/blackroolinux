/*
 * memory.c: PlayStation 1 memory initialisation.
 *
 * Configures the RAM_SIZE register (0x1F801060) to match the
 * installed RAM: 2MB (stock), 4MB (partial upgrade), or 8MB (full mod).
 *
 * Attribution:
 *   Original 2MB-only version: Runix project (pre-2007)
 *   Multi-size support and auto-detection: Blackroo Linux (2026)
 *   Register values from psx-spx: https://psx-spx.consoledev.net/memorycontrol/
 */
#include <linux/init.h>
#include <linux/config.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/bootmem.h>

#include <asm/addrspace.h>
#include <asm/page.h>
#include <asm/bootinfo.h>
#include <asm/ps/hwregs.h>

/* One definition, shared with fs/binfmt_fixed.c and (by hand) with
 * userland/blackroo.ld. See the header for what moves together. */
#include <asm/blackroo-user.h>
#define BLACKROO_USER_RESERVE  BLACKROO_USER_SIZE

/*
 * Probe actual RAM size by writing unique patterns at power-of-2
 * boundaries and checking for mirroring. Uses uncached KSEG1
 * addresses (0xA0000000+) to bypass the instruction/data cache.
 *
 * Must be called BEFORE the kernel writes data structures to
 * high memory, as the probe writes are destructive.
 *
 * Returns detected RAM size in bytes.
 */
static unsigned long __init psx_probe_ram(volatile int *mem_size_reg)
{
	volatile unsigned long *base   = (volatile unsigned long *)0xA0000000;
	volatile unsigned long *at_2mb = (volatile unsigned long *)0xA0200000;
	volatile unsigned long *at_4mb = (volatile unsigned long *)0xA0400000;

	/* Configure register for maximum (8MB) so we can probe all of it */
	*mem_size_reg = MEM_SIZE_REG_8MB;

	/* Write unique patterns at base, 2MB, and 4MB boundaries */
	*base   = 0xDEADBEEF;
	*at_2mb = 0xCAFEBABE;
	*at_4mb = 0x12345678;

	/* Check: did the 4MB write wrap around and overwrite base? */
	if (*base != 0xDEADBEEF) {
		/* Base was overwritten — RAM is smaller than 4MB */
		/* Check if 2MB boundary is distinct */
		*base   = 0xDEADBEEF;
		*at_2mb = 0xCAFEBABE;
		if (*base == 0xCAFEBABE) {
			/* 2MB write wrapped to base — only 1MB (unlikely) */
			*mem_size_reg = MEM_SIZE_REG_2MB;
			return 1 << 20;
		}
		/* 2MB is distinct from base, but 4MB wraps — 2MB system */
		*mem_size_reg = MEM_SIZE_REG_2MB;
		return 2 << 20;
	}

	/* Base still intact. Check if 2MB and 4MB are both distinct */
	if (*at_2mb == 0xCAFEBABE && *at_4mb == 0x12345678) {
		/* All three are distinct — 8MB */
		*mem_size_reg = MEM_SIZE_REG_8MB;
		return 8 << 20;
	}

	if (*at_2mb == 0xCAFEBABE) {
		/* 2MB is distinct, 4MB mirrors — 4MB system */
		*mem_size_reg = MEM_SIZE_REG_4MB;
		return 4 << 20;
	}

	/* Fallback: assume 2MB (safest) */
	*mem_size_reg = MEM_SIZE_REG_2MB;
	return 2 << 20;
}

static void __init setup_memory_region(void)
{
	unsigned long mem_start, mem_size;
	volatile int *mem_size_reg = (int *)0x1f801060;

	/*
	 * BLACKROO: the size is a build-time choice again.
	 *
	 * This used to be hard-wired to 8 MB (`*mem_size_reg = 0xB88`) with the
	 * config-driven block commented out, which is fatal on a stock console:
	 * the kernel would hand the allocator 8 MB of RAM that isn't installed,
	 * and every page above 2 MB just mirrors low memory. A CD that has to
	 * boot on unmodified hardware must be built with CONFIG_PSX_2MB_RAM.
	 *
	 * Register values from psx-spx Memory Control:
	 *   2 MB 0x0888 | 4 MB 0x0988 | 8 MB 0x0B88
	 */
#if defined(CONFIG_PSX_RAM_AUTO)
	/* Runtime probe. Historically mis-read as 1 MB under DuckStation —
	 * prefer an explicit size unless you are testing the probe itself. */
	mem_size = psx_probe_ram(mem_size_reg);
#elif defined(CONFIG_PSX_8MB_RAM)
	*mem_size_reg = MEM_SIZE_REG_8MB;
	mem_size = 8 << 20;
#elif defined(CONFIG_PSX_4MB_RAM)
	*mem_size_reg = MEM_SIZE_REG_4MB;
	mem_size = 4 << 20;
#else
	*mem_size_reg = MEM_SIZE_REG_2MB;
	mem_size = 2 << 20;
#endif

	mem_start = 0;

	/*
	 * BLACKROO: hold back the top of RAM for userspace.
	 *
	 * binfmt_fixed loads statically linked programs at the address they
	 * were linked for, which only works if the kernel never hands that
	 * memory out. Simply declaring less RAM than the machine has is the
	 * least invasive way to reserve it: the allocator never sees it, so
	 * nothing needs to know it is special.
	 *
	 * Must match BLACKROO_USER_BASE/SIZE in fs/binfmt_fixed.c.
	 */
#ifdef CONFIG_BINFMT_FIXED
	if (mem_size > BLACKROO_USER_RESERVE) {
		mem_size -= BLACKROO_USER_RESERVE;
		printk("PSX: reserving %luKB at the top of RAM for userspace\n",
		       (unsigned long)BLACKROO_USER_RESERVE >> 10);
	}
#endif

	add_memory_region(mem_start, mem_size, BOOT_MEM_RAM);

	printk("PSX: %lu KB RAM configured (reg=0x%04x)\n",
	       mem_size >> 10, *mem_size_reg);
}

void __init prom_meminit(unsigned int magic)
{
	setup_memory_region();
}

void prom_free_prom_memory (void)
{
	unsigned long addr, end;
	extern	char _ftext;

	/*
	 * Free everything below the kernel itself but leave
	 * the first page reserved for the exception handlers.
	 */

	end = PHYSADDR(&_ftext);

	addr = PAGE_SIZE;

	while (addr < end) {
		ClearPageReserved(virt_to_page(addr));
		set_page_count(virt_to_page(addr), 1);
		free_page(addr);
		addr += PAGE_SIZE;
	}

	printk("Freeing unused PROM memory: %ldk freed\n",
	       (end - (PAGE_SIZE)) >> 10);
}

int is_in_rom(unsigned long addr) {
	return 0;
}
