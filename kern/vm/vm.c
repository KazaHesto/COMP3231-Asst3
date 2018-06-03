#include <types.h>
#include <kern/errno.h>
#include <lib.h>
#include <thread.h>
#include <addrspace.h>
#include <vm.h>
#include <machine/tlb.h>

#define NUMPAGES 16

struct PTE *pagetable;
bool pt_fullflag;
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
	if (faulttype == VM_FAULT_READONLY)
		return EFAULT;
	
	/*
	lookup pt
	if valid
		load tlb
	else 
		lookup region
		if valid
			alloc frame
			zero fill
			insert pte
			load tlb
		else EFAULT
	*/
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
	
	pt_fullflag = false;
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
	if (pt_fullflag == false) {
		for (int i = 0; i <= NUMPAGES; i++) {
			if (i == NUMPAGES) {
				// page table full
				pt_fullflag = true;
				break;
			}
			if (pagetable[i].frameno == -1) {
				pagetable[i].cached = pte.cached;
				pagetable[i].referenced = pte.referenced;
				pagetable[i].modified = pte.modified;
				pagetable[i].read = pte.read;
				pagetable[i].write = pte.write;
				pagetable[i].exec = pte.exec;
				pagetable[i].valid = pte.valid;
				pagetable[i].frameno = pte.frameno;
				pagetable[i].pid = pte.pid;
				break;
			}
		}
		return;
	}
	
	// Perform clock page replacement
	for (int i = 0; i < NUMPAGES; i++) {
		// replace first page with referenced bit equal false
		if (pagetable[i].referenced == false) {
			// reset other pages' referenced bit
			for (int j = 0; j < NUMPAGES; j++) {
				pagetable[j].referenced = false;
			}
			pagetable[i].cached = pte.cached;
			pagetable[i].referenced = pte.referenced;
			pagetable[i].modified = pte.modified;
			pagetable[i].read = pte.read;
			pagetable[i].write = pte.write;
			pagetable[i].exec = pte.exec;
			pagetable[i].valid = pte.valid;
			pagetable[i].frameno = pte.frameno;
			pagetable[i].pid = pte.pid;
			return;
		}
	}
	
}

struct PTE pt_lookup(int pindex)
{
	return pagetable[pindex];
}

void pt_update(int pindex, struct PTE pte)
{
	pagetable[pindex] = pte;
}