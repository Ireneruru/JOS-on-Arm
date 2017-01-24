/* See COPYRIGHT for copyright information. */

#include <inc/types.h>
#include <inc/arm.h>
#include <inc/mmu.h>
#include <inc/error.h>
#include <inc/string.h>
#include <inc/assert.h>

#include <kern/pmap.h>

pde_t kern_pgdir[4096] __attribute__((aligned(16 * 1024)));

#define TOTAL_PHYS_MEM (256 * 1024 * 1024) // 256MB
#define NPAGES (TOTAL_PHYS_MEM / PGSIZE)

struct PageInfo pages[NPAGES];
static struct PageInfo *page_free_list;
size_t npages = NPAGES;

static void check_page_free_list();
static void check_page_alloc(void);
static void check_kern_pgdir(void);
static physaddr_t check_va2pa(pde_t *pgdir, uintptr_t va);
static void check_page(void);
static void check_page_installed_pgdir(void);

static void set_domain(int did, int priv) {
    int clear_bit = ~(11 << (2 * did));
    int new_priv = priv << (2 * did);
    asm("mrc p15, 0, r0, c3, c0, 0\n"
	    "and r0, r0, %0\n"
	    "orr r0, r0, %1\n"
	    "mcr p15, 0, r0, c3, c0, 0\n" 
	    : 
	    : "r"(clear_bit), "r"(new_priv)
	    : "r0");
}

void mem_init()
{
    page_init();


    // map physical memory
    for (uintptr_t addr = KERNBASE; addr != 0; addr += PTSIZE) {
	kern_pgdir[PDX(addr)] = PADDR((void*)addr) | PDE_ENTRY_1M | PDE_NONE_U;
	kern_pgdir[PDX(PADDR((void*)addr))] = 0;
    }

    // map kernel stack
    kern_pgdir[PDX(KSTACKTOP - KSTKSIZE)] = PADDR(bootstack) | PDE_ENTRY_1M | PDE_NONE_U;

    // map gpio memory-map
    kern_pgdir[PDX(GPIOBASE)] = 0x3F200000 | PDE_ENTRY_1M | PDE_NONE_U;


    load_pgdir(PADDR(kern_pgdir));
    set_domain(0, DOMAIN_CLIENT);

    check_page_free_list();
    check_page_alloc();
    check_page();
    check_kern_pgdir();
    check_page_installed_pgdir();
}

void page_init(void)
{
    extern char end[];
    for (physaddr_t addr = 0; addr < TOTAL_PHYS_MEM; addr += PGSIZE) {
	struct PageInfo *pg = pa2page(addr);
	if (addr == 0 || (0x100000 <= addr && addr < PADDR(end)))
	    continue;
	pg->pp_ref = 0;
	pg->pp_link = page_free_list;
	page_free_list = pg;
    }
}

struct PageInfo * page_alloc(int alloc_flags)
{
    if (page_free_list == NULL) return NULL;
    struct PageInfo* ret = page_free_list;
    page_free_list = ret->pp_link;
    if (alloc_flags & ALLOC_ZERO) 
	memset(page2kva(ret), 0, PGSIZE);
    ret->pp_link = NULL;
    return ret;
}

void page_free(struct PageInfo *pp)
{
    if (pp->pp_ref == 0) {
	pp->pp_link = page_free_list;
	page_free_list = pp;
    }
    else {
	panic("pp->pp_ref is not zero. Wrong call of the page_free!!!");
    }
}

void page_decref(struct PageInfo* pp)
{
    if (--pp->pp_ref == 0)
	page_free(pp);
}

static pte_t* pgtbl_alloc()
{
    static pte_t* tbl = NULL;
    if ((uintptr_t)tbl % PGSIZE == 0) {
	struct PageInfo *pg = page_alloc(ALLOC_ZERO);
	if (pg == NULL) return NULL;
	tbl = page2kva(pg);
	pg->pp_ref++;
    }
    pte_t *ret = tbl;
    tbl += NPTENTRIES * 4;
    return ret;
}

pte_t * pgdir_walk(pde_t *pgdir, const void *va, int create)
{
    if (!(pgdir[PDX(va)] & PTE_P)) {
	if (!create) return NULL;
	pte_t* pgtbl = pgtbl_alloc();
	if (!pgtbl) return NULL;
	pgdir[PDX(va)] = PADDR(pgtbl) | PDE_ENTRY;
    }
    pte_t *pgtbl = (pte_t*)KADDR(PDE_ADDR(pgdir[PDX(va)]));
    return &pgtbl[PTX(va)];
}

static void boot_map_region(pde_t *pgdir, uintptr_t va, size_t size, physaddr_t pa)
{
    assert(va % PGSIZE == 0);
    assert(pa % PGSIZE == 0);
    for (int i = 0; i < size; i += PGSIZE) {
	pte_t *pte = pgdir_walk(pgdir, (void*)(va + i), 1);
	if (pte) {
	    *pte = (pa + i) | PTE_ENTRY_SMALL | PTE_NONE_U;
	}
	else {
	    panic("boot_map_region out of memory\n");
	}
    }
}

int page_insert(pde_t *pgdir, struct PageInfo *pp, void *va, int perm)
{
    pte_t *pte = pgdir_walk(pgdir, va, 1);
    if (pte == NULL) return -E_NO_MEM;
    if (*pte & PTE_P) {
	if (PTE_SMALL_ADDR(*pte) == page2pa(pp)) {
	    pp->pp_ref--;
	    tlb_invalidate(pgdir, va);
	}
	else {
	    page_remove(pgdir, va);
	}
    }
    *pte = page2pa(pp) | perm | PTE_P;
    pp->pp_ref++;
    return 0;
}

struct PageInfo * page_lookup(pde_t *pgdir, void *va, pte_t **pte_store)
{
    pte_t *pte = pgdir_walk(pgdir, va, 0);
    if (pte_store != NULL) *pte_store = pte;
    if (pte == NULL || !(*pte & PTE_P)) return NULL;
    return pa2page(PTE_SMALL_ADDR(*pte));
}

void page_remove(pde_t *pgdir, void *va)
{
    struct PageInfo *page = page_lookup(pgdir, va, 0);
    pte_t *pte = pgdir_walk(pgdir, va, 0);
    if (page != NULL) page_decref(page);
    if (pte != NULL) {
	*pte = 0;
	tlb_invalidate(pgdir, va);
    }
}

void tlb_invalidate(pde_t *pgdir, void *va)
{
    asm("mcr p15, 0, %0, c8, c7, 1"
	    :
	    : "r"(va)
	    :);
}

// --------------------------------------------------------------
// Checking functions.
// --------------------------------------------------------------

//
// Check that the pages on the page_free_list are reasonable.
//
    static void
check_page_free_list()
{
    int count = 0;

    for (struct PageInfo* pg = page_free_list; pg != NULL; pg = pg->pp_link) {
	assert(pg->pp_ref == 0);
	count++;
    }
    assert(count > 0);
    cprintf("check_page_free_list() succeeded!\n");
}

//
// Check the physical page allocator (page_alloc(), page_free(),
// and page_init()).
//
    static void
check_page_alloc(void)
{
    struct PageInfo *pp, *pp0, *pp1, *pp2;
    int nfree;
    struct PageInfo *fl;
    char *c;
    int i;

    // check number of free pages
    for (pp = page_free_list, nfree = 0; pp; pp = pp->pp_link)
	++nfree;

    // should be able to allocate three pages
    pp0 = pp1 = pp2 = 0;
    assert((pp0 = page_alloc(0)));
    assert((pp1 = page_alloc(0)));
    assert((pp2 = page_alloc(0)));

    assert(pp0);
    assert(pp1 && pp1 != pp0);
    assert(pp2 && pp2 != pp1 && pp2 != pp0);
    assert(page2pa(pp0) < npages*PGSIZE);
    assert(page2pa(pp1) < npages*PGSIZE);
    assert(page2pa(pp2) < npages*PGSIZE);

    // temporarily steal the rest of the free pages
    fl = page_free_list;
    page_free_list = 0;

    // should be no free memory
    assert(!page_alloc(0));

    // free and re-allocate?
    page_free(pp0);
    page_free(pp1);
    page_free(pp2);
    pp0 = pp1 = pp2 = 0;
    assert((pp0 = page_alloc(0)));
    assert((pp1 = page_alloc(0)));
    assert((pp2 = page_alloc(0)));
    assert(pp0);
    assert(pp1 && pp1 != pp0);
    assert(pp2 && pp2 != pp1 && pp2 != pp0);
    assert(!page_alloc(0));

    // test flags
    memset(page2kva(pp0), 1, PGSIZE);
    page_free(pp0);
    assert((pp = page_alloc(ALLOC_ZERO)));
    assert(pp && pp0 == pp);
    c = page2kva(pp);
    for (i = 0; i < PGSIZE; i++)
	assert(c[i] == 0);

    // give free list back
    page_free_list = fl;

    // free the pages we took
    page_free(pp0);
    page_free(pp1);
    page_free(pp2);

    // number of free pages should be the same
    for (pp = page_free_list; pp; pp = pp->pp_link)
	--nfree;
    assert(nfree == 0);

    cprintf("check_page_alloc() succeeded!\n");
}

//
// Checks that the kernel part of virtual address space
// has been setup roughly correctly (by mem_init()).
//
// This function doesn't test every corner case,
// but it is a pretty good sanity check.
//

static void check_kern_pgdir(void)
{
    uint32_t i, n;
    pde_t *pgdir;

    pgdir = kern_pgdir;

    /*
    // check pages array
    n = ROUNDUP(npages*sizeof(struct PageInfo), PGSIZE);
    for (i = 0; i < n; i += PGSIZE)
    assert(check_va2pa(pgdir, UPAGES + i) == PADDR(pages) + i);
     */

    // check phys mem
    for (i = 0; i < npages * PGSIZE; i += PGSIZE)
	assert(check_va2pa(pgdir, KERNBASE + i) == i);

    /*
    // check kernel stack
    for (i = 0; i < KSTKSIZE; i += PGSIZE)
    assert(check_va2pa(pgdir, KSTACKTOP - KSTKSIZE + i) == PADDR(bootstack) + i);
    assert(check_va2pa(pgdir, KSTACKTOP - PTSIZE) == ~0);
     */

    // check PDE permissions
    for (i = 0; i < NPDENTRIES; i++) {
	switch (i) {
	    //	    case PDX(UVPT):
	    case PDX(KSTACKTOP-1):
		//	    case PDX(UPAGES):
	    case PDX(GPIOBASE):
		assert(pgdir[i] & PTE_P);
		break;
	    default:
		if (i >= PDX(KERNBASE)) {
		    assert(pgdir[i] & PDE_P);
		    assert(pgdir[i] & PDE_NONE_U);
		} else
		    assert(pgdir[i] == 0);
		break;
	}
    }
    cprintf("check_kern_pgdir() succeeded!\n");
}

// This function returns the physical address of the page containing 'va',
// defined by the page directory 'pgdir'.  The hardware normally performs
// this functionality for us!  We define our own version to help check
// the check_kern_pgdir() function; it shouldn't be used elsewhere.
static physaddr_t check_va2pa(pde_t *pgdir, uintptr_t va)
{
    pte_t *p;

    pgdir = &pgdir[PDX(va)];
    if (!(*pgdir & PDE_P))
	return ~0;

    if ((*pgdir & PDE_ENTRY_1M) == PDE_ENTRY_1M) {
	return (physaddr_t) (((*pgdir) & 0xFFF00000) + (va & 0xFFFFF));
    } 
    else if ((*pgdir & PDE_ENTRY_16M) == PDE_ENTRY_16M){
	return (physaddr_t) (((*pgdir) & 0xFF000000) + (va & 0xFFFFFF));
    }
    else {
	p = (pte_t*) KADDR(PDE_ADDR(*pgdir));
	if (!(p[PTX(va)] & PTE_P))
	    return ~0;
	pte_t pte = p[PTX(va)];
	if ((pte & PTE_ENTRY_SMALL) == PTE_ENTRY_SMALL) {
	    return PTE_SMALL_ADDR(p[PTX(va)]) + (va & 0xFFF);
	} else {
	    return PTE_LARGE_ADDR(p[PTX(va)]) + (va & 0xFFFF);
	}
    }
    panic("unreachable area.\n");
    return ~0;
}

// check page_insert, page_remove, &c
    static void
check_page(void)
{
       struct PageInfo *pp, *pp0, *pp1, *pp2;
       struct PageInfo *fl;
       pte_t *ptep, *ptep1;
       void *va;
       int i;
       extern pde_t entry_pgdir[];

    // should be able to allocate three pages
    pp0 = pp1 = pp2 = 0;
    assert((pp0 = page_alloc(0)));
    assert((pp1 = page_alloc(0)));
    assert((pp2 = page_alloc(0)));

    assert(pp0);
    assert(pp1 && pp1 != pp0);
    assert(pp2 && pp2 != pp1 && pp2 != pp0);

    // temporarily steal the rest of the free pages
    fl = page_free_list;
    page_free_list = 0;

    // should be no free memory
    assert(!page_alloc(0));

    // there is no page allocated at address 0
    assert(page_lookup(kern_pgdir, (void *) 0x0, &ptep) == NULL);

    // there is no free memory, so we can't allocate a page table
    assert(page_insert(kern_pgdir, pp1, 0x0, PTE_NONE_U) < 0);

    // free pp0 and try again: pp0 should be used for page table
    page_free(pp0);
    assert(page_insert(kern_pgdir, pp1, 0x0, PTE_NONE_U) == 0);
    assert(PTE_SMALL_ADDR(kern_pgdir[0]) == page2pa(pp0));
    assert(check_va2pa(kern_pgdir, 0x0) == page2pa(pp1));
    assert(pp1->pp_ref == 1);
    assert(pp0->pp_ref == 1);

    // should be able to map pp2 at PGSIZE because pp0 is already allocated for page table
    assert(page_insert(kern_pgdir, pp2, (void*) PGSIZE, PTE_NONE_U) == 0);
    assert(check_va2pa(kern_pgdir, PGSIZE) == page2pa(pp2));
    assert(pp2->pp_ref == 1);

    // should be no free memory
    assert(!page_alloc(0));

    // should be able to map pp2 at PGSIZE because it's already there
    assert(page_insert(kern_pgdir, pp2, (void*) PGSIZE, PTE_NONE_U) == 0);
    assert(check_va2pa(kern_pgdir, PGSIZE) == page2pa(pp2));
    assert(pp2->pp_ref == 1);

    // pp2 should NOT be on the free list
    // could happen in ref counts are handled sloppily in page_insert
    assert(!page_alloc(0));

    // check that pgdir_walk returns a pointer to the pte
    ptep = (pte_t *) KADDR(PTE_SMALL_ADDR(kern_pgdir[PDX(PGSIZE)]));
    assert(pgdir_walk(kern_pgdir, (void*)PGSIZE, 0) == ptep+PTX(PGSIZE));

    // should be able to change permissions too.
    assert(page_insert(kern_pgdir, pp2, (void*) PGSIZE, PTE_RW_U) == 0);
    assert(check_va2pa(kern_pgdir, PGSIZE) == page2pa(pp2));
    assert(pp2->pp_ref == 1);
    assert(*pgdir_walk(kern_pgdir, (void*) PGSIZE, 0) & PTE_RW_U);
    //assert(kern_pgdir[0] & PTE_U);

    // should be able to remap with fewer permissions
    assert(page_insert(kern_pgdir, pp2, (void*) PGSIZE, PTE_NONE_U) == 0);
    assert(*pgdir_walk(kern_pgdir, (void*) PGSIZE, 0) & PTE_NONE_U);
    assert((*pgdir_walk(kern_pgdir, (void*) PGSIZE, 0) & PTE_RW_U) != PTE_RW_U);

    // should not be able to map at PTSIZE because need free page for page table
    assert(page_insert(kern_pgdir, pp0, (void*) PTSIZE, PTE_NONE_U) < 0);

    // insert pp1 at PGSIZE (replacing pp2)
    assert(page_insert(kern_pgdir, pp1, (void*) PGSIZE, PTE_NONE_U) == 0);
    assert((*pgdir_walk(kern_pgdir, (void*) PGSIZE, 0) & PTE_RW_U) != PTE_RW_U);

    // should have pp1 at both 0 and PGSIZE, pp2 nowhere, ...
    assert(check_va2pa(kern_pgdir, 0) == page2pa(pp1));
    assert(check_va2pa(kern_pgdir, PGSIZE) == page2pa(pp1));
    // ... and ref counts should reflect this
    assert(pp1->pp_ref == 2);
    assert(pp2->pp_ref == 0);

    // pp2 should be returned by page_alloc
    assert((pp = page_alloc(0)) && pp == pp2);

    // unmapping pp1 at 0 should keep pp1 at PGSIZE
    page_remove(kern_pgdir, 0x0);
    assert(check_va2pa(kern_pgdir, 0x0) == ~0);
    assert(check_va2pa(kern_pgdir, PGSIZE) == page2pa(pp1));
    assert(pp1->pp_ref == 1);
    assert(pp2->pp_ref == 0);

    // test re-inserting pp1 at PGSIZE
    assert(page_insert(kern_pgdir, pp1, (void*) PGSIZE, 0) == 0);
    assert(pp1->pp_ref);
    assert(pp1->pp_link == NULL);

    // unmapping pp1 at PGSIZE should free it
    page_remove(kern_pgdir, (void*) PGSIZE);
    assert(check_va2pa(kern_pgdir, 0x0) == ~0);
    assert(check_va2pa(kern_pgdir, PGSIZE) == ~0);
    assert(pp1->pp_ref == 0);
    assert(pp2->pp_ref == 0);

    // so it should be returned by page_alloc
    assert((pp = page_alloc(0)) && pp == pp1);

    // should be no free memory
    assert(!page_alloc(0));

    // forcibly take pp0 back
    assert(PTE_SMALL_ADDR(kern_pgdir[0]) == page2pa(pp0));
    kern_pgdir[0] = 0;
    assert(pp0->pp_ref == 1);
    pp0->pp_ref = 0;

    // check pointer arithmetic in pgdir_walk
    page_free(pp0);
    va = (void*)(PGSIZE * NPDENTRIES + PGSIZE);
    ptep = pgdir_walk(kern_pgdir, va, 1);
    ptep1 = (pte_t *) KADDR(PTE_SMALL_ADDR(kern_pgdir[PDX(va)]));
    assert(ptep == ptep1 + PTX(va));
    kern_pgdir[PDX(va)] = 0;
    pp0->pp_ref = 0;

    // check that new page tables get cleared
    memset(page2kva(pp0), 0xFF, PGSIZE);
    page_free(pp0);
    pgdir_walk(kern_pgdir, 0x0, 1);
    ptep = (pte_t *) page2kva(pp0);
    for(i=0; i<NPTENTRIES; i++)
	assert((ptep[i] & PTE_P) == 0);
    kern_pgdir[0] = 0;
    pp0->pp_ref = 0;

    // give free list back
    page_free_list = fl;

    // free the pages we took
    page_free(pp0);
    page_free(pp1);
    page_free(pp2);

    cprintf("check_page() succeeded!\n");
}

// check page_insert, page_remove, &c, with an installed kern_pgdir
    static void
check_page_installed_pgdir(void)
{
    struct PageInfo *pp, *pp0, *pp1, *pp2;
    struct PageInfo *fl;
    pte_t *ptep, *ptep1;
    uintptr_t va;
    int i;

    // check that we can read and write installed pages
    pp1 = pp2 = 0;
    assert((pp0 = page_alloc(0)));
    assert((pp1 = page_alloc(0)));
    assert((pp2 = page_alloc(0)));
    page_free(pp0);
    memset(page2kva(pp1), 1, PGSIZE);
    memset(page2kva(pp2), 2, PGSIZE);
    page_insert(kern_pgdir, pp1, (void*) PGSIZE, PTE_NONE_U);
    assert(pp1->pp_ref == 1);
    assert(*(uint32_t *)PGSIZE == 0x01010101U);
    page_insert(kern_pgdir, pp2, (void*) PGSIZE, PTE_NONE_U);
    assert(*(uint32_t *)PGSIZE == 0x02020202U);
    assert(pp2->pp_ref == 1);
    assert(pp1->pp_ref == 0);
    *(uint32_t *)PGSIZE = 0x03030303U;
    assert(*(uint32_t *)page2kva(pp2) == 0x03030303U);
    page_remove(kern_pgdir, (void*) PGSIZE);
    assert(pp2->pp_ref == 0);

    // forcibly take pp0 back
    assert(PTE_SMALL_ADDR(kern_pgdir[0]) == page2pa(pp0));
    kern_pgdir[0] = 0;
    assert(pp0->pp_ref == 1);
    pp0->pp_ref = 0;

    // free the pages we took
    page_free(pp0);

    cprintf("check_page_installed_pgdir() succeeded!\n");
}
