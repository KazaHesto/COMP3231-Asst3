#include <types.h>
#include <kern/errno.h>
#include <lib.h>
#include <thread.h>
#include <addrspace.h>
#include <vm.h>

// frame table entry states
#define FRAME_FREE       0
#define FRAME_USED       1
#define FRAME_LOCKED    -1

struct ft_entry {
	int state;
};

static struct spinlock stealmem_lock = SPINLOCK_INITIALIZER;

static struct ft_entry *f_table = NULL;
static unsigned int num_frames;
static unsigned int free_index;

/* Initialization function */
void ft_bootstrap(void)
{
	num_frames = ram_getsize() / PAGE_SIZE;
	f_table = kmalloc(sizeof(struct ft_entry) * (num_frames));
	unsigned int first_index = ram_getfirstfree() / PAGE_SIZE;
	free_index = first_index;

	for (unsigned int i = 0; i < num_frames; i++) {
		if (i < first_index) {
			// mark frames as not available
			f_table[i].state = FRAME_LOCKED;
		} else {
			// mark frames as free
			f_table[i].state = FRAME_FREE;
		}
	}
}

/* Note that this function returns a VIRTUAL address, not a physical
 * address
 * WARNING: this function gets called very early, before
 * vm_bootstrap().  You may wish to modify main.c to call your
 * frame table initialisation function, or check to see if the
 * frame table has been initialised and call ram_stealmem() otherwise.
 */

vaddr_t alloc_kpages(unsigned int npages)
{
	paddr_t addr;
	if (f_table == NULL) {
		// using ram_stealmem() while frametable isn't ready
		spinlock_acquire(&stealmem_lock);
		addr = ram_stealmem(npages);
		spinlock_release(&stealmem_lock);
		if (addr == 0) {
			return 0;
		}
		return PADDR_TO_KVADDR(addr);
	}

	if (npages != 1) {
		// allocator only works for 1 page alloc
		return 0;
	}

	spinlock_acquire(&stealmem_lock);
	if (free_index == num_frames) {
		// no free memory
		spinlock_release(&stealmem_lock);
		return 0;
	}

	addr = free_index * PAGE_SIZE;
	// set memory as allocated
	f_table[free_index].state = FRAME_USED;

	unsigned int i = free_index;
	free_index = num_frames;
	for (; i < num_frames; i++) {
		if (f_table[i].state == FRAME_FREE) {
			// update free_index to the next free frame
			free_index = i;
			break;
		}
	}
	spinlock_release(&stealmem_lock);

	bzero((void *)PADDR_TO_KVADDR(addr), PAGE_SIZE);
	return PADDR_TO_KVADDR(addr);
}

void free_kpages(vaddr_t addr)
{
	paddr_t paddr = KVADDR_TO_PADDR(addr);
	unsigned int addr_index = paddr / PAGE_SIZE;

	spinlock_acquire(&stealmem_lock);
	if (f_table[addr_index].state != FRAME_USED) {
		// frame can't be freed
		spinlock_release(&stealmem_lock);
		return;
	}

	f_table[addr_index].state = FRAME_FREE;
	if (addr_index < free_index) {
		// freed frame is before current first free frame or there
		// were no free frames
		free_index = addr_index;
	}
	spinlock_release(&stealmem_lock);
}

