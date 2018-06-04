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
	uint32_t pid;
	uint32_t next;
};

static struct PTE *pagetable;
static uint32_t num_pages;

uint32_t hpt_hash(uint32_t pid, vaddr_t faultaddr);
uint32_t hpt_indexof(uint32_t pid, vaddr_t faultaddr);

void
vm_bootstrap(void)
{
	/* page table initialisation */
	num_pages = ram_getsize() / PAGE_SIZE * 2;
	pagetable = kmalloc(sizeof(struct PTE) * num_pages);
	for (uint32_t i = 0; i < num_pages; i++) {
		pagetable[i].write = 0;
		pagetable[i].page = 0;
		pagetable[i].frame = 0;
		pagetable[i].pid = 0;
		pagetable[i].next = num_pages;
	}

	ft_bootstrap();
}

int
vm_fault(int faulttype, vaddr_t faultaddress)
{
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

	if (as->start == NULL) {
		/*
		 * No regions set up. This is probably also a
		 * kernel fault early in boot.
		 */
		return EFAULT;
	}

	int write = 0;
	// check which region the address is in and the
	// corresponding permissions
	struct region *cur_region = as->start;
	while (cur_region != NULL) {
		if (faultaddress >= cur_region->base) {
			if (faultaddress - cur_region->base < cur_region->size) {
				write = cur_region->write;
				break;
			}
		}
		cur_region = cur_region->next;
	}

	if (cur_region == NULL) {
		// no region matching the faultaddress
		if (faultaddress < as->stack_end && faultaddress > (as->start->base + as->start->size)) {
			write = 1;
		} else {
			return EFAULT;
		}
	}


	spinlock_acquire(&pagetable_lock);
	// find matching entry in page table
	uint32_t index = hpt_indexof((uint32_t) as, faultaddress);

	if (index == num_pages) {
		// no space remaining
		return ENOMEM;
	}

	if (pagetable[index].pid == 0) {
		// no entry in page table yet
		pagetable[index].write = write;
		pagetable[index].page = faultaddress;
		pagetable[index].frame = alloc_kpages(1);
		pagetable[index].pid = (uint32_t) as;
		pagetable[index].next = num_pages;
	}

	paddr_t paddr = KVADDR_TO_PADDR(pagetable[index].frame);
	spinlock_release(&pagetable_lock);

	/* make sure it's page-aligned */
	KASSERT((paddr & PAGE_FRAME) == paddr);

	/* Disable interrupts on this CPU while frobbing the TLB. */
	int spl = splhigh();
	int ehi = faultaddress;
	int elo = paddr | TLBLO_VALID;
	if (write) {
		elo |= TLBLO_DIRTY;
	}
	DEBUG(DB_VM, "vm.c: 0x%x -> 0x%x\n", faultaddress, paddr);
	tlb_random(ehi, elo);
	splx(spl);
	return 0;
}

// frees frames in use by a given process
void
vm_freeproc(uint32_t pid)
{
	if (pid == 0) {
		return;
	}
	spinlock_acquire(&pagetable_lock);
	for (uint32_t i = 0; i < num_pages; i++) {
		if (pagetable[i].pid == pid) {
			free_kpages(pagetable[i].frame);

			uint32_t gap = i;
			pagetable[gap].write = 0;
			pagetable[gap].page  = 0;
			pagetable[gap].frame = 0;
			pagetable[gap].pid   = 0;
			pagetable[gap].next  = num_pages;

			// shift pages further on in hash table
			for (uint32_t j = i + 1; pagetable[j].pid != 0; j++) {
				// all page entries should be page aligned
				KASSERT((pagetable[j].frame & PAGE_FRAME) == pagetable[j].frame);
				if (j == num_pages) {
					// reached end of array, loop back to beginning
					j = 0;
					continue;
				}
				if (j == i || pagetable[j].pid != 0) {
					// looped through whole array or free slot found, everything has been shifted
					spinlock_release(&pagetable_lock);
					return;
				}
				if (hpt_indexof(pid, pagetable[j].frame) == gap) {
					// move page entry to the gap
					pagetable[gap].write = pagetable[j].write;
					pagetable[gap].page  = pagetable[j].page;
					pagetable[gap].frame = pagetable[j].frame;
					pagetable[gap].pid   = pagetable[j].pid;
					pagetable[gap].next  = pagetable[j].next;

					pagetable[j].write = 0;
					pagetable[j].page  = 0;
					pagetable[j].frame = 0;
					pagetable[j].pid   = 0;
					pagetable[j].next  = num_pages;

					gap = j;
				}
			}
		}
	}
	spinlock_release(&pagetable_lock);
}

// clones the pages of a process to another's pid
int
vm_cloneproc(uint32_t oldpid, uint32_t newpid)
{
	if (oldpid == 0 || newpid == 0) {
		return EFAULT;
	}
	spinlock_acquire(&pagetable_lock);
	for (uint32_t i = 0; i < num_pages; i++) {
		if (pagetable[i].pid == oldpid) {
			uint32_t index = hpt_indexof(newpid, pagetable[i].page);
			if (index == num_pages) {
				spinlock_release(&pagetable_lock);
				vm_freeproc(newpid);
				return ENOMEM;
			}
			// copy page entry to the new page entry
			pagetable[index].write = pagetable[i].write;
			pagetable[index].page  = pagetable[i].page;
			pagetable[index].frame = alloc_kpages(1);
			pagetable[index].pid   = newpid;
			pagetable[index].next  = 0;

			if (memcpy((void *)pagetable[index].frame, (void *)pagetable[i].frame, PAGE_SIZE) == NULL) {
				spinlock_release(&pagetable_lock);
				vm_freeproc(newpid);
				return ENOMEM;
			}
		}
	}
	spinlock_release(&pagetable_lock);
	return 0;
}

uint32_t
hpt_hash(uint32_t pid, vaddr_t faultaddr)
{
	uint32_t index;

	index = ((pid) ^ (faultaddr >> PAGE_BITS)) % num_pages;
	return index;
}

// returns the index of the pte for a given address, or finds the closest empty
// slot and adds it to the next chain
uint32_t
hpt_indexof(uint32_t pid, vaddr_t faultaddr)
{
	uint32_t index = hpt_hash(pid, faultaddr);
	if (pagetable[index].pid == 0) {
		// hash location is free, page not allocated and free space available
		return index;
	}
	while (pagetable[index].pid != pid || pagetable[index].page != faultaddr) {
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
				if (pagetable[j].pid == 0) {
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

