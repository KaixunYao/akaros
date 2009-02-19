/* See COPYRIGHT for copyright information. */

#include <inc/x86.h>
#include <inc/mmu.h>
#include <inc/error.h>
#include <inc/string.h>
#include <inc/assert.h>

#include <kern/pmap.h>
#include <kern/kclock.h>
#include <kern/env.h>

// These variables are set by i386_detect_memory()
static physaddr_t maxpa;	// Maximum physical address
size_t npage;			// Amount of physical memory (in pages)
static size_t basemem;		// Amount of base memory (in bytes)
static size_t extmem;		// Amount of extended memory (in bytes)

// These variables are set in i386_vm_init()
pde_t* boot_pgdir;		// Virtual address of boot time page directory
physaddr_t boot_cr3;		// Physical address of boot time page directory
static char* boot_freemem;	// Pointer to next byte of free mem

struct Page* pages;		// Virtual address of physical page array
static struct Page_list page_free_list;	// Free list of physical pages

extern struct Env *envs;

// Global descriptor table.
//
// The kernel and user segments are identical (except for the DPL).
// To load the SS register, the CPL must equal the DPL.  Thus,
// we must duplicate the segments for the user and the kernel.
//
struct Segdesc gdt[] =
{
	// 0x0 - unused (always faults -- for trapping NULL far pointers)
	SEG_NULL,

	// 0x8 - kernel code segment
	[GD_KT >> 3] = SEG(STA_X | STA_R, 0x0, 0xffffffff, 0),

	// 0x10 - kernel data segment
	[GD_KD >> 3] = SEG(STA_W, 0x0, 0xffffffff, 0),

	// 0x18 - user code segment
	[GD_UT >> 3] = SEG(STA_X | STA_R, 0x0, 0xffffffff, 3),

	// 0x20 - user data segment
	[GD_UD >> 3] = SEG(STA_W, 0x0, 0xffffffff, 3),

	// 0x28 - tss, initialized in idt_init()
	[GD_TSS >> 3] = SEG_NULL
};

struct Pseudodesc gdt_pd = {
	sizeof(gdt) - 1, (unsigned long) gdt
};

static int
nvram_read(int r)
{
	return mc146818_read(r) | (mc146818_read(r + 1) << 8);
}

void
i386_detect_memory(void)
{
	/* For shit BIOS reasons, this isn't seeing any more than 64MB,
	 * explained a little here: 
	 * http://exec.h1.ru/docs/os-devel-faq/os-faq-memory.html
	 */

	// CMOS tells us how many kilobytes there are
	basemem = ROUNDDOWN(nvram_read(NVRAM_BASELO)*1024, PGSIZE);
	extmem = ROUNDDOWN(nvram_read(NVRAM_EXTLO)*1024, PGSIZE);

	// Calculate the maximum physical address based on whether
	// or not there is any extended memory.  See comment in <inc/memlayout.h>
	if (extmem)
		maxpa = EXTPHYSMEM + extmem;
	else
		maxpa = basemem;

	npage = maxpa / PGSIZE;

	cprintf("Physical memory: %dK available, ", (int)(maxpa/1024));
	cprintf("base = %dK, extended = %dK\n", (int)(basemem/1024), (int)(extmem/1024));
}

// --------------------------------------------------------------
// Set up initial memory mappings and turn on MMU.
// --------------------------------------------------------------

static void check_boot_pgdir(bool pse);

//
// Allocate n bytes of physical memory aligned on an 
// align-byte boundary.  Align must be a power of two.
// Return kernel virtual address.  Returned memory is uninitialized.
//
// If we're out of memory, boot_alloc should panic.
// This function may ONLY be used during initialization,
// before the page_free_list has been set up.
// 
static void*
boot_alloc(uint32_t n, uint32_t align)
{
	extern char end[];
	void *v;

	// Initialize boot_freemem if this is the first time.
	// 'end' is a magic symbol automatically generated by the linker,
	// which points to the end of the kernel's bss segment -
	// i.e., the first virtual address that the linker
	// did _not_ assign to any kernel code or global variables.
	if (boot_freemem == 0)
		boot_freemem = end;

	//	Step 1: round boot_freemem up to be aligned properly
	boot_freemem = ROUNDUP(boot_freemem, align);

	//	Step 2: save current value of boot_freemem as allocated chunk
	v = boot_freemem;
	//  Step 2.5: check if we can alloc
	if (PADDR(boot_freemem + n) > maxpa)
		panic("Out of memory in boot alloc, you fool!\n");
	//	Step 3: increase boot_freemem to record allocation
	boot_freemem += n;	
	//	Step 4: return allocated chunk
	return v;
}

//
// Given pgdir, a pointer to a page directory,
// walk the 2-level page table structure to find
// the page table entry (PTE) for linear address la.
// Return a pointer to this PTE.
//
// If the relevant page table doesn't exist in the page directory:
//	- If create == 0, return 0.
//	- Otherwise allocate a new page table, install it into pgdir,
//	  and return a pointer into it.
//        (Questions: What data should the new page table contain?
//	  And what permissions should the new pgdir entry have?
//	  Note that we use the 486-only "WP" feature of %cr0, which
//	  affects the way supervisor-mode writes are checked.)
//
// This function abstracts away the 2-level nature of
// the page directory by allocating new page tables
// as needed.
// 
// boot_pgdir_walk may ONLY be used during initialization,
// before the page_free_list has been set up.
// It should panic on failure.  (Note that boot_alloc already panics
// on failure.)
//
// Supports returning jumbo (4MB PSE) PTEs.  To create with a jumbo, pass in 2.
// 
// Maps non-PSE PDEs as U/W.  W so the kernel can, U so the user can read via
// UVPT.  UVPT security comes from the UVPT mapping (U/R).  All other kernel pages
// protected at the second layer
static pte_t*
boot_pgdir_walk(pde_t *pgdir, uintptr_t la, int create)
{
	pde_t* the_pde = &pgdir[PDX(la)];
	void* new_table;

	if (*the_pde & PTE_P) {
		if (*the_pde & PTE_PS)
			return (pte_t*)the_pde;
		return &((pde_t*)KADDR(PTE_ADDR(*the_pde)))[PTX(la)];
	}
	if (!create)
		return NULL;
	if (create == 2) {
		if (JPGOFF(la))
			panic("Attempting to find a Jumbo PTE at an unaligned VA!");
		*the_pde = PTE_PS | PTE_P;
		return (pte_t*)the_pde;
	}
	new_table = boot_alloc(PGSIZE, PGSIZE);
	memset(new_table, 0, PGSIZE);
	*the_pde = (pde_t)PADDR(new_table) | PTE_P | PTE_W | PTE_U;
	return &((pde_t*)KADDR(PTE_ADDR(*the_pde)))[PTX(la)];
}

//
// Map [la, la+size) of linear address space to physical [pa, pa+size)
// in the page table rooted at pgdir.  Size is a multiple of PGSIZE.
// Use permission bits perm|PTE_P for the entries.
//
// This function may ONLY be used during initialization,
// before the page_free_list has been set up.
//
// To map with Jumbos, set PTE_PS in perm
static void
boot_map_segment(pde_t *pgdir, uintptr_t la, size_t size, physaddr_t pa, int perm)
{
	uintptr_t i;
	pte_t *pte;
	// la can be page unaligned, but weird things will happen
	// unless pa has the same offset.  pa always truncates any
	// possible offset.  will warn.  size can be weird too. 
	if (PGOFF(la)) {
		warn("la not page aligned in boot_map_segment!");
		size += PGOFF(la);
	}
	// even though our maxpa doesn't go above 64MB yet...
	if (pa + size > maxpa)
		warn("Attempting to map to physical memory beyond maxpa!");
	if (perm & PTE_PS) {
		if (JPGOFF(la) || JPGOFF(pa))
			panic("Tried to map a Jumbo page at an unaligned address!");
		// need to index with i instead of la + size, in case of wrap-around
		for (i = 0; i < size; i += JPGSIZE, la += JPGSIZE, pa += JPGSIZE) {
			pte = boot_pgdir_walk(pgdir, la, 2);
			*pte = PTE_ADDR(pa) | PTE_P | perm;
		}
	} else {
		for (i = 0; i < size; i += PGSIZE, la += PGSIZE, pa += PGSIZE) {
			pte = boot_pgdir_walk(pgdir, la, 1);
			*pte = PTE_ADDR(pa) | PTE_P | perm;
		}
	}
}

// Set up a two-level page table:
//    boot_pgdir is its linear (virtual) address of the root
//    boot_cr3 is the physical adresss of the root
// Then turn on paging.  Then effectively turn off segmentation.
// (i.e., the segment base addrs are set to zero).
// 
// This function only sets up the kernel part of the address space
// (ie. addresses >= UTOP).  The user part of the address space
// will be setup later.
//
// From UTOP to ULIM, the user is allowed to read but not write.
// Above ULIM the user cannot read (or write). 
void
i386_vm_init(void)
{
	pde_t* pgdir;
	uint32_t cr0;
	size_t n;
	bool pse;

	// check for PSE support
	asm volatile ("movl    $1, %%eax;
                   cpuid;
                   andl    $0x00000008, %%edx"
	              : "=d"(pse) 
				  : 
	              : "%eax");
	// turn on PSE
	if (pse) {
		cprintf("PSE capability detected.\n");
		uint32_t cr4;
		cr4 = rcr4();
		cr4 |= CR4_PSE;
		lcr4(cr4);
	}

	/*
	 * PSE status: 
	 * - can walk and set up boot_map_segments with jumbos but can't
	 *   insert yet.  
	 * - anything related to a single struct Page still can't handle 
	 *   jumbos.  will need to think about and adjust Page functions
	 * - do we want to store info like this in the struct Page?  or just check
	 *   by walking the PTE
	 * - when we alloc a page, and we want it to be 4MB, we'll need
	 *   to have contiguous memory, etc
	 * - there's a difference between having 4MB page table entries
	 *   and having 4MB Page tracking structs.  changing the latter will
	 *   break a lot of things
	 * - showmapping and friends work on a 4KB granularity, but map to the
	 *   correct entries
	 */

	//////////////////////////////////////////////////////////////////////
	// create initial page directory.
	pgdir = boot_alloc(PGSIZE, PGSIZE);
	memset(pgdir, 0, PGSIZE);
	boot_pgdir = pgdir;
	boot_cr3 = PADDR(pgdir);

	//////////////////////////////////////////////////////////////////////
	// Recursively insert PD in itself as a page table, to form
	// a virtual page table at virtual address VPT.
	// (For now, you don't have understand the greater purpose of the
	// following two lines.  Unless you are eagle-eyed, in which case you
	// should already know.)

	// Permissions: kernel RW, user NONE
	pgdir[PDX(VPT)] = PADDR(pgdir)|PTE_W|PTE_P;

	// same for UVPT
	// Permissions: kernel R, user R 
	pgdir[PDX(UVPT)] = PADDR(pgdir)|PTE_U|PTE_P;

	//////////////////////////////////////////////////////////////////////
	// Map the kernel stack (symbol name "bootstack").  The complete VA
	// range of the stack, [KSTACKTOP-PTSIZE, KSTACKTOP), breaks into two
	// pieces:
	//     * [KSTACKTOP-KSTKSIZE, KSTACKTOP) -- backed by physical memory
	//     * [KSTACKTOP-PTSIZE, KSTACKTOP-KSTKSIZE) -- not backed => faults
	//     Permissions: kernel RW, user NONE
	// Your code goes here:

	// remember that the space for the kernel stack is allocated in the binary.
	// bootstack and bootstacktop point to symbols in the data section, which 
	// at this point are like 0xc010b000.  KSTACKTOP is the desired loc in VM
	boot_map_segment(pgdir, (uintptr_t)KSTACKTOP - KSTKSIZE, 
	                 KSTKSIZE, PADDR(bootstack), PTE_W);

	//////////////////////////////////////////////////////////////////////
	// Map all of physical memory at KERNBASE. 
	// Ie.  the VA range [KERNBASE, 2^32) should map to
	//      the PA range [0, 2^32 - KERNBASE)
	// We might not have 2^32 - KERNBASE bytes of physical memory, but
	// we just set up the mapping anyway.
	// Permissions: kernel RW, user NONE
	// Your code goes here: 
	
	// this maps all of the possible phys memory
	// note the use of unsigned underflow to get size = 0x40000000
	//boot_map_segment(pgdir, KERNBASE, -KERNBASE, 0, PTE_W);
	// but this only maps what is available, and saves memory.  every 4MB of
	// mapped memory requires a 2nd level page: 2^10 entries, each covering 2^12
	// need to modify tests below to account for this
	if (pse)
		boot_map_segment(pgdir, KERNBASE, maxpa, 0, PTE_W | PTE_PS);
	else
		boot_map_segment(pgdir, KERNBASE, maxpa, 0, PTE_W );

	//////////////////////////////////////////////////////////////////////
	// Make 'pages' point to an array of size 'npage' of 'struct Page'.
	// The kernel uses this structure to keep track of physical pages;
	// 'npage' equals the number of physical pages in memory.  User-level
	// programs get read-only access to the array as well.
	// You must allocate the array yourself.
	// Map this array read-only by the user at linear address UPAGES
	// (ie. perm = PTE_U | PTE_P)
	// Permissions:
	//    - pages -- kernel RW, user NONE
	//    - the read-only version mapped at UPAGES -- kernel R, user R
	// Your code goes here: 
	
	// round up to the nearest page
	size_t page_array_size = ROUNDUP(npage*sizeof(struct Page), PGSIZE);
	pages = (struct Page*)boot_alloc(page_array_size, PGSIZE);
	memset(pages, 0, page_array_size);
	if (page_array_size > PTSIZE) {
		warn("page_array_size bigger than PTSIZE, userland will not see all pages");
		page_array_size = PTSIZE;
	}
	boot_map_segment(pgdir, UPAGES, page_array_size, PADDR(pages), PTE_U);

	//////////////////////////////////////////////////////////////////////
	// Make 'envs' point to an array of size 'NENV' of 'struct Env'.
	// Map this array read-only by the user at linear address UENVS
	// (ie. perm = PTE_U | PTE_P).
	// Permissions:
	//    - envs itself -- kernel RW, user NONE
	//    - the image of envs mapped at UENVS  -- kernel R, user R
	
	// round up to the nearest page
	size_t env_array_size = ROUNDUP(NENV*sizeof(struct Env), PGSIZE);
	envs = (struct Env*)boot_alloc(env_array_size, PGSIZE);
	memset(envs, 0, env_array_size);
	if (env_array_size > PTSIZE) {
		warn("env_array_size bigger than PTSIZE, userland will not see all environments");
		env_array_size = PTSIZE;
	}
	boot_map_segment(pgdir, UENVS, env_array_size, PADDR(envs), PTE_U);

	// Check that the initial page directory has been set up correctly.
	check_boot_pgdir(pse);

	//////////////////////////////////////////////////////////////////////
	// On x86, segmentation maps a VA to a LA (linear addr) and
	// paging maps the LA to a PA.  I.e. VA => LA => PA.  If paging is
	// turned off the LA is used as the PA.  Note: there is no way to
	// turn off segmentation.  The closest thing is to set the base
	// address to 0, so the VA => LA mapping is the identity.

	// Current mapping: VA KERNBASE+x => PA x.
	//     (segmentation base=-KERNBASE and paging is off)

	// From here on down we must maintain this VA KERNBASE + x => PA x
	// mapping, even though we are turning on paging and reconfiguring
	// segmentation.

	// Map VA 0:4MB same as VA KERNBASE, i.e. to PA 0:4MB.
	// (Limits our kernel to <4MB)
	/* They mean linear address 0:4MB, and the kernel < 4MB is only until 
	 * segmentation is turned off.
	 * once we turn on paging, segmentation is still on, so references to
	 * KERNBASE+x will get mapped to linear address x, which we need to make 
	 * sure can map to phys addr x, until we can turn off segmentation and
	 * KERNBASE+x maps to LA KERNBASE+x, which maps to PA x, via paging
	 */
	pgdir[0] = pgdir[PDX(KERNBASE)];

	// Install page table.
	lcr3(boot_cr3);

	// Turn on paging.
	cr0 = rcr0();
	// not sure why they turned on TS and EM here.  or anything other 
	// than PG and WP
	cr0 |= CR0_PE|CR0_PG|CR0_AM|CR0_WP|CR0_NE|CR0_TS|CR0_EM|CR0_MP;
	cr0 &= ~(CR0_TS|CR0_EM);
	lcr0(cr0);

	// Current mapping: KERNBASE+x => x => x.
	// (x < 4MB so uses paging pgdir[0])

	// Reload all segment registers.
	asm volatile("lgdt gdt_pd");
	asm volatile("movw %%ax,%%gs" :: "a" (GD_UD|3));
	asm volatile("movw %%ax,%%fs" :: "a" (GD_UD|3));
	asm volatile("movw %%ax,%%es" :: "a" (GD_KD));
	asm volatile("movw %%ax,%%ds" :: "a" (GD_KD));
	asm volatile("movw %%ax,%%ss" :: "a" (GD_KD));
	asm volatile("ljmp %0,$1f\n 1:\n" :: "i" (GD_KT));  // reload cs
	asm volatile("lldt %%ax" :: "a" (0));

	// Final mapping: KERNBASE+x => KERNBASE+x => x.

	// This mapping was only used after paging was turned on but
	// before the segment registers were reloaded.
	pgdir[0] = 0;

	// Flush the TLB for good measure, to kill the pgdir[0] mapping.
	lcr3(boot_cr3);
}

//
// Checks that the kernel part of virtual address space
// has been setup roughly correctly(by i386_vm_init()).
//
// This function doesn't test every corner case,
// in fact it doesn't test the permission bits at all,
// but it is a pretty good sanity check. 
//
static physaddr_t check_va2pa(pde_t *pgdir, uintptr_t va);
static pte_t get_vaperms(pde_t *pgdir, uintptr_t va);

static void
check_boot_pgdir(bool pse)
{
	uint32_t i, n;
	pde_t *pgdir, pte;

	pgdir = boot_pgdir;

	// check pages array
	n = ROUNDUP(npage*sizeof(struct Page), PGSIZE);
	for (i = 0; i < n; i += PGSIZE)
		assert(check_va2pa(pgdir, UPAGES + i) == PADDR(pages) + i);

	// check envs array (new test for lab 3)
	n = ROUNDUP(NENV*sizeof(struct Env), PGSIZE);
	for (i = 0; i < n; i += PGSIZE)
		assert(check_va2pa(pgdir, UENVS + i) == PADDR(envs) + i);

	// check phys mem
	//for (i = 0; KERNBASE + i != 0; i += PGSIZE)
	// adjusted check to account for only mapping avail mem
	if (pse)
		for (i = 0; i < maxpa; i += JPGSIZE)
			assert(check_va2pa(pgdir, KERNBASE + i) == i);
	else
		for (i = 0; i < maxpa; i += PGSIZE)
			assert(check_va2pa(pgdir, KERNBASE + i) == i);

	// check kernel stack
	for (i = 0; i < KSTKSIZE; i += PGSIZE)
		assert(check_va2pa(pgdir, KSTACKTOP - KSTKSIZE + i) == PADDR(bootstack) + i);

	// check for zero/non-zero in PDEs
	for (i = 0; i < NPDENTRIES; i++) {
		switch (i) {
		case PDX(VPT):
		case PDX(UVPT):
		case PDX(KSTACKTOP-1):
		case PDX(UPAGES):
		case PDX(UENVS):
			assert(pgdir[i]);
			break;
		default:
			//if (i >= PDX(KERNBASE))
			// adjusted check to account for only mapping avail mem
			// and you can't KADDR maxpa (just above legal range)
			if (i >= PDX(KERNBASE) && i <= PDX(KADDR(maxpa-1)))
				assert(pgdir[i]);
			else
				assert(pgdir[i] == 0);
			break;
		}
	}

	// check permissions
	// user read-only.  check for user and write, should be only user
	// eagle-eyed viewers should be able to explain the extra cases
	for (i = UENVS; i < ULIM; i+=PGSIZE) {
		pte = get_vaperms(pgdir, i);
		if ((pte & PTE_P) && (i != UVPT+(VPT>>10))) {
			if (pte & PTE_PS) {
				assert((pte & PTE_U) != PTE_U);
				assert((pte & PTE_W) != PTE_W);
			} else {
				assert((pte & PTE_U) == PTE_U);
				assert((pte & PTE_W) != PTE_W);
			}
		}
	}
	// kernel read-write.
	for (i = ULIM; i <= KERNBASE + maxpa - PGSIZE; i+=PGSIZE) {
		pte = get_vaperms(pgdir, i);
		if ((pte & PTE_P) && (i != VPT+(UVPT>>10))) {
			assert((pte & PTE_U) != PTE_U);
			assert((pte & PTE_W) == PTE_W);
		}
	}
	// special mappings
	pte = get_vaperms(pgdir, UVPT+(VPT>>10));
	assert((pte & PTE_U) != PTE_U);
	assert((pte & PTE_W) != PTE_W);

	// note this means the kernel cannot directly manipulate this virtual address
	// convince yourself this isn't a big deal, eagle-eyes!
	pte = get_vaperms(pgdir, VPT+(UVPT>>10));
	assert((pte & PTE_U) != PTE_U);
	assert((pte & PTE_W) != PTE_W);

	cprintf("check_boot_pgdir() succeeded!\n");
}

// This function returns the physical address of the page containing 'va',
// defined by the page directory 'pgdir'.  The hardware normally performs
// this functionality for us!  We define our own version to help check
// the check_boot_pgdir() function; it shouldn't be used elsewhere.

static physaddr_t
check_va2pa(pde_t *pgdir, uintptr_t va)
{
	pte_t *p;

	pgdir = &pgdir[PDX(va)];
	if (!(*pgdir & PTE_P))
		return ~0;
	if (*pgdir & PTE_PS)
		return PTE_ADDR(*pgdir);
	p = (pte_t*) KADDR(PTE_ADDR(*pgdir));
	if (!(p[PTX(va)] & PTE_P))
		return ~0;
	return PTE_ADDR(p[PTX(va)]);
}

/* 
 * This function returns a PTE with the aggregate permissions equivalent
 * to walking the two levels of paging.  PPN = 0.  Somewhat fragile, in that
 * it returns PTE_PS if either entry has PTE_PS (which should only happen
 * for some of the recusive walks)
 */

static pte_t
get_vaperms(pde_t *pgdir, uintptr_t va)
{
	pde_t* pde = &pgdir[PDX(va)];
	pte_t* pte = pgdir_walk(pgdir, (void*)va, 0);
	if (!pte || !(*pte & PTE_P))
		return 0;
	return PGOFF(*pde & *pte) + PTE_PS & (*pde | *pte);
}
		
// --------------------------------------------------------------
// Tracking of physical pages.
// The 'pages' array has one 'struct Page' entry per physical page.
// Pages are reference counted, and free pages are kept on a linked list.
// --------------------------------------------------------------

//  
// Initialize page structure and memory free list.
// After this point, ONLY use the functions below
// to allocate and deallocate physical memory via the page_free_list,
// and NEVER use boot_alloc() or the related boot-time functions above.
//
void
page_init(void)
{
	// The example code here marks all pages as free.
	// However this is not truly the case.  What memory is free?
	//  1) Mark page 0 as in use.
	//     This way we preserve the real-mode IDT and BIOS structures
	//     in case we ever need them.  (Currently we don't, but...)
	//  2) Mark the rest of base memory as free.
	//  3) Then comes the IO hole [IOPHYSMEM, EXTPHYSMEM).
	//     Mark it as in use so that it can never be allocated.      
	//  4) Then extended memory [EXTPHYSMEM, ...).
	//     Some of it is in use, some is free. Where is the kernel?
	//     Which pages are used for page tables and other data structures?
	//
	// Change the code to reflect this.
	int i;
	physaddr_t physaddr_after_kernel = PADDR(ROUNDUP(boot_freemem, PGSIZE));
	LIST_INIT(&page_free_list);

	pages[0].pp_ref = 1;
	for (i = 1; i < PPN(IOPHYSMEM); i++) {
		pages[i].pp_ref = 0;
		LIST_INSERT_HEAD(&page_free_list, &pages[i], pp_link);
	}
	for (i = PPN(IOPHYSMEM); i < PPN(EXTPHYSMEM); i++) {
		pages[i].pp_ref = 1;
	}
	for (i = PPN(EXTPHYSMEM); i < PPN(physaddr_after_kernel); i++) {
		pages[i].pp_ref = 1;
	}
	for (i = PPN(physaddr_after_kernel); i < npage; i++) {
		pages[i].pp_ref = 0;
		LIST_INSERT_HEAD(&page_free_list, &pages[i], pp_link);
	}
}

//
// Initialize a Page structure.
// The result has null links and 0 refcount.
// Note that the corresponding physical page is NOT initialized!
//
static void
page_initpp(struct Page *pp)
{
	memset(pp, 0, sizeof(*pp));
}

//
// Allocates a physical page.
// Does NOT set the contents of the physical page to zero -
// the caller must do that if necessary.
//
// *pp_store -- is set to point to the Page struct of the newly allocated
// page
//
// RETURNS 
//   0 -- on success
//   -E_NO_MEM -- otherwise 
//
// Hint: use LIST_FIRST, LIST_REMOVE, and page_initpp
// Hint: pp_ref should not be incremented 
int
page_alloc(struct Page **pp_store)
{
	if (LIST_EMPTY(&page_free_list))
		return -E_NO_MEM;
	*pp_store = LIST_FIRST(&page_free_list);
	LIST_REMOVE(*pp_store, pp_link);
	page_initpp(*pp_store);
	return 0;
}

//
// Return a page to the free list.
// (This function should only be called when pp->pp_ref reaches 0.)
//
void
page_free(struct Page *pp)
{
	if (pp->pp_ref)
		panic("Attempting to free page with non-zero reference count!");
	LIST_INSERT_HEAD(&page_free_list, pp, pp_link);
}

//
// Decrement the reference count on a page,
// freeing it if there are no more refs.
//
void
page_decref(struct Page* pp)
{
	if (--pp->pp_ref == 0)
		page_free(pp);
}

// Given 'pgdir', a pointer to a page directory, pgdir_walk returns
// a pointer to the page table entry (PTE) for linear address 'va'.
// This requires walking the two-level page table structure.
//
// If the relevant page table doesn't exist in the page directory, then:
//    - If create == 0, pgdir_walk returns NULL.
//    - Otherwise, pgdir_walk tries to allocate a new page table
//	with page_alloc.  If this fails, pgdir_walk returns NULL.
//    - Otherwise, pgdir_walk returns a pointer into the new page table.
//
// This is boot_pgdir_walk, but using page_alloc() instead of boot_alloc().
// Unlike boot_pgdir_walk, pgdir_walk can fail.
//
// Hint: you can turn a Page * into the physical address of the
// page it refers to with page2pa() from kern/pmap.h.
//
// Supports returning jumbo (4MB PSE) PTEs.  To create with a jumbo, pass in 2.
pte_t*
pgdir_walk(pde_t *pgdir, const void *va, int create)
{
	pde_t* the_pde = &pgdir[PDX(va)];
	struct Page* new_table;

	if (*the_pde & PTE_P) {
		if (*the_pde & PTE_PS)
			return (pte_t*)the_pde;
		return &((pde_t*)KADDR(PTE_ADDR(*the_pde)))[PTX(va)];
	}
	if (!create)
		return NULL;
	if (create == 2) {
		if (JPGOFF(va))
			panic("Attempting to find a Jumbo PTE at an unaligned VA!");
		*the_pde = PTE_PS | PTE_P;
		return (pte_t*)the_pde;
	}
	if (page_alloc(&new_table))
		return NULL;
	new_table->pp_ref = 1;
	memset(page2kva(new_table), 0, PGSIZE);
	*the_pde = (pde_t)page2pa(new_table) | PTE_P | PTE_W | PTE_U;
	return &((pde_t*)KADDR(PTE_ADDR(*the_pde)))[PTX(va)];
}
//
// Map the physical page 'pp' at virtual address 'va'.
// The permissions (the low 12 bits) of the page table
//  entry should be set to 'perm|PTE_P'.
//
// Details
//   - If there is already a page mapped at 'va', it is page_remove()d.
//   - If necessary, on demand, allocates a page table and inserts it into
//     'pgdir'.
//   - pp->pp_ref should be incremented if the insertion succeeds.
//   - The TLB must be invalidated if a page was formerly present at 'va'.
//     (this is handled in page_remove)
//
// RETURNS: 
//   0 on success
//   -E_NO_MEM, if page table couldn't be allocated
//
// Hint: The TA solution is implemented using pgdir_walk, page_remove,
// and page2pa.
//
int
page_insert(pde_t *pgdir, struct Page *pp, void *va, int perm) 
{
	pte_t* pte = pgdir_walk(pgdir, va, 1);
	if (!pte)
		return -E_NO_MEM;
	// need to up the ref count in case pp is already mapped at va
	// and we don't want to page_remove (which could free pp) and then 
	// continue as if pp wasn't freed.  moral = up the ref asap
	pp->pp_ref++;
	if (*pte & PTE_P) {
		page_remove(pgdir, va);
	}
	*pte = page2pa(pp) | PTE_P | perm;
	return 0;
}

//
// Return the page mapped at virtual address 'va'.
// If pte_store is not zero, then we store in it the address
// of the pte for this page.  This is used by page_remove
// but should not be used by other callers.
//
// Return 0 if there is no page mapped at va.
//
// Hint: the TA solution uses pgdir_walk and pa2page.
//
// For jumbos, right now this returns the first Page* in the 4MB
struct Page *
page_lookup(pde_t *pgdir, void *va, pte_t **pte_store)
{
	pte_t* pte = pgdir_walk(pgdir, va, 0);
	if (!pte || !(*pte & PTE_P))
		return 0;
	if (pte_store)
		*pte_store = pte;
	return pa2page(PTE_ADDR(*pte));
}

//
// Unmaps the physical page at virtual address 'va'.
// If there is no physical page at that address, silently does nothing.
//
// Details:
//   - The ref count on the physical page should decrement.
//   - The physical page should be freed if the refcount reaches 0.
//   - The pg table entry corresponding to 'va' should be set to 0.
//     (if such a PTE exists)
//   - The TLB must be invalidated if you remove an entry from
//     the pg dir/pg table.
//
// Hint: The TA solution is implemented using page_lookup,
// 	tlb_invalidate, and page_decref.
//
// This may be wonky wrt Jumbo pages and decref.  
void
page_remove(pde_t *pgdir, void *va)
{
	pte_t* pte;
	struct Page* page;
	page = page_lookup(pgdir, va, &pte);
	if (!page)
		return;
	*pte = 0;
	tlb_invalidate(pgdir, va);
	page_decref(page);
}

//
// Invalidate a TLB entry, but only if the page tables being
// edited are the ones currently in use by the processor.
//
void
tlb_invalidate(pde_t *pgdir, void *va)
{
	// Flush the entry only if we're modifying the current address space.
	// For now, there is only one address space, so always invalidate.
	invlpg(va);
}

static uintptr_t user_mem_check_addr;

//
// Check that an environment is allowed to access the range of memory
// [va, va+len) with permissions 'perm | PTE_P'.
// Normally 'perm' will contain PTE_U at least, but this is not required.
// 'va' and 'len' need not be page-aligned; you must test every page that
// contains any of that range.  You will test either 'len/PGSIZE',
// 'len/PGSIZE + 1', or 'len/PGSIZE + 2' pages.
//
// A user program can access a virtual address if (1) the address is below
// ULIM, and (2) the page table gives it permission.  These are exactly
// the tests you should implement here.
//
// If there is an error, set the 'user_mem_check_addr' variable to the first
// erroneous virtual address.
//
// Returns 0 if the user program can access this range of addresses,
// and -E_FAULT otherwise.
//
// Hint: The TA solution uses pgdir_walk.
//
int
user_mem_check(struct Env *env, const void *va, size_t len, int perm)
{
	// LAB 3: Your code here. 

	return 0;
}

//
// Checks that environment 'env' is allowed to access the range
// of memory [va, va+len) with permissions 'perm | PTE_U'.
// If it can, then the function simply returns.
// If it cannot, 'env' is destroyed.
//
void
user_mem_assert(struct Env *env, const void *va, size_t len, int perm)
{
	if (user_mem_check(env, va, len, perm | PTE_U) < 0) {
		cprintf("[%08x] user_mem_check assertion failure for "
			"va %08x\n", curenv->env_id, user_mem_check_addr);
		env_destroy(env);	// may not return
	}
}

void
page_check(void)
{
	struct Page *pp, *pp0, *pp1, *pp2;
	struct Page_list fl;
	pte_t *ptep;

	// should be able to allocate three pages
	pp0 = pp1 = pp2 = 0;
	assert(page_alloc(&pp0) == 0);
	assert(page_alloc(&pp1) == 0);
	assert(page_alloc(&pp2) == 0);

	assert(pp0);
	assert(pp1 && pp1 != pp0);
	assert(pp2 && pp2 != pp1 && pp2 != pp0);

	// temporarily steal the rest of the free pages
	fl = page_free_list;
	LIST_INIT(&page_free_list);

	// should be no free memory
	assert(page_alloc(&pp) == -E_NO_MEM);

	// Fill pp1 with bogus data and check for invalid tlb entries
	memset(page2kva(pp1), 0xFFFFFFFF, PGSIZE);

	// there is no page allocated at address 0
	assert(page_lookup(boot_pgdir, (void *) 0x0, &ptep) == NULL);

	// there is no free memory, so we can't allocate a page table 
	assert(page_insert(boot_pgdir, pp1, 0x0, 0) < 0);

	// free pp0 and try again: pp0 should be used for page table
	page_free(pp0);
	assert(page_insert(boot_pgdir, pp1, 0x0, 0) == 0);
	tlb_invalidate(boot_pgdir, 0x0);
	// DEP Should have shot down invalid TLB entry - let's check
	{
	  int *x = 0x0;
	  assert(*x == 0xFFFFFFFF);
	}
	assert(PTE_ADDR(boot_pgdir[0]) == page2pa(pp0));
	assert(check_va2pa(boot_pgdir, 0x0) == page2pa(pp1));
	assert(pp1->pp_ref == 1);
	assert(pp0->pp_ref == 1);

	// should be able to map pp2 at PGSIZE because pp0 is already allocated for page table
	assert(page_insert(boot_pgdir, pp2, (void*) PGSIZE, 0) == 0);
	assert(check_va2pa(boot_pgdir, PGSIZE) == page2pa(pp2));
	assert(pp2->pp_ref == 1);

	// Make sure that pgdir_walk returns a pointer to the pte and
	// not the table or some other garbage
	{
	  pte_t *p = KADDR(PTE_ADDR(boot_pgdir[PDX(PGSIZE)]));
	  assert(pgdir_walk(boot_pgdir, (void *)PGSIZE, 0) == &p[PTX(PGSIZE)]);
	}

	// should be no free memory
	assert(page_alloc(&pp) == -E_NO_MEM);

	// should be able to map pp2 at PGSIZE because it's already there
	assert(page_insert(boot_pgdir, pp2, (void*) PGSIZE, PTE_U) == 0);
	assert(check_va2pa(boot_pgdir, PGSIZE) == page2pa(pp2));
	assert(pp2->pp_ref == 1);

	// Make sure that we actually changed the permission on pp2 when we re-mapped it
	{
	  pte_t *p = pgdir_walk(boot_pgdir, (void*)PGSIZE, 0);
	  assert(((*p) & PTE_U) == PTE_U);
	}

	// pp2 should NOT be on the free list
	// could happen in ref counts are handled sloppily in page_insert
	assert(page_alloc(&pp) == -E_NO_MEM);

	// should not be able to map at PTSIZE because need free page for page table
	assert(page_insert(boot_pgdir, pp0, (void*) PTSIZE, 0) < 0);

	// insert pp1 at PGSIZE (replacing pp2)
	assert(page_insert(boot_pgdir, pp1, (void*) PGSIZE, 0) == 0);

	// should have pp1 at both 0 and PGSIZE, pp2 nowhere, ...
	assert(check_va2pa(boot_pgdir, 0) == page2pa(pp1));
	assert(check_va2pa(boot_pgdir, PGSIZE) == page2pa(pp1));
	// ... and ref counts should reflect this
	assert(pp1->pp_ref == 2);
	assert(pp2->pp_ref == 0);

	// pp2 should be returned by page_alloc
	assert(page_alloc(&pp) == 0 && pp == pp2);

	// unmapping pp1 at 0 should keep pp1 at PGSIZE
	page_remove(boot_pgdir, 0x0);
	assert(check_va2pa(boot_pgdir, 0x0) == ~0);
	assert(check_va2pa(boot_pgdir, PGSIZE) == page2pa(pp1));
	assert(pp1->pp_ref == 1);
	assert(pp2->pp_ref == 0);

	// unmapping pp1 at PGSIZE should free it
	page_remove(boot_pgdir, (void*) PGSIZE);
	assert(check_va2pa(boot_pgdir, 0x0) == ~0);
	assert(check_va2pa(boot_pgdir, PGSIZE) == ~0);
	assert(pp1->pp_ref == 0);
	assert(pp2->pp_ref == 0);

	// so it should be returned by page_alloc
	assert(page_alloc(&pp) == 0 && pp == pp1);

	// should be no free memory
	assert(page_alloc(&pp) == -E_NO_MEM);

	// forcibly take pp0 back
	assert(PTE_ADDR(boot_pgdir[0]) == page2pa(pp0));
	boot_pgdir[0] = 0;
	assert(pp0->pp_ref == 1);
	pp0->pp_ref = 0;

	// Catch invalid pointer addition in pgdir_walk - i.e. pgdir + PDX(va)
	{
	  // Give back pp0 for a bit
	  page_free(pp0);

	  void * va = (void *)((PGSIZE * NPDENTRIES) + PGSIZE);
	  pte_t *p2 = pgdir_walk(boot_pgdir, va, 1);
	  pte_t *p = KADDR(PTE_ADDR(boot_pgdir[PDX(va)]));
	  assert(p2 == &p[PTX(va)]);

	  // Clean up again
	  boot_pgdir[PDX(va)] = 0;
	  pp0->pp_ref = 0;
	}

	// give free list back
	page_free_list = fl;

	// free the pages we took
	page_free(pp0);
	page_free(pp1);
	page_free(pp2);

	cprintf("page_check() succeeded!\n");
}


/* 
	// helpful if you want to manually walk with kvm / bochs
	//cprintf("pgdir va = %08p, pgdir pa = %08p\n\n", pgdir, PADDR(pgdir));

    // testing code for boot_pgdir_walk 
	pte_t* temp;
	temp = boot_pgdir_walk(pgdir, VPT + (VPT >> 10), 1);
	cprintf("pgdir = %p\n", pgdir);
	cprintf("test recursive walking pte_t* = %p\n", temp);
	cprintf("test recursive walking entry = %p\n", PTE_ADDR(temp));
	temp = boot_pgdir_walk(pgdir, 0xc0400000, 1);
	cprintf("LA = 0xc0400000 = %p\n", temp);
	temp = boot_pgdir_walk(pgdir, 0xc0400070, 1);
	cprintf("LA = 0xc0400070 = %p\n", temp);
	temp = boot_pgdir_walk(pgdir, 0xc0800000, 0);
	cprintf("LA = 0xc0800000, no create = %p\n", temp);
	temp = boot_pgdir_walk(pgdir, 0xc0600070, 1);
	cprintf("LA = 0xc0600070 = %p\n", temp);
	temp = boot_pgdir_walk(pgdir, 0xc0600090, 0);
	cprintf("LA = 0xc0600090, nc = %p\n", temp);
	temp = boot_pgdir_walk(pgdir, 0xc0608070, 0);
	cprintf("LA = 0xc0608070, nc = %p\n", temp);
	temp = boot_pgdir_walk(pgdir, 0xc0800070, 1);
	cprintf("LA = 0xc0800070 = %p\n", temp);
	temp = boot_pgdir_walk(pgdir, 0xc0b00070, 0);
	cprintf("LA = 0xc0b00070, nc = %p\n", temp);
	temp = boot_pgdir_walk(pgdir, 0xc0c00000, 0);
	cprintf("LA = 0xc0c00000, nc = %p\n", temp);

	// testing for boot_map_seg
	cprintf("\n");
	cprintf("before mapping 1 page to 0x00350000\n");
	cprintf("0xc4000000's &pte: %08x\n",boot_pgdir_walk(pgdir, 0xc4000000, 1));
	cprintf("0xc4000000's pte: %08x\n",*(boot_pgdir_walk(pgdir, 0xc4000000, 1)));
	boot_map_segment(pgdir, 0xc4000000, 4096, 0x00350000, PTE_W);
	cprintf("after mapping\n");
	cprintf("0xc4000000's &pte: %08x\n",boot_pgdir_walk(pgdir, 0xc4000000, 1));
	cprintf("0xc4000000's pte: %08x\n",*(boot_pgdir_walk(pgdir, 0xc4000000, 1)));

	cprintf("\n");
	cprintf("before mapping 3 pages to 0x00700000\n");
	cprintf("0xd0000000's &pte: %08x\n",boot_pgdir_walk(pgdir, 0xd0000000, 1));
	cprintf("0xd0000000's pte: %08x\n",*(boot_pgdir_walk(pgdir, 0xd0000000, 1)));
	cprintf("0xd0001000's &pte: %08x\n",boot_pgdir_walk(pgdir, 0xd0001000, 1));
	cprintf("0xd0001000's pte: %08x\n",*(boot_pgdir_walk(pgdir, 0xd0001000, 1)));
	cprintf("0xd0002000's &pte: %08x\n",boot_pgdir_walk(pgdir, 0xd0002000, 1));
	cprintf("0xd0002000's pte: %08x\n",*(boot_pgdir_walk(pgdir, 0xd0002000, 1)));
	boot_map_segment(pgdir, 0xd0000000, 4096*3, 0x00700000, 0);
	cprintf("after mapping\n");
	cprintf("0xd0000000's &pte: %08x\n",boot_pgdir_walk(pgdir, 0xd0000000, 1));
	cprintf("0xd0000000's pte: %08x\n",*(boot_pgdir_walk(pgdir, 0xd0000000, 1)));
	cprintf("0xd0001000's &pte: %08x\n",boot_pgdir_walk(pgdir, 0xd0001000, 1));
	cprintf("0xd0001000's pte: %08x\n",*(boot_pgdir_walk(pgdir, 0xd0001000, 1)));
	cprintf("0xd0002000's &pte: %08x\n",boot_pgdir_walk(pgdir, 0xd0002000, 1));
	cprintf("0xd0002000's pte: %08x\n",*(boot_pgdir_walk(pgdir, 0xd0002000, 1)));

	cprintf("\n");
	cprintf("before mapping 1 unaligned to 0x00500010\n");
	cprintf("0xc8000010's &pte: %08x\n",boot_pgdir_walk(pgdir, 0xc8000010, 1));
	cprintf("0xc8000010's pte: %08x\n",*(boot_pgdir_walk(pgdir, 0xc8000010, 1)));
	cprintf("0xc8001010's &pte: %08x\n",boot_pgdir_walk(pgdir, 0xc8001010, 1));
	cprintf("0xc8001010's pte: %08x\n",*(boot_pgdir_walk(pgdir, 0xc8001010, 1)));
	boot_map_segment(pgdir, 0xc8000010, 4096, 0x00500010, PTE_W);
	cprintf("after mapping\n");
	cprintf("0xc8000010's &pte: %08x\n",boot_pgdir_walk(pgdir, 0xc8000010, 1));
	cprintf("0xc8000010's pte: %08x\n",*(boot_pgdir_walk(pgdir, 0xc8000010, 1)));
	cprintf("0xc8001010's &pte: %08x\n",boot_pgdir_walk(pgdir, 0xc8001010, 1));
	cprintf("0xc8001010's pte: %08x\n",*(boot_pgdir_walk(pgdir, 0xc8001010, 1)));

	cprintf("\n");
	boot_map_segment(pgdir, 0xe0000000, 4096, 0x10000000, PTE_W);

*/
