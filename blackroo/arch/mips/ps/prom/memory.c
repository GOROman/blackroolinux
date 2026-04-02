/*
 * memory.c: memory initialisation code.
 */
#include <linux/init.h>
#include <linux/config.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/bootmem.h>

#include <asm/addrspace.h>
#include <asm/page.h>

#include <asm/bootinfo.h>

static void __init setup_memory_region(void)
{
	add_memory_region(0, 2 << 20, BOOT_MEM_RAM);
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
	       (end - PAGE_SIZE) >> 10);
}

/* BLACKROO: add_memory_region stub — the standard MIPS kernel
 * doesn't have this function (it's from mipsnommu).
 * For PlayStation, we just set boot_mem_map directly. */
#include <asm/bootinfo.h>

struct boot_mem_map_entry {
	unsigned long addr;
	unsigned long size;
	long type;
};

struct boot_mem_map_type {
	int nr_map;
	struct boot_mem_map_entry map[32];
};

struct boot_mem_map_type boot_mem_map = {0};

void __init add_memory_region(unsigned long start, unsigned long size, long type)
{
	int idx = boot_mem_map.nr_map;
	if (idx < 32) {
		boot_mem_map.map[idx].addr = start;
		boot_mem_map.map[idx].size = size;
		boot_mem_map.map[idx].type = type;
		boot_mem_map.nr_map++;
	}
}
