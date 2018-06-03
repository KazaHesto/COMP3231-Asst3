#include <types.h>
#include <kern/errno.h>
#include <lib.h>
#include <thread.h>
#include <addrspace.h>
#include <vm.h>

/* Place your frametable data-structures here
 * You probably also want to write a frametable initialisation
 * function and call it from vm_bootstrap
 */

struct ft_entry {
	int state;
};

static struct spinlock stealmem_lock = SPINLOCK_INITIALIZER;

static struct ft_entry *f_table;
static unsigned int first_index;
static unsigned int last_index;
static unsigned int free_index;

static paddr_t first_addr;

/* Initialization function */
void ft_bootstrap(void)
{
	unsigned int num_frames = ram_getsize() / PAGE_SIZE;

	f_table = kmalloc(sizeof(struct ft_entry) * (num_frames));
	first_addr = ram_getfirstfree();
	first_index = first_addr / PAGE_SIZE;
	last_index = num_frames - 1;
	free_index = first_index;

	for (unsigned int i = 0; i <= last_index; i++) {
		if (i < first_index) {
			// mark frames as not available
			f_table[i].state = -1;
		} else {
			// mark frames as free
			f_table[i].state = 0;
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

	spinlock_acquire(&stealmem_lock);
	if (free_index == 0) {
		// no free memory
		spinlock_release(&stealmem_lock);
		return 0;
	}

	addr = PAGE_SIZE * free_index;
	// set memory as allocated
	f_table[free_index].state = 1;

	unsigned int i = free_index;
	free_index = 0;
	for (; i <= last_index; i++) {
		if (f_table[i].state == 0) {
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
	if (f_table[addr_index].state != 1) {
		// frame can't be freed
		spinlock_release(&stealmem_lock);
		return;
	}

	f_table[addr_index].state = 0;
	if (addr_index < free_index) {
		// freed frame is before current first free frame
		free_index = addr_index;
	}
	spinlock_release(&stealmem_lock);
}

