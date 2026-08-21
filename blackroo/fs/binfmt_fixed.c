/*
 * binfmt_fixed.c — load a statically linked ELF at the address it was
 *                  linked for, on a machine with no MMU.
 *
 * Why this exists
 * ---------------
 * This kernel builds BINFMT_FLAT and no ELF loader, and every BusyBox in the
 * tree is a static ELF linked at 0x00400000 — 4 MB, on a console with 2 MB and
 * no MMU to map it anywhere else. So execve() has never succeeded here, and a
 * root filesystem only moves the failure from "cannot mount" to "no init
 * found".
 *
 * The FLAT format solves this properly by being relocatable, but producing
 * bFLT for MIPS with the toolchains available turned out to be a research
 * project: plain builds emit R_MIPS_26 absolute call targets that bFLT
 * relocations cannot patch, and -membedded-pic is overridden by the target's
 * -mabicalls default (see docs/23-ROOT-FILESYSTEM-PLAN.md).
 *
 * This takes the other road, the one small no-MMU systems have always taken:
 * link the program at a fixed address, reserve that memory so the kernel never
 * allocates it, and copy the segments straight there. No relocation, no GOT,
 * no new toolchain. The cost is that one such program is resident at a time,
 * which is exactly enough for a shell.
 *
 * The reserved window is BLACKROO_USER_BASE..+BLACKROO_USER_SIZE, carved out
 * in arch/mipsnommu/ps/prom/memory.c by declaring less RAM to the kernel than
 * the machine has. Nothing else can hand that memory out, so a fixed link
 * address is safe.
 *
 * Attribution: New Blackroo work (2026, GPL v2)
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/stat.h>
#include <linux/sched.h>
#include <linux/mm.h>
#include <linux/mman.h>
#include <linux/errno.h>
#include <linux/signal.h>
#include <linux/string.h>
#include <linux/binfmts.h>
#include <linux/personality.h>
#include <linux/elf.h>
#include <linux/init.h>
#include <linux/slab.h>

#include <asm/uaccess.h>
#include <asm/pgalloc.h>

/*
 * The reserved user window, as seen from USER mode.
 *
 * Physically this is the top of RAM, held back in prom/memory.c. The kernel
 * would call it 0x801d0000 (KSEG0); user mode cannot touch KSEG0 at all, so
 * programs are linked and loaded at the KUSEG address for the same memory.
 * The PS1 maps KUSEG straight to RAM, no TLB.
 *
 * BASE/SIZE/END come from one header shared with prom/memory.c - they used to
 * be duplicated here with a comment asking the reader to keep them in step.
 */
#include <asm/blackroo-user.h>

/* leave room at the top for the stack */
#define BLACKROO_STACK_SIZE	0x00003000	/* 12 KB */

#undef DEBUG_FIXED
#ifdef DEBUG_FIXED
#define DBG(fmt, args...) printk("binfmt_fixed: " fmt, ##args)
#else
#define DBG(fmt, args...)
#endif

static int load_fixed_binary(struct linux_binprm *, struct pt_regs *);

static struct linux_binfmt fixed_format = {
	NULL, THIS_MODULE, load_fixed_binary, NULL, NULL, 0
};

/* copy one string to the user stack area, returning the new top */
static unsigned long putstring(unsigned long p, char *s)
{
	int len = strlen(s) + 1;

	p -= len;
	memcpy((char *)p, s, len);
	return p;
}

static unsigned long putstringarray(unsigned long p, int count, char **array)
{
	while (count)
		p = putstring(p, array[--count]);
	return p;
}

/*
 * Build argc/argv/envp on the stack, the same shape binfmt_flat produces.
 * There is no user/kernel split here, so these are plain stores.
 */
static unsigned long create_fixed_tables(unsigned long pp,
					 struct linux_binprm *bprm)
{
	unsigned long *argv, *envp, *sp;
	char *p = (char *)pp;
	int argc = bprm->argc;
	int envc = bprm->envc;

	sp = (unsigned long *)((-(unsigned long)sizeof(char *)) & (unsigned long)p);

	sp -= envc + 1;
	envp = sp;
	sp -= argc + 1;
	argv = sp;

	*(--sp) = argc;

	current->mm->arg_start = (unsigned long)p;
	while (argc-- > 0) {
		*argv++ = (unsigned long)p;
		while (*p++)
			;
	}
	*argv = 0;

	current->mm->arg_end = current->mm->env_start = (unsigned long)p;
	while (envc-- > 0) {
		*envp++ = (unsigned long)p;
		while (*p++)
			;
	}
	*envp = 0;
	current->mm->env_end = (unsigned long)p;

	return (unsigned long)sp;
}

static int load_fixed_binary(struct linux_binprm *bprm, struct pt_regs *regs)
{
	struct elfhdr *elf;
	struct elf_phdr *phdr = NULL;
	unsigned long phsize, low = ~0UL, high = 0;
	unsigned long stack_top, p;
	int i, retval, nload = 0;

	elf = (struct elfhdr *)bprm->buf;

	if (memcmp(elf->e_ident, ELFMAG, SELFMAG) != 0)
		return -ENOEXEC;
	if (elf->e_type != ET_EXEC)
		return -ENOEXEC;
	if (elf->e_machine != EM_MIPS)
		return -ENOEXEC;
	if (!bprm->file->f_op || !bprm->file->f_op->read)
		return -EACCES;

	phsize = elf->e_phnum * sizeof(struct elf_phdr);
	if (elf->e_phnum < 1 || phsize > 64 * sizeof(struct elf_phdr))
		return -ENOEXEC;

	phdr = (struct elf_phdr *)kmalloc(phsize, GFP_KERNEL);
	if (!phdr)
		return -ENOMEM;

	retval = kernel_read(bprm->file, elf->e_phoff, (char *)phdr, phsize);
	if (retval != phsize) {
		retval = (retval < 0) ? retval : -EIO;
		goto out_free;
	}

	/*
	 * Check every loadable segment lands inside the reserved window before
	 * touching anything. A binary linked for the wrong address must be
	 * rejected cleanly, not discovered halfway through overwriting the
	 * kernel.
	 */
	for (i = 0; i < elf->e_phnum; i++) {
		unsigned long start, end;

		if (phdr[i].p_type != PT_LOAD)
			continue;

		start = phdr[i].p_vaddr;
		end = start + phdr[i].p_memsz;

		if (start < BLACKROO_USER_BASE ||
		    end > BLACKROO_USER_END - BLACKROO_STACK_SIZE) {
			printk(KERN_ERR "binfmt_fixed: %s segment %d is "
			       "0x%lx..0x%lx, outside the reserved window "
			       "0x%08x..0x%08x\n",
			       bprm->filename, i, start, end,
			       BLACKROO_USER_BASE,
			       BLACKROO_USER_END - BLACKROO_STACK_SIZE);
			retval = -ENOEXEC;
			goto out_free;
		}
		if (start < low)
			low = start;
		if (end > high)
			high = end;
		nload++;
	}

	if (!nload) {
		retval = -ENOEXEC;
		goto out_free;
	}

	retval = flush_old_exec(bprm);
	if (retval)
		goto out_free;

	/* point of no return */
	set_personality(PER_LINUX);

	current->mm->start_code = low;
	current->mm->end_code = high;
	current->mm->start_data = low;
	current->mm->end_data = high;
	current->mm->brk = current->mm->start_brk = high;

	for (i = 0; i < elf->e_phnum; i++) {
		if (phdr[i].p_type != PT_LOAD)
			continue;

		DBG("segment %d: file 0x%lx+0x%lx -> 0x%lx (mem 0x%lx)\n",
		    i, (unsigned long)phdr[i].p_offset,
		    (unsigned long)phdr[i].p_filesz,
		    (unsigned long)phdr[i].p_vaddr,
		    (unsigned long)phdr[i].p_memsz);

		if (phdr[i].p_filesz) {
			retval = kernel_read(bprm->file, phdr[i].p_offset,
					     (char *)phdr[i].p_vaddr,
					     phdr[i].p_filesz);
			if (retval != phdr[i].p_filesz) {
				printk(KERN_ERR "binfmt_fixed: short read on "
				       "segment %d\n", i);
				send_sig(SIGKILL, current, 0);
				retval = (retval < 0) ? retval : -EIO;
				goto out_free;
			}
		}

		/* .bss and any other zero-fill tail */
		if (phdr[i].p_memsz > phdr[i].p_filesz)
			memset((char *)phdr[i].p_vaddr + phdr[i].p_filesz, 0,
			       phdr[i].p_memsz - phdr[i].p_filesz);
	}

	compute_creds(bprm);
	current->flags &= ~PF_FORKNOEXEC;

	flush_icache_range(current->mm->start_code, current->mm->end_code);

	set_binfmt(&fixed_format);

	/* arguments and environment at the top of the window */
	stack_top = BLACKROO_USER_END - 16;
	p = putstringarray(stack_top, 1, &bprm->filename);
	p = putstringarray(p, bprm->envc, bprm->envp);
	p = putstringarray(p, bprm->argc, bprm->argv);

	current->mm->start_stack = create_fixed_tables(p, bprm);

	printk(KERN_INFO "binfmt_fixed: %s at 0x%lx..0x%lx, entry 0x%lx, "
	       "stack 0x%lx\n", bprm->filename, low, high,
	       (unsigned long)elf->e_entry, current->mm->start_stack);

	kfree(phdr);

	start_thread(regs, elf->e_entry, current->mm->start_stack);

	return 0;

out_free:
	kfree(phdr);
	return retval;
}

static int __init init_fixed_binfmt(void)
{
	printk(KERN_INFO "binfmt_fixed: fixed-address ELF loader, "
	       "window 0x%08x..0x%08x\n",
	       BLACKROO_USER_BASE, BLACKROO_USER_END);
	return register_binfmt(&fixed_format);
}

static void __exit exit_fixed_binfmt(void)
{
	unregister_binfmt(&fixed_format);
}

module_init(init_fixed_binfmt);
module_exit(exit_fixed_binfmt);
