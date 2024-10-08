/*	$NetBSD: locore.s,v 1.46 2024/01/17 12:33:49 thorpej Exp $	*/

/*
 * Copyright (c) 1980, 1990, 1993
 *	The Regents of the University of California.  All rights reserved.
 *
 * This code is derived from software contributed to Berkeley by
 * the Systems Programming Group of the University of Utah Computer
 * Science Department.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * from: Utah $Hdr: locore.s 1.66 92/12/22$
 *
 *	@(#)locore.s	8.6 (Berkeley) 5/27/94
 */

/*
 * Copyright (c) 1994, 1995 Gordon W. Ross
 * Copyright (c) 1988 University of Utah.
 *
 * This code is derived from software contributed to Berkeley by
 * the Systems Programming Group of the University of Utah Computer
 * Science Department.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. All advertising materials mentioning features or use of this software
 *    must display the following acknowledgement:
 *	This product includes software developed by the University of
 *	California, Berkeley and its contributors.
 * 4. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * from: Utah $Hdr: locore.s 1.66 92/12/22$
 *
 *	@(#)locore.s	8.6 (Berkeley) 5/27/94
 */

#include "opt_compat_netbsd.h"
#include "opt_compat_sunos.h"
#include "opt_ddb.h"
#include "opt_fpsp.h"
#include "opt_fpu_emulate.h"
#include "opt_m68k_arch.h"

#include "ksyms.h"
#include "assym.h"
#include <machine/asm.h>
#include <machine/trap.h>

/*
 * This is for kvm_mkdb, and should be the address of the beginning
 * of the kernel text segment (not necessarily the same as kernbase).
 */
	.text
GLOBAL(kernel_text)

/*
 * Temporary stack for a variety of purposes.
 * Try and make this the first thing is the data segment so it
 * is page aligned.  Note that if we overflow here, we run into
 * our text segment.
 */
	.data
	.space	PAGE_SIZE
ASLOCAL(tmpstk)

	.text
/*
 * Macro to relocate a symbol, used before MMU is enabled.
 */
#define	_RELOC(var, ar)		\
	lea	var-KERNBASE,ar;		\
	addl	%a5,ar

#define	RELOC(var, ar)		_RELOC(_C_LABEL(var), ar)
#define	ASRELOC(var, ar)	_RELOC(_ASM_LABEL(var), ar)

/*
 * Initialization
 *
 * A4 contains the address of the end of the symtab
 * A5 contains physical load point from boot
 * VBR contains zero from ROM.  Exceptions will continue to vector
 * through ROM until MMU is turned on at which time they will vector
 * through our table (vectors.s).
 */

BSS(lowram,4)
BSS(esym,4)

	.text
ASENTRY_NOPROFILE(start)
	movw	#PSL_HIGHIPL, %sr	| no interrupts
	movl	#CACHE_OFF, %d0
	movc	%d0, %cacr		| clear and disable on-chip cache(s)

	/* XXX fixed load address */
	movl	#0x20100000, %a5

	movl	#0x20000000, %a0
	RELOC(edata, %a1)
1:
	movl	%a5@+, %a0@+
	cmpl	%a5, %a1
	bne	1b

	movl	#0x20000000, %a5

	ASRELOC(tmpstk, %a0)
	movl	%a0, %sp		| give ourselves a temporary stack

	RELOC(edata, %a0)
	RELOC(end, %a1)
2:
	clrb	%a0@+
	cmpl	%a0, %a1
	bne	2b

	RELOC(esym, %a0)
#if 0
	movl	%a4, %a0@		| store end of symbol table
#else
	clrl	%a0@			| no symbol table, yet
#endif

	RELOC(lowram, %a0)
	movl	%a5, %a0@		| store start of physical memory

#if 0
	RELOC(boothowto, %a0)		| save reboot flags
	movl	%d7, %a0@
	RELOC(bootdev, %a0)		|   and boot device
	movl	%d6, %a0@
#endif

	/*
	 * All data registers are now free.  All address registers
	 * except a5 are free.  a5 is used by the RELOC() macro,
	 * and cannot be used until after the MMU is enabled.
	 */

/* determine our CPU/MMU combo - check for all regardless of kernel config */
	movl	#0x200,%d0		| data freeze bit
	movc	%d0,%cacr		|   only exists on 68030
	movc	%cacr,%d0		| read it back
	tstl	%d0			| zero?
	jeq	Lnot68030		| yes, we have 68020/68040
	RELOC(mmutype, %a0)		| no, we have 68030
	movl	#MMU_68030,%a0@		| set to reflect 68030 PMMU
	RELOC(cputype, %a0)
	movl	#CPU_68030,%a0@		| and 68030 CPU
	jra	Lstart1
Lnot68030:
	bset	#31,%d0			| data cache enable bit
	movc	%d0,%cacr		|   only exists on 68040
	movc	%cacr,%d0		| read it back
	tstl	%d0			| zero?
	beq	Lis68020		| yes, we have 68020
	moveq	#0,%d0			| now turn it back off
	movec	%d0,%cacr		|   before we access any data
	RELOC(mmutype, %a0)
	movl	#MMU_68040,%a0@		| with a 68040 MMU
	RELOC(cputype, %a0)
	movl	#CPU_68040,%a0@		| and a 68040 CPU
	RELOC(fputype, %a0)
	movl	#FPU_68040,%a0@		| ...and FPU
	jra	Lstart1
Lis68020:
	/* impossible */

Lstart1:

/* initialize source/destination control registers for movs */
	moveq	#FC_USERD,%d0		| user space
	movc	%d0,%sfc		|   as source
	movc	%d0,%dfc		|   and destination of transfers

/* initialize memory size (for pmap_bootstrap) */
	movl	0x5c00ac00, %d0
	andb	#0x60, %d0
	jne	Lnot8M
	movl	#0x20800000, %d1	| memory end, 8M
	jra	Lmemok
Lnot8M:
	cmpb	#0x20, %d0
	jne	Lunkmem
	movl	#0x22000000, %d1	| memory end, 32M
	jra	Lmemok
Lunkmem:
	/* ??? */
	movl	#0x20400000, %d1	| memory end, assume at least 4M
	
Lmemok:
	moveq	#PGSHIFT,%d2
	lsrl	%d2,%d1			| convert to page (click) number
	movl	%a5,%d0			| lowram value from ROM via boot
	lsrl	%d2,%d0			| convert to page number
	subl	%d0,%d1			| compute amount of RAM present
	RELOC(physmem, %a0)
	movl	%d1,%a0@		| and physmem
/* configure kernel and lwp0 VA space so we can get going */
	.globl	_Sysseg_pa, _pmap_bootstrap, _avail_start
#if NKSYMS || defined(DDB) || defined(MODULAR)
	RELOC(esym,%a0)			| end of static kernel test/data/syms
	movl	%a0@,%d5
	jne	Lstart2
#endif
	movl	#_C_LABEL(end),%d5	| end of static kernel text/data
Lstart2:
	addl	#PAGE_SIZE-1,%d5
	andl	#PG_FRAME,%d5		| round to a page
	movl	%d5,%a4
	addl	%a5,%a4			| convert to PA
	subl	#KERNBASE, %a4
	pea	%a5@			| firstpa
	pea	%a4@			| nextpa
	RELOC(pmap_bootstrap,%a0)
	jbsr	%a0@			| pmap_bootstrap(firstpa, nextpa)
	addql	#8,%sp

/*
 * Prepare to enable MMU.
 */
	RELOC(Sysseg_pa, %a0)		| system segment table addr
	movl	%a0@,%d1		| read value (a PA)
	subl	#KERNBASE, %d1

	RELOC(mmutype, %a0)
	cmpl	#MMU_68040,%a0@		| 68040?
	jne	Lmotommu1		| no, skip
	.long	0x4e7b1807		| movc d1,srp
	jra	Lstploaddone
Lmotommu1:
#ifdef M68030
	RELOC(protorp, %a0)
	movl	%d1,%a0@(4)		| segtable address
	pmove	%a0@,%srp		| load the supervisor root pointer
#endif /* M68030 */
Lstploaddone:

	RELOC(mmutype, %a0)
	cmpl	#MMU_68040,%a0@		| 68040?
	jne	Lmotommu2		| no, skip

	movel #0x2000c000, %d0		| double map RAM
	.long	0x4e7b0004		| movc d0,itt0
	.long	0x4e7b0006		| movc d0,dtt0
	moveq	#0, %d0			| ensure TT regs are disabled
	.long	0x4e7b0005		| movc d0,itt1
	.long	0x4e7b0007		| movc d0,dtt1

	.word	0xf4d8			| cinva bc
	.word	0xf518			| pflusha

	movl	#MMU40_TCR_BITS, %d0
	.long	0x4e7b0003		| movc d0,tc
	movl	#0x80008000, %d0
	movc	%d0, %cacr		| turn on both caches

	jmp	Lenab1:l		| avoid pc-relative
Lmotommu2:
	/* XXX do TT here */
	pflusha
	RELOC(prototc, %a2)
	movl	#MMU51_TCR_BITS,%a2@	| value to load TC with
	pmove	%a2@,%tc		| load it
	jmp	Lenab1

/*
 * Should be running mapped from this point on
 */
Lenab1:
	.word	0xf4d8			| cinva bc
	.word	0xf518			| pflusha
	nop
	nop
	nop
	nop
	nop
	moveq	#0,%d0			| ensure TT regs are disabled
	.long	0x4e7b0004		| movc d0,itt0
	.long	0x4e7b0005		| movc d0,itt1
	.long	0x4e7b0006		| movc d0,dtt0
	.long	0x4e7b0007		| movc d0,dtt1

	lea	_ASM_LABEL(tmpstk),%sp	| re-load temporary stack
	jbsr	_C_LABEL(vec_init)	| initialize vector table
/* call final pmap setup */
	jbsr	_C_LABEL(pmap_bootstrap_finalize)
/* set kernel stack, user SP */
	movl	_C_LABEL(lwp0uarea),%a1	| get lwp0 uarea
	lea	%a1@(USPACE-4),%sp	| set kernel stack to end of area
	movl	#USRSTACK-4,%a2
	movl	%a2,%usp		| init user SP

	tstl	_C_LABEL(fputype)	| Have an FPU?
	jeq	Lenab2			| No, skip.
	clrl	%a1@(PCB_FPCTX)		| ensure null FP context
	movl	%a1,%sp@-
	jbsr	_C_LABEL(m68881_restore)   | restore it (does not kill a1)
	addql	#4,%sp
Lenab2:

/* flush TLB and turn on caches */
	jbsr	_C_LABEL(_TBIA)		| invalidate TLB
	cmpl	#MMU_68040,_C_LABEL(mmutype)	| 68040?
	jeq	Lnocache0		| yes, cache already on
	movl	#CACHE_ON,%d0
	movc	%d0,%cacr		| clear cache(s)
Lnocache0:

/* Final setup for call to main(). */
	jbsr	_C_LABEL(fic_init)

/*
 * Create a fake exception frame so that cpu_lwp_fork() can copy it.
 * main() nevers returns; we exit to user mode from a forked process
 * later on.
 */
	clrw	%sp@-			| vector offset/frame type
	clrl	%sp@-			| PC - filled in by "execve"
	movw	#PSL_USER,%sp@-		| in user mode
	clrl	%sp@-			| stack adjust count and padding
	lea	%sp@(-64),%sp		| construct space for D0-D7/A0-A7
	lea	_C_LABEL(lwp0),%a0	| save pointer to frame
	movl	%sp,%a0@(L_MD_REGS)	|   in lwp0.l_md.md_regs

	jra	_C_LABEL(main)		| main()

	pea	Lmainreturned		| Yow!  Main returned!
	jbsr	_C_LABEL(panic)
	/* NOTREACHED */
Lmainreturned:
	.asciz	"main() returned"
	.even

/*
 * Trap/interrupt vector routines
 */ 
#include <m68k/m68k/trap_subr.s>

/*
 * Use common m68k bus error and address error handlers.
 */
#include <m68k/m68k/busaddrerr.s>

/*
 * FP exceptions.
 */
ENTRY_NOPROFILE(fpfline)
#if defined(M68040)
	cmpl	#FPU_68040,_C_LABEL(fputype) | 68040 FPU?
	jne	Lfp_unimp		| no, skip FPSP
	cmpw	#0x202c,%sp@(6)		| format type 2?
	jne	_C_LABEL(illinst)	| no, not an FP emulation
Ldofp_unimp:
#ifdef FPSP
#if 0
	addl	#1, _C_LABEL(evcnt_fpsp_unimp)+EVCNT_COUNT
#endif
	jmp	_ASM_LABEL(fpsp_unimp)	| yes, go handle it
#endif
Lfp_unimp:
#endif /* M68040 */
#ifdef FPU_EMULATE
	clrl	%sp@-			| stack adjust count
	moveml	#0xFFFF,%sp@-		| save registers
	moveq	#T_FPEMULI,%d0		| denote as FP emulation trap
	jra	_ASM_LABEL(fault)	| do it
#else
	jra	_C_LABEL(illinst)
#endif

ENTRY_NOPROFILE(fpunsupp)
#if defined(M68040)
	cmpl	#FPU_68040,_C_LABEL(fputype) | 68040 FPU?
	jne	_C_LABEL(illinst)	| no, treat as illinst
#ifdef FPSP
#if 0
	addl	#1, _C_LABEL(evcnt_fpsp_unsupp)+EVCNT_COUNT
#endif
	jmp	_ASM_LABEL(fpsp_unsupp)	| yes, go handle it
#endif
Lfp_unsupp:
#endif /* M68040 */
#ifdef FPU_EMULATE
	clrl	%sp@-			| stack adjust count
	moveml	#0xFFFF,%sp@-		| save registers
	moveq	#T_FPEMULD,%d0		| denote as FP emulation trap
	jra	_ASM_LABEL(fault)	| do it
#else
	jra	_C_LABEL(illinst)
#endif

/*
 * Handles all other FP coprocessor exceptions.
 * Note that since some FP exceptions generate mid-instruction frames
 * and may cause signal delivery, we need to test for stack adjustment
 * after the trap call.
 */
ENTRY_NOPROFILE(fpfault)
	clrl	%sp@-		| stack adjust count
	moveml	#0xFFFF,%sp@-	| save user registers
	movl	%usp,%a0		| and save
	movl	%a0,%sp@(FR_SP)	|   the user stack pointer
	clrl	%sp@-		| no VA arg
	movl	_C_LABEL(curpcb),%a0 | current pcb
	lea	%a0@(PCB_FPCTX),%a0 | address of FP savearea
	fsave	%a0@		| save state
#if defined(M68040) || defined(M68060)
	/* always null state frame on 68040, 68060 */
	cmpl	#FPU_68040,_C_LABEL(fputype)
	jge	Lfptnull
#endif
	tstb	%a0@		| null state frame?
	jeq	Lfptnull	| yes, safe
	clrw	%d0		| no, need to tweak BIU
	movb	%a0@(1),%d0	| get frame size
	bset	#3,%a0@(0,%d0:w)	| set exc_pend bit of BIU
Lfptnull:
	fmovem	%fpsr,%sp@-	| push %fpsr as code argument
	frestore %a0@		| restore state
	movl	#T_FPERR,%sp@-	| push type arg
	jra	_ASM_LABEL(faultstkadj) | call trap and deal with stack cleanup


ENTRY_NOPROFILE(badtrap)
	moveml	#0xC0C0,%sp@-		| save scratch regs
	movw	%sp@(22),%sp@-		| push exception vector info
	clrw	%sp@-
	movl	%sp@(22),%sp@-		| and PC
	jbsr	_C_LABEL(straytrap)	| report
	addql	#8,%sp			| pop args
	moveml	%sp@+,#0x0303		| restore regs
	jra	_ASM_LABEL(rei)		| all done

ENTRY_NOPROFILE(trap0)
	clrl	%sp@-			| stack adjust count
	moveml	#0xFFFF,%sp@-		| save user registers
	movl	%usp,%a0			| save the user SP
	movl	%a0,%sp@(FR_SP)		|   in the savearea
	movl	%d0,%sp@-			| push syscall number
	jbsr	_C_LABEL(syscall)	| handle it
	addql	#4,%sp			| pop syscall arg
	tstl	_C_LABEL(astpending)
	jne	Lrei2
	tstb	_C_LABEL(ssir)
	jeq	Ltrap1
	movw	#SPL1,%sr
	tstb	_C_LABEL(ssir)
	jne	Lsir1
Ltrap1:	
	movl	%sp@(FR_SP),%a0		| grab and restore
	movl	%a0,%usp			|   user SP
	moveml	%sp@+,#0x7FFF		| restore most registers
	addql	#8,%sp			| pop SP and stack adjust
	rte

/*
 * Trap 12 is the entry point for the cachectl "syscall" (both HPUX & BSD)
 *	cachectl(command, addr, length)
 * command in d0, addr in a1, length in d1
 */
ENTRY_NOPROFILE(trap12)
	movl	_C_LABEL(curlwp),%a0
	movl	%a0@(L_PROC),%sp@-	| push current proc pointer
	movl	%d1,%sp@-			| push length
	movl	%a1,%sp@-			| push addr
	movl	%d0,%sp@-			| push command
	jbsr	_C_LABEL(cachectl1)	| do it
	lea	%sp@(16),%sp		| pop args
	jra	_ASM_LABEL(rei)		| all done

/*
 * Trace (single-step) trap.  Kernel-mode is special.
 * User mode traps are simply passed on to trap().
 */
ENTRY_NOPROFILE(trace)
	clrl	%sp@-			| stack adjust count
	moveml	#0xFFFF,%sp@-
	moveq	#T_TRACE,%d0

	| Check PSW and see what happen.
	|   T=0 S=0	(should not happen)
	|   T=1 S=0	trace trap from user mode
	|   T=0 S=1	trace trap on a trap instruction
	|   T=1 S=1	trace trap from system mode (kernel breakpoint)

	movw	%sp@(FR_HW),%d1		| get PSW
	notw	%d1			| XXX no support for T0 on 680[234]0
	andw	#PSL_TS,%d1		| from system mode (T=1, S=1)?
	jeq	Lkbrkpt			| yes, kernel breakpoint
	jra	_ASM_LABEL(fault)	| no, user-mode fault


/*
 * Trap 15 is used for:
 *	- GDB breakpoints (in user programs)
 *	- KGDB breakpoints (in the kernel)
 *	- trace traps for SUN binaries (not fully supported yet)
 * User mode traps are simply passed to trap().
 */
ENTRY_NOPROFILE(trap15)
	clrl	%sp@-			| stack adjust count
	moveml	#0xFFFF,%sp@-
	moveq	#T_TRAP15,%d0
	movw	%sp@(FR_HW),%d1		| get PSW
	andw	#PSL_S,%d1		| from system mode?
	jne	Lkbrkpt			| yes, kernel breakpoint
	jra	_ASM_LABEL(fault)	| no, user-mode fault

Lkbrkpt: | Kernel-mode breakpoint or trace trap. (%d0=trap_type)
	| Save the system sp rather than the user sp.
	movw	#PSL_HIGHIPL,%sr		| lock out interrupts
	lea	%sp@(FR_SIZE),%a6		| Save stack pointer
	movl	%a6,%sp@(FR_SP)		|  from before trap

	| If were are not on tmpstk switch to it.
	| (so debugger can change the stack pointer)
	movl	%a6,%d1
	cmpl	#_ASM_LABEL(tmpstk),%d1
	jls	Lbrkpt2			| already on tmpstk
	| Copy frame to the temporary stack
	movl	%sp,%a0			| %a0=src
	lea	_ASM_LABEL(tmpstk)-96,%a1 | a1=dst
	movl	%a1,%sp			| %sp=new frame
	moveq	#FR_SIZE,%d1
Lbrkpt1:
	movl	%a0@+,%a1@+
	subql	#4,%d1
	bgt	Lbrkpt1

Lbrkpt2:
	| Call the trap handler for the kernel debugger.
	| Do not call trap() to do it, so that we can
	| set breakpoints in trap() if we want.  We know
	| the trap type is either T_TRACE or T_BREAKPOINT.
	movl	%d0,%sp@-		| push trap type
	jbsr	_C_LABEL(trap_kdebug)
	addql	#4,%sp			| pop args

	| The stack pointer may have been modified, or
	| data below it modified (by kgdb push call),
	| so push the hardware frame at the current sp
	| before restoring registers and returning.

	movl	%sp@(FR_SP),%a0		| modified %sp
	lea	%sp@(FR_SIZE),%a1		| end of our frame
	movl	%a1@-,%a0@-		| copy 2 longs with
	movl	%a1@-,%a0@-		| ... predecrement
	movl	%a0,%sp@(FR_SP)		| %sp = h/w frame
	moveml	%sp@+,#0x7FFF		| restore all but %sp
	movl	%sp@,%sp			| ... and %sp
	rte				| all done

/*
 * Interrupt handlers.
 */

ENTRY_NOPROFILE(lev6intr)	/* Level 6: clock */
	INTERRUPT_SAVEREG
	/* XXX */
	movl _C_LABEL(clockbase), %a0
	movl %a0@, %d0
	movl %d0, %a0@
	btst #2, %d0
	jeq 1f
	addql	#1,_C_LABEL(m68k_intr_evcnt)+CLOCK_INTRCNT
	lea	%sp@(0), %a1		| a1 = &clockframe
	movl	%a1, %sp@-
	jbsr	_C_LABEL(hardclock)	| hardclock(&frame)
	addql	#4, %sp
	jra 2f
1:
	movl	%d0, %sp@-
	jbsr	_C_LABEL(otherclock)
	addql	#4, %sp
2:
	INTERRUPT_RESTOREREG
	jra	_ASM_LABEL(rei)		| all done

ENTRY_NOPROFILE(lev7intr)	/* level 7: parity errors, reset key */
	addql	#1,_C_LABEL(m68k_intr_evcnt)+NMI_INTRCNT
	clrl	%sp@-
	moveml	#0xFFFF,%sp@-		| save registers
	movl	%usp,%a0			| and save
	movl	%a0,%sp@(FR_SP)		|   the user stack pointer
	jbsr	_C_LABEL(nmihand)	| call handler
	movl	%sp@(FR_SP),%a0		| restore
	movl	%a0,%usp			|   user SP
	moveml	%sp@+,#0x7FFF		| and remaining registers
	addql	#8,%sp			| pop SP and stack adjust
	jra	_ASM_LABEL(rei)		| all done

/*
 * Emulation of VAX REI instruction.
 *
 * This code deals with checking for and servicing ASTs
 * (profiling, scheduling) and software interrupts (network, softclock).
 * We check for ASTs first, just like the VAX.  To avoid excess overhead
 * the T_ASTFLT handling code will also check for software interrupts so we
 * do not have to do it here.  After identifying that we need an AST we
 * drop the IPL to allow device interrupts.
 *
 * This code is complicated by the fact that sendsig may have been called
 * necessitating a stack cleanup.
 */
BSS(ssir,1)

ASENTRY_NOPROFILE(rei)
	tstl	_C_LABEL(astpending)	| AST pending?
	jeq	Lchksir			| no, go check for SIR
Lrei1:
	btst	#5,%sp@			| yes, are we returning to user mode?
	jne	Lchksir			| no, go check for SIR
	movw	#PSL_LOWIPL,%sr		| lower SPL
	clrl	%sp@-			| stack adjust
	moveml	#0xFFFF,%sp@-		| save all registers
	movl	%usp,%a1			| including
	movl	%a1,%sp@(FR_SP)		|    the users SP
Lrei2:
	clrl	%sp@-			| VA == none
	clrl	%sp@-			| code == none
	movl	#T_ASTFLT,%sp@-		| type == async system trap
	pea	%sp@(12)		| fp == address of trap frame
	jbsr	_C_LABEL(trap)		| go handle it
	lea	%sp@(16),%sp		| pop value args
	movl	%sp@(FR_SP),%a0		| restore user SP
	movl	%a0,%usp			|   from save area
	movw	%sp@(FR_ADJ),%d0		| need to adjust stack?
	jne	Laststkadj		| yes, go to it
	moveml	%sp@+,#0x7FFF		| no, restore most user regs
	addql	#8,%sp			| toss SP and stack adjust
	rte				| and do real RTE
Laststkadj:
	lea	%sp@(FR_HW),%a1		| pointer to HW frame
	addql	#8,%a1			| source pointer
	movl	%a1,%a0			| source
	addw	%d0,%a0			|  + hole size = dest pointer
	movl	%a1@-,%a0@-		| copy
	movl	%a1@-,%a0@-		|  8 bytes
	movl	%a0,%sp@(FR_SP)		| new SSP
	moveml	%sp@+,#0x7FFF		| restore user registers
	movl	%sp@,%sp			| and our SP
	rte				| and do real RTE
Lchksir:
	tstb	_C_LABEL(ssir)		| SIR pending?
	jeq	Ldorte			| no, all done
	movl	%d0,%sp@-			| need a scratch register
	movw	%sp@(4),%d0		| get SR
	andw	#PSL_IPL7,%d0		| mask all but IPL
	jne	Lnosir			| came from interrupt, no can do
	movl	%sp@+,%d0			| restore scratch register
Lgotsir:
	movw	#SPL1,%sr		| prevent others from servicing int
	tstb	_C_LABEL(ssir)		| too late?
	jeq	Ldorte			| yes, oh well...
	clrl	%sp@-			| stack adjust
	moveml	#0xFFFF,%sp@-		| save all registers
	movl	%usp,%a1			| including
	movl	%a1,%sp@(FR_SP)		|    the users SP
Lsir1:	
	clrl	%sp@-			| VA == none
	clrl	%sp@-			| code == none
	movl	#T_SSIR,%sp@-		| type == software interrupt
	pea	%sp@(12)		| fp == address of trap frame
	jbsr	_C_LABEL(trap)		| go handle it
	lea	%sp@(16),%sp		| pop value args
	movl	%sp@(FR_SP),%a0		| restore
	movl	%a0,%usp			|   user SP
	moveml	%sp@+,#0x7FFF		| and all remaining registers
	addql	#8,%sp			| pop SP and stack adjust
	rte
Lnosir:
	movl	%sp@+,%d0			| restore scratch register
Ldorte:
	rte				| real return

/*
 * Primitives
 */ 

/*
 * Use common m68k process/lwp switch and context save subroutines.
 */
#define	FPCOPROC	/* XXX: Temporarily required */
#include <m68k/m68k/switch_subr.s>


#if defined(M68040)
ENTRY(suline)
	movl	%sp@(4),%a0		| address to write
	movl	_C_LABEL(curpcb),%a1	| current pcb
	movl	#Lslerr,%a1@(PCB_ONFAULT) | where to return to on a fault
	movl	%sp@(8),%a1		| address of line
	movl	%a1@+,%d0			| get lword
	movsl	%d0,%a0@+			| put lword
	nop				| sync
	movl	%a1@+,%d0			| get lword
	movsl	%d0,%a0@+			| put lword
	nop				| sync
	movl	%a1@+,%d0			| get lword
	movsl	%d0,%a0@+			| put lword
	nop				| sync
	movl	%a1@+,%d0			| get lword
	movsl	%d0,%a0@+			| put lword
	nop				| sync
	moveq	#0,%d0			| indicate no fault
	jra	Lsldone
Lslerr:
	moveq	#-1,%d0
Lsldone:
	movl	_C_LABEL(curpcb),%a1	| current pcb
	clrl	%a1@(PCB_ONFAULT) 	| clear fault address
	rts
#endif

/*
 * Set processor priority level calls.  Most are implemented with
 * inline asm expansions.  However, spl0 requires special handling
 * as we need to check for our emulated software interrupts.
 */

ENTRY(spl0)
	moveq	#0,%d0
	movw	%sr,%d0			| get old SR for return
	movw	#PSL_LOWIPL,%sr		| restore new SR
	tstb	_C_LABEL(ssir)		| software interrupt pending?
	jeq	Lspldone		| no, all done
	subql	#4,%sp			| make room for RTE frame
	movl	%sp@(4),%sp@(2)		| position return address
	clrw	%sp@(6)			| set frame type 0
	movw	#PSL_LOWIPL,%sp@		| and new SR
	jra	Lgotsir			| go handle it
Lspldone:
	rts

/*
 * _delay(u_int N)
 *
 * Delay for at least (N/256) microseconds.
 * This routine depends on the variable:  delay_divisor
 * which should be set based on the CPU clock rate.
 */
ENTRY_NOPROFILE(_delay)
	| d0 = arg = (usecs << 8)
	movl	%sp@(4),%d0
	| d1 = delay_divisor
	movl	_C_LABEL(delay_divisor),%d1
L_delay:
	subl	%d1,%d0
	jgt	L_delay
	rts

ENTRY_NOPROFILE(doboot)
	movl #0x5c00c060, %d0		| want phys addressing
	.long	0x4e7b0006		| movc d0,dtt0
	movl	#1, 0x5c00b800		| reset
	stop	#0x2700			| paranoia

	.data
GLOBAL(mmutype)
	.long	MMU_HP		| default to HP MMU
GLOBAL(cputype)
	.long	CPU_68020	| default to 68020 CPU
GLOBAL(fputype)
	.long	FPU_68881	| default to 68881 FPU
GLOBAL(prototc)
	.long	0		| prototype translation control

#ifdef DEBUG
	.globl	fulltflush, fullcflush
fulltflush:
	.long	0
fullcflush:
	.long	0
#endif
