/*
 * Copyright (c) 2000, 2001, 2002, 2003, 2004, 2005, 2008, 2009
 *        The President and Fellows of Harvard College.
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
 * THIS SOFTWARE IS PROVIDED BY THE UNIVERSITY AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE UNIVERSITY OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <types.h>
#include <kern/errno.h>
#include <lib.h>
#include <spl.h>
#include <spinlock.h>
#include <current.h>
#include <mips/tlb.h>
#include <addrspace.h>
#include <vm.h>
#include <proc.h>

#define NUMSTACK 16

/*
 * Note! If OPT_DUMBVM is set, as is the case until you start the VM
 * assignment, this file is not compiled or linked or in any way
 * used. The cheesy hack versions in dumbvm.c are used instead.
 *
 * UNSW: If you use ASST3 config as required, then this file forms
 * part of the VM subsystem.
 *
 */

struct addrspace *
as_create(void)
{
	struct addrspace *as;

	as = kmalloc(sizeof(struct addrspace));
	if (as == NULL) {
		return NULL;
	}

	as->stack_end = USERSTACK;
	as->start = NULL;

	return as;
}

int
as_copy(struct addrspace *old, struct addrspace **ret)
{
	struct addrspace *newas;

	newas = as_create();
	if (newas==NULL) {
		return ENOMEM;
	}

	newas->stack_end = old->stack_end;
	newas->start = NULL;

	// copying each region
	struct region *currOld = old->start;
	struct region *currNew = NULL;
	while (currOld != NULL) {
		struct region *new = kmalloc(sizeof(struct region));
		new->base = currOld->base;
		new->size = currOld->size;
		new->read = currOld->read;
		new->write = currOld->write;
		new->modified = currOld->modified;
		new->next = NULL;
		// is newas has no start
		if (newas->start == NULL) {
			newas->start = new;
			currNew = new;
			currOld = currOld->next;
			continue;
		}
		currNew->next = new;
		currNew = currNew->next;
		currOld = currOld->next;
	}

	*ret = newas;
	return 0;
}

void
as_destroy(struct addrspace *as)
{
	if (as == NULL) return;

	// freeing each region
	struct region *curr = as->start;
	while (curr != NULL) {
		struct region *del = curr;
		curr = curr->next;
		kfree(del);
	}

	kfree(as);
}

void
as_activate(void)
{
	/* Copied from dumbvm.c */
	int i, spl;
	struct addrspace *as;

	as = proc_getas();
	if (as == NULL) {
		return;
	}

	/* Disable interrupts on this CPU while frobbing the TLB. */
	spl = splhigh();

	for (i=0; i<NUM_TLB; i++) {
		tlb_write(TLBHI_INVALID(i), TLBLO_INVALID(), i);
	}

	splx(spl);
}

void
as_deactivate(void)
{
	/*
	 * Write this. For many designs it won't need to actually do
	 * anything. See proc.c for an explanation of why it (might)
	 * be needed.
	 */
}

/*
 * Set up a segment at virtual address VADDR of size MEMSIZE. The
 * segment in memory extends from VADDR up to (but not including)
 * VADDR+MEMSIZE.
 *
 * The READABLE, WRITEABLE, and EXECUTABLE flags are set if read,
 * write, or execute permission should be set on the segment. At the
 * moment, these are ignored. When you write the VM system, you may
 * want to implement them.
 */
int
as_define_region(struct addrspace *as, vaddr_t vaddr, size_t memsize,
                 int readable, int writeable, int executable)
{
	if (as == NULL) return EFAULT;
	if (vaddr + memsize >= as->stack_end) return ENOMEM;

	struct region *new = kmalloc(sizeof(struct region));
	if (new == NULL) {
		return ENOMEM;
	}

	new->base = vaddr;
	new->size = memsize;
	new->read = readable;
	new->write = writeable;

	// have the highest address region at the start
	if (new->base > as->start->base) {
		new->next = as->start;
		as->start = new;
	} else {
		new->next = as->start->next;
		as->start->next = new;
	}

	// unused
	(void) executable;

	return 0;
}

int
as_prepare_load(struct addrspace *as)
{
	if (as == NULL) return EFAULT;

	struct region *curr = as->start;
	while (curr != NULL) {
		// check if not writable
		if (curr->write == false) {
			curr->write = true;
			curr->modified = true;
		}
		curr = curr->next;
	}

	return 0;
}

int
as_complete_load(struct addrspace *as)
{
	if (as == NULL) return EFAULT;

	struct region *curr = as->start;
	while (curr != NULL) {
		// check if writable and modified
		if (curr->write == true && curr->modified == true) {
			curr->write = false;
			curr->modified = false;
		}
		curr = curr->next;
	}

	return 0;
}

int
as_define_stack(struct addrspace *as, vaddr_t *stackptr)
{
	if (as == NULL) return EFAULT;

	/* Initial user-level stack pointer */
	*stackptr = as->stack_end;

	return 0;
}

