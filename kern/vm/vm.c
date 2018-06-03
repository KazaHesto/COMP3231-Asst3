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

void vm_bootstrap(void)
{
	/* page table initialisation */
	num_pages = ram_getsize() / PAGE_SIZE;
	pagetable = kmalloc(sizeof(struct PTE) * num_pages);
	for (uint32_t i = 0; i < num_pages; i++) {
		pagetable[i].write = 0;
		pagetable[i].page = 0;
		pagetable[i].frame = 0;
		pagetable[i].pid = NULL;
		pagetable[i].next = 0;
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

	// find matching entry in page table
	uint32_t index = hpt_hash(as, faultaddress);
	spinlock_acquire(&pagetable_lock);
	while (pagetable[index].pid != as || pagetable[index].page != faultaddress) {
		if (pagetable[index].next == 0) {
			for (uint32_t j = index; j < num_pages; j++) {
				if (pagetable[j].pid == NULL) {
					pagetable[index].next = j;
					index = j;
				}
			}
		}
		index = pagetable[index].next;
	}

	if (pagetable[index].pid == NULL) {
		// no entry in page table yet
		pagetable[index].write = cur_region->write;
		pagetable[index].page = faultaddress;
		pagetable[index].frame = alloc_kpages(1);
		pagetable[index].pid = as;
		pagetable[index].next = 0;
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

uint32_t hpt_hash(struct addrspace *as, vaddr_t faultaddr)
{
	uint32_t index;

	index = (((uint32_t )as) ^ (faultaddr >> PAGE_BITS)) % num_pages;
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

