#include <types.h>
#include <kern/errno.h>
#include <lib.h>
#include <thread.h>
#include <addrspace.h>
#include <vm.h>
#include <machine/tlb.h>
#include <proc.h>
#include <current.h>
#include <spl.h>

// used for hash
#define PAGE_BITS  12

static struct spinlock pagetable_lock = SPINLOCK_INITIALIZER;

/* Page Table Entry */
struct PTE {
	int write;			// write permission bit
	vaddr_t page;
	vaddr_t frame;
	struct addrspace *pid;
	uint32_t next;
};

static struct PTE *pagetable;
static uint32_t num_pages;

uint32_t hpt_hash(struct addrspace *as, vaddr_t faultaddr);
uint32_t hpt_indexof(struct addrspace *as, vaddr_t faultaddr);

void
vm_bootstrap(void)
{
	/* page table initialisation */
	num_pages = ram_getsize() / PAGE_SIZE;
	pagetable = kmalloc(sizeof(struct PTE) * num_pages);
	for (uint32_t i = 0; i < num_pages; i++) {
		pagetable[i].write = 0;
		pagetable[i].page = 0;
		pagetable[i].frame = 0;
		pagetable[i].pid = NULL;
		pagetable[i].next = num_pages;
	}

	ft_bootstrap();
}

int
vm_fault(int faulttype, vaddr_t faultaddress)
{
	uint32_t ehi, elo;
	faultaddress &= PAGE_FRAME;

	DEBUG(DB_VM, "vm.c: fault: 0x%x\n", faultaddress);

	switch (faulttype) {
		case VM_FAULT_READONLY:
		return EFAULT;
		case VM_FAULT_READ:
		case VM_FAULT_WRITE:
		break;
		default:
		return EINVAL;
	}

	if (curproc == NULL) {
		/*
		 * No process. This is probably a kernel fault early
		 * in boot. Return EFAULT so as to panic instead of
		 * getting into an infinite faulting loop.
		 */
		return EFAULT;
	}

	struct addrspace *as = proc_getas();
	if (as == NULL) {
		/*
		 * No address space set up. This is probably also a
		 * kernel fault early in boot.
		 */
		return EFAULT;
	}

	// check which region the address is in and the
	// corresponding permissions
	struct region *cur_region = as->start;
	while (cur_region != NULL) {
		if (faultaddress >= cur_region->base) {
			if (faultaddress - cur_region->base < cur_region->size) {
				break;
			}
		}
		cur_region = cur_region->next;
	}

	if (cur_region == NULL) {
		// no region matching the faultaddress
		return EFAULT;
	}


	spinlock_acquire(&pagetable_lock);
	// find matching entry in page table
	uint32_t index = hpt_indexof(as, faultaddress);

	if (index == num_pages) {
		// no space remaining
		return ENOMEM;
	}

	if (pagetable[index].pid == NULL) {
		// no entry in page table yet
		pagetable[index].write = cur_region->write;
		pagetable[index].page = faultaddress;
		pagetable[index].frame = alloc_kpages(1);
		pagetable[index].pid = as;
		pagetable[index].next = num_pages;
	}

	paddr_t paddr = KVADDR_TO_PADDR(pagetable[index].frame);
	spinlock_release(&pagetable_lock);

	/* make sure it's page-aligned */
	KASSERT((paddr & PAGE_FRAME) == paddr);

	/* Disable interrupts on this CPU while frobbing the TLB. */
	int spl = splhigh();

	for (int i = 0; i < NUM_TLB; i++) {
		tlb_read(&ehi, &elo, i);
		if (elo & TLBLO_VALID) {
			continue;
		}
		ehi = faultaddress;
		elo = paddr | TLBLO_VALID;
		if (cur_region->write) {
			elo |= TLBLO_DIRTY;
		}
		DEBUG(DB_VM, "vm.c: 0x%x -> 0x%x\n", faultaddress, paddr);
		tlb_write(ehi, elo, i);
		splx(spl);
		return 0;
	}

	kprintf("vm.c: Ran out of TLB entries - cannot handle page fault\n");
	splx(spl);
	return EFAULT;
}

// frees frames in use by a given process
void
vm_freeproc(void)
{
	if (curproc == NULL) {
		/*
		 * No process. This is probably a kernel fault early
		 * in boot.
		 */
		return;
	}

	struct addrspace *as = proc_getas();
	if (as == NULL) {
		/*
		 * No address space set up. This is probably also a
		 * kernel fault early in boot.
		 */
		return;
	}

	uint32_t ehi, elo;

	spinlock_acquire(&pagetable_lock);
	for (uint32_t i = 0; i < num_pages; i++) {
		if (pagetable[i].pid == as) {
			free_kpages(pagetable[i].frame);

			/* Disable interrupts on this CPU while frobbing the TLB. */
			int spl = splhigh();
			// invalidate the tlb entry for page being deleted
			for (int k = 0; k < NUM_TLB; k++) {
				tlb_read(&ehi, &elo, k);
				if (elo & (TLBLO_VALID | pagetable[i].frame)) {
					tlb_write(TLBHI_INVALID(k), TLBLO_INVALID(), k);
					break;
				}
			}
			splx(spl);

			uint32_t gap = i;
			pagetable[gap].write = 0;
			pagetable[gap].page  = 0;
			pagetable[gap].frame = 0;
			pagetable[gap].pid   = NULL;
			pagetable[gap].next  = num_pages;

			// shift pages further on in hash table
			for (uint32_t j = i + 1; pagetable[j].pid != NULL; j++) {
				// all page entries should be page aligned
				KASSERT((pagetable[j].frame & PAGE_FRAME) == pagetable[j].frame);
				if (j == num_pages) {
					// reached end of array, loop back to beginning
					j = 0;
					continue;
				}
				if (j == i || pagetable[j].pid != NULL) {
					// looped through whole array or free slot found, everything has been shifted
					return;
				}
				if (hpt_indexof(as, pagetable[j].frame) == gap) {
					// move page entry to the gap
					pagetable[gap].write = pagetable[j].write;
					pagetable[gap].page  = pagetable[j].page;
					pagetable[gap].frame = pagetable[j].frame;
					pagetable[gap].pid   = pagetable[j].pid;
					pagetable[gap].next  = pagetable[j].next;

					pagetable[j].write = 0;
					pagetable[j].page  = 0;
					pagetable[j].frame = 0;
					pagetable[j].pid   = NULL;
					pagetable[j].next  = num_pages;

					gap = j;
				}
			}
		}
	}
	spinlock_release(&pagetable_lock);
}

uint32_t
hpt_hash(struct addrspace *as, vaddr_t faultaddr)
{
	uint32_t index;

	index = (((uint32_t )as) ^ (faultaddr >> PAGE_BITS)) % num_pages;
	return index;
}

// returns the index of the pte for a given address, or finds the closest empty
// slot and adds it to the next chain
uint32_t
hpt_indexof(struct addrspace *as, vaddr_t faultaddr)
{
	uint32_t index = hpt_hash(as, faultaddr);
	if (pagetable[index].pid == NULL) {
		// hash location is free, page not allocated and free space available
		return index;
	}
	while (pagetable[index].pid != as || pagetable[index].page != faultaddr) {
		if (pagetable[index].next == num_pages) {
			// page not found, finding next free space to add new page
			for (uint32_t j = index + 1; j <= num_pages; j++) {
				if (j == num_pages) {
					// reached end of array, loop back to beginning
					j = 0;
					continue;
				}
				if (j == index) {
					// looped through whole array, no page space left
					return num_pages;
				}
				if (pagetable[j].pid == NULL) {
					// space found, update chain to point to it
					pagetable[index].next = j;
					index = j;
					break;
				}
			}
		}
		index = pagetable[index].next;
	}
	return index;
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

