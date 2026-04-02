/*
 * arch/mips/kernel/head.S
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License.  See the file "COPYING" in the main directory of this archive
 * for more details.
 *
 * Copyright (C) 1994, 1995 Waldorf Electronics
 * Written by Ralf Baechle and Andreas Busse
 * Copyright (C) 1995 - 1999 Ralf Baechle
 * Copyright (C) 1996 Paul M. Antoine
 * Modified for DECStation and hence R3000 support by Paul M. Antoine
 * Further modifications by David S. Miller and Harald Koerfgen
 * Copyright (C) 1999 Silicon Graphics, Inc.
 *
 * Head.S contains the MIPS exception handler and startup code.
 *
 **************************************************************************
 *  9 Nov, 2000.
 *  Added Cache Error exception handler and SBDDP EJTAG debug exception.
 *
 *  Kevin Kissell, kevink@mips.com and Carsten Langgaard, carstenl@mips.com
 *  Copyright (C) 2000 MIPS Technologies, Inc.  All rights reserved.
 **************************************************************************
 */
#include <linux/config.h>
#include <linux/threads.h>

#include <asm/asm.h>
#include <asm/cacheops.h>
#include <asm/current.h>
#include <asm/offset.h>
#include <asm/processor.h>
#include <asm/regdef.h>
#include <asm/cachectl.h>
#include <asm/mipsregs.h>
#include <asm/stackframe.h>
#include <asm/bootinfo.h>

	.text
	/*
	 * Reserved space for exception handlers.
	 * Necessary for machines which link their kernels at KUSEG.
	 * FIXME: Use the initcode feature to get rid of unused handler
	 * variants.
	 */
	.fill	0x280
/*	
 * This is space for the interrupt handlers.
 * After trap_init() they are located at virtual address KUSEG.
 *
 * These handlers much be written in a relocatable manner
 * because based upon the cpu type an arbitrary one of the
 * following pieces of code will be copied to the KUSEG
 * vector location.
 */

	/* Cache Error */
	LEAF(except_vec2_generic)
	.set	noat
	.set    mips0
	/*
	 * This is a very bad place to be.  Our cache error
	 * detection has triggered.  If we have write-back data
	 * in the cache, we may not be able to recover.  As a
	 * first-order desperate measure, turn off KUSEG cacheing.
	 */
	mfc0	k0,CP0_CONFIG
	li	k1,~CONF_CM_CMASK
	and	k0,k0,k1
	ori	k0,k0,CONF_CM_UNCACHED
	mtc0	k0,CP0_CONFIG
	/* Give it a few cycles to sink in... */
	nop
	nop
	nop

	j	cache_parity_error
	nop
	END(except_vec2_generic)

	/* General exception vector R4000 version. */
	NESTED(except_vec3_r4000, 0, sp)
	.set	noat
	mfc0	k1, CP0_CAUSE
	andi	k1, k1, 0x7c
	li	k0, 31<<2
	beq	k1, k0, handle_vced
	 li	k0, 14<<2
	beq	k1, k0, handle_vcei
	 la	k0, exception_handlers
	addu	k0, k0, k1
	lw	k0, (k0)
	nop
	jr	k0
	 nop

/*
 * Big shit, we now may have two dirty primary cache lines for the same
 * physical address.  We can savely invalidate the line pointed to by
 * c0_badvaddr because after return from this exception handler the load /
 * store will be re-executed.
 */
	.set    mips3
handle_vced:
	mfc0	k0, CP0_BADVADDR
 li k1, -4
 and k0, k1
	mtc0	zero, CP0_TAGLO
	cache	Index_Store_Tag_D,(k0)
	cache	Hit_Writeback_Inv_SD,(k0)
#ifdef CONFIG_PROC_FS
	lui	k0, %hi(vced_count)
	lw	k1, %lo(vced_count)(k0)
	addiu	k1, 1
	sw	k1, %lo(vced_count)(k0)
#endif
	eret

handle_vcei:
	mfc0	k0, CP0_BADVADDR
	cache	Hit_Writeback_Inv_SD,(k0)		# also cleans pi
#ifdef CONFIG_PROC_FS
	lui	k0, %hi(vcei_count)
	lw	k1, %lo(vcei_count)(k0)
	addiu	k1, 1
	sw	k1, %lo(vcei_count)(k0)
#endif
	eret

	END(except_vec3_r4000)
	.set	at

	/* General exception vector. */
	NESTED(except_vec3_generic, 0, sp)
	.set	noat
	.set	mips0
	mfc0	k1, CP0_CAUSE
	la	k0, exception_handlers
	andi	k1, k1, 0x7c
	addu	k0, k0, k1
	lw	k0, (k0)
	nop
	jr	k0
	 nop
	END(except_vec3_generic)
	.set	at

	/*
	 * Special interrupt vector for embedded MIPS.  This is a
	 * dedicated interrupt vector which reduces interrupt processing
	 * overhead.  The jump instruction will be inserted here at
	 * initialization time.  This handler may only be 8 bytes in size!
	 */
	NESTED(except_vec4, 0, sp)
1:	j	1b			/* Dummy, will be replaced */
	 nop
	END(except_vec4)

	/*
	 * SBDDP EJTAG debug exception handler.
	 * The EJTAG debug exception entry point is 0xbfc00480, which
	 * normally is in the boot PROM, so the boot PROM must do a
	 * unconditional jump to this vector.
	 */	
	NESTED(except_vec_ejtag_debug, 0, sp)
	PRINT("SDBBP EJTAG debug exception - not handled yet, hang!\n");
1:	j	1b
	 nop
	END(except_vec_ejtag_debug)

	
/*
 * Kernel entry point
 */
NESTED(kernel_entry, 16, sp)
	.set	noreorder
	/* The following two symbols are used for kernel profiling. */
	EXPORT(stext)
	EXPORT(_stext)

	/*
	 * BLACKROO: quiesce the machine before touching anything.
	 *
	 * Every previous boot of this kernel came in via UniROM or our own
	 * kloader, and kloader_launch() (bootloader/src/kernel.c) masked the
	 * interrupts, acked I_STAT and stopped DMA before jumping here. The
	 * BIOS CD loader does none of that: when a disc boots us directly,
	 * the BIOS shell's VBlank IRQ and DMA channels are still live as we
	 * clear BSS and overwrite the exception vector below. Do the work
	 * ourselves so the entry conditions are the same either way.
	 *
	 * Registers: psx-spx Interrupts / DMA Channels.
	 *   0x1f801070 I_STAT   0x1f801074 I_MASK
	 *   0x1f8010f0 DPCR     0x1f8010f4 DICR
	 * Accessed through KSEG1 (0xbf80...) so nothing is cached.
	 */
	mtc0	zero, $12		/* SR: IEc=0 BEV=0 IsC=0, kernel mode */
	nop
	li	t0, 0xbf801074
	sw	zero, 0(t0)		/* I_MASK = 0: mask every source */
	li	t0, 0xbf801070
	sw	zero, 0(t0)		/* I_STAT = 0: ack anything pending */
	li	t0, 0xbf8010f0
	sw	zero, 0(t0)		/* DPCR  = 0: all DMA channels off */
	li	t0, 0xbf8010f4
	sw	zero, 0(t0)		/* DICR  = 0: DMA interrupts off */

	/*
	 * BLACKROO: earliest possible sign of life on SIO1 — "BR!".
	 *
	 * This runs before C, before BSS is clear, before any driver. If a
	 * host sees these three bytes the BIOS did hand us control and any
	 * later silence is our fault; if it sees nothing, we were never
	 * entered. The TX wait is bounded, so a machine with no cable (or no
	 * CTS) falls through instead of hanging here forever.
	 *
	 * SIO1 at 0x1f801050: +0 DATA, +4 STAT, +8 MODE, +0xa CTRL, +0xe RATE.
	 * MODE 0x4e = 8N1 with the 16x baud multiplier, CTRL 0x27 asserts RTS
	 * — the two values docs/13-SIO1-HARDWARE-RESEARCH.md found are
	 * mandatory on real silicon.
	 */
	li	t0, 0xbf801050
	li	t1, 0x0012
	sh	t1, 0x0e(t0)		/* RATE = 115200 */
	li	t1, 0x004e
	sh	t1, 0x08(t0)		/* MODE = 8N1, MUL16 */
	li	t1, 0x0027
	sh	t1, 0x0a(t0)		/* CTRL = TX|RX|DTR|RTS */

	li	t3, 'B'
	bal	__early_sio_putc
	 nop
	li	t3, 'R'
	bal	__early_sio_putc
	 nop
	li	t3, '!'
	bal	__early_sio_putc
	 nop
	li	t3, 0x0a
	bal	__early_sio_putc
	 nop

	/*
	 * Stack for kernel and init, current variable
	 */
	la	$28, init_task_union
	addiu	t0, $28, KERNEL_STACK_SIZE-32
	sw	t0, kernelsp
	subu	sp, t0, 4*SZREG

	/* The firmware/bootloader passes argc/argp/envp
	 * to us as arguments.  But clear bss first because
	 * the romvec and other important info is stored there
	 * by prom_init().
	 */
	la	t0, _edata
	sw	zero, (t0)
	la	t1, (_end - 4)
1:
	addiu	t0, 4
	bne	t0, t1, 1b
	 sw	zero, (t0)

	/*
	 * BLACKROO: Install TLB refill handler at 0x80000000.
	 * The R3000A jumps here on UTLB Miss (BEV=0).
	 * This does identity mapping: virtual = physical.
	 * Without this, ANY KUSEG access (userspace) crashes.
	 *
	 * R3000 TLB entry format:
	 *   EntryHi: VPN (bits 31:12) | PID (bits 11:6)
	 *   EntryLo: PFN (bits 31:12) | N(11) | D(10) | V(9) | G(8)
	 *
	 * Identity map: PFN = VPN, D=1 (writable), V=1 (valid), G=1 (global)
	 */
	/* Copy 32 bytes (8 instructions) of TLB handler to 0x80000000 */
	la	t0, __tlb_refill_handler
	li	t1, 0x80000000		/* UTLB Miss vector address */
	li	t2, 32			/* handler is 8 instructions = 32 bytes */
2:	lw	t3, 0(t0)
	sw	t3, 0(t1)
	addiu	t0, 4
	addiu	t1, 4
	addiu	t2, -4
	bgtz	t2, 2b
	 nop

	jal	init_arch
	 nop
	END(kernel_entry)

/*
 * BLACKROO: byte out of SIO1, bounded wait. t3 = char, clobbers t0/t1/t2.
 * Used only by the early-boot marker above; the real console is siocon.c.
 */
LEAF(__early_sio_putc)
	.set	noreorder
	li	t0, 0xbf801050
	li	t2, 0x40000		/* spin limit: no host, no problem */
1:	lhu	t1, 0x04(t0)		/* STAT */
	andi	t1, t1, 0x0001		/* bit 0: ready for TX */
	bnez	t1, 2f
	 addiu	t2, t2, -1
	bgtz	t2, 1b
	 nop
	jr	ra			/* gave up — carry on booting */
	 nop
2:	sb	t3, 0(t0)		/* DATA */
	jr	ra
	 nop
	END(__early_sio_putc)

/*
 * BLACKROO: TLB refill handler — copied to 0x80000000 at boot.
 * Identity maps KUSEG: virtual address = physical address.
 * 32 bytes (8 instructions) — fits in the 32-byte UTLB vector slot.
 */
	.set	noreorder
__tlb_refill_handler:
	/* R3000 COP0 registers: $8=BadVAddr, $10=EntryHi, $2=EntryLo */
	mfc0	k0, $8			/* k0 = BadVAddr (faulting address) */
	nop
	mtc0	k0, $10			/* EntryHi = VPN (bits 31:12) | PID 0 */
	ori	k0, k0, 0x700		/* add D(10)|V(9)|G(8) flags */
	mtc0	k0, $2			/* EntryLo = PFN | D | V | G */
	nop
	tlbwr				/* write random TLB entry */
	rfe				/* return from exception */
__tlb_refill_handler_end:

/*
 * This buffer is reserved for the use of the cache error handler.
 */
		.data
		EXPORT(cache_error_buffer)
		.fill	32*4,1,0

EXPORT(kernelsp)
		PTR	0
EXPORT(current_pgd)
		PTR	0
		.text

		.org	0x1000
EXPORT(swapper_pg_dir)

		.org	0x2000
EXPORT(empty_bad_page)

		.org	0x3000
EXPORT(empty_bad_page_table)

		.org	0x4000
EXPORT(invalid_pte_table)

		.org	0x5000
/* XXX This label is required to keep GAS trying to be too clever ...
   Bug?  */
dummy:
/*
 * Align to 8kb boundary for init_task_union which follows in the
 * .text segment.
 */
		.align	13
