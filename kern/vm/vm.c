#include <types.h>
#include <kern/errno.h>
#include <lib.h>
#include <thread.h>
#include <addrspace.h>
#include <vm.h>
#include <machine/tlb.h>

#define NUMPAGES 16

struct PTE *pagetable;
/* Place your page table functions here */

void vm_bootstrap(void)
{
	ft_bootstrap();

	/* page table initialisation */
	pt_bootstrap();
        
}

int
vm_fault(int faulttype, vaddr_t faultaddress)
{
	(void) faulttype;
	(void) faultaddress;

	panic("vm_fault hasn't been written yet\n");

	return EFAULT;
}

/*
 *
 * SMP-specific functions.  Unused in our configuration.
 */

void
vm_tlbshootdown(const struct tlbshootdown *ts)
{
	(void)ts;
	panic("vm tried to do tlb shootdown?!\n");
}

void pt_bootstrap(void)
{
	pagetable = kmalloc(sizeof(struct PTE) * NUMPAGES);
	
	for (int i = 0; i < NUMPAGES; i++) {
		pagetable[i].cached = false;
		pagetable[i].referenced = false;
		pagetable[i].modified = false;
		pagetable[i].read = false;
		pagetable[i].write = false;
		pagetable[i].exec = false;
		pagetable[i].valid = false;
		pagetable[i].frameno = -1;		// not mapped to any frame
		pagetable[i].pid = -1;
	}
	
}

void pt_insert(struct PTE pte)
{
	(void) pte;
}

struct PTE pt_lookup(int pindex)
{
	(void) pindex;
	struct PTE tmp;
	return tmp;
}

void pt_update(int pindex, struct PTE pte)
{
	(void) pindex;
	(void) pte;
}