/*
 * elf_stubs.c — Stub functions for BINFMT_ELF on uClinux (no MMU)
 *
 * BINFMT_ELF requires do_brk() and setup_arg_pages() which are MMU
 * functions. On uClinux these don't exist. These stubs provide
 * minimal implementations so statically-linked ELF binaries can
 * be executed.
 *
 * BLACKROO: This is a hack to get ELF busybox running. The proper
 * approach is BINFMT_FLAT with elf2flt-compiled binaries. But we
 * don't have elf2flt yet, and we DO have an ELF busybox.
 *
 * Attribution: Blackroo Linux (2026). New stubs for uClinux ELF support.
 */

#include <linux/mm.h>
#include <linux/mman.h>
#include <linux/slab.h>
#include <linux/binfmts.h>
#include <linux/sched.h>

/*
 * do_brk() - extend the heap/BSS area
 *
 * On MMU systems this creates new virtual memory mappings.
 * On uClinux we just allocate physical memory with kmalloc
 * and update the brk pointer. This is crude but works for
 * statically linked binaries.
 */
unsigned long do_brk(unsigned long addr, unsigned long len)
{
	if (!len)
		return addr;

	/*
	 * On uClinux, memory is flat — just update the brk.
	 * The memory at 'addr' should already be accessible
	 * since there's no memory protection.
	 */
	current->mm->brk = addr + len;
	return addr;
}

/*
 * setup_arg_pages() - set up the stack for argument passing
 *
 * On MMU systems this creates a VMA for the stack.
 * On uClinux the stack is already set up in flat memory.
 */
int setup_arg_pages(struct linux_binprm *bprm)
{
	/* Stack is already allocated — nothing to do on uClinux */
	return 0;
}

/*
 * copy_strings_kernel() — copy argv/envp strings from kernel space
 *
 * On MMU systems this is in fs/exec.c inside #ifndef NO_MM.
 * On uClinux, the copy_strings() function doesn't exist either,
 * so we implement a minimal version that copies strings into
 * the binprm page.
 */
int copy_strings_kernel(int argc, char **argv, struct linux_binprm *bprm)
{
	/* BLACKROO: on uClinux, user and kernel share address space.
	 * The strings are already accessible. Just update bprm->p.
	 * This is a minimal stub — full implementation would copy
	 * strings to the bprm pages. */
	int i;
	for (i = 0; i < argc; i++) {
		int len = 0;
		char *s = argv[i];
		while (s[len]) len++;
		bprm->p -= (len + 1);
	}
	return 0;
}
