/* Author(s): <Your name here>
 * COS 318, Fall 2019: Project 5 Virtual Memory
 * Implementation of the memory manager for the kernel.
*/

/* memory.c
 *
 * Note: 
 * There is no separate swap area. When a data page is swapped out, 
 * it is stored in the location it was loaded from in the process' 
 * image. This means it's impossible to start two processes from the 
 * same image without screwing up the running. It also means the 
 * disk image is read once. And that we cannot use the program disk.
 *
 */

#include "common.h"
#include "kernel.h"
#include "scheduler.h"
#include "memory.h"
#include "thread.h"
#include "util.h"
#include "interrupt.h"
#include "tlb.h"
#include "usb/scsi.h"

/* Static global variables */
// Keep track of all pages: their vaddr, status, and other properties
static page_map_entry_t page_map[PAGEABLE_PAGES];

// address of the kernel page directory (shared by all kernel threads)
static uint32_t *kernel_pdir;

// allocate the kernel page tables
static uint32_t *kernel_ptabs[N_KERNEL_PTS];

//other global variables...

/* Identity-mapped region below the simulated physical memory */
#define KERNEL_MEM_START 0x8000
/* VGA text buffer, must be writable by user processes */
#define VGA_MEM_START 0xb8000
#define VGA_MEM_LIMIT 0xc0000

/* The currently-used victim cursor for the replacement policy */
static int clock_hand = 0;

/* Main API */

/* Use virtual address to get index in page directory. */
uint32_t get_dir_idx(uint32_t vaddr){
  return (vaddr & PAGE_DIRECTORY_MASK) >> PAGE_DIRECTORY_BITS;
}

/* Use virtual address to get index in a page table. */
uint32_t get_tab_idx(uint32_t vaddr){
  return (vaddr & PAGE_TABLE_MASK) >> PAGE_TABLE_BITS;
}

/* Returns physical address of page number i */
uint32_t* page_addr(int i){
  return (uint32_t *) (MEM_START + i * PAGE_SIZE);
}

/* Set flags in a page table entry to 'mode' */
void set_ptab_entry_flags(uint32_t * pdir, uint32_t vaddr, uint32_t mode){
  uint32_t dir_idx = get_dir_idx((uint32_t) vaddr);
  uint32_t tab_idx = get_tab_idx((uint32_t) vaddr);
  uint32_t dir_entry;
  uint32_t *tab;
  uint32_t entry;

  dir_entry = pdir[dir_idx];
  ASSERT(dir_entry & PE_P); /* dir entry present */
  tab = (uint32_t *) (dir_entry & PE_BASE_ADDR_MASK);
  /* clear table[index] bits 11..0 */
  entry = tab[tab_idx] & PE_BASE_ADDR_MASK;

  /* set table[index] bits 11..0 */
  entry |= mode & ~PE_BASE_ADDR_MASK;
  tab[tab_idx] = entry;

  /* Flush TLB because we just changed a page table entry in memory */
  flush_tlb_entry(vaddr);
}

/* Initialize a page table entry
 *  
 * 'vaddr' is the virtual address which is mapped to the physical
 * address 'paddr'. 'mode' sets bit [12..0] in the page table entry.
 *   
 * If user is nonzero, the page is mapped as accessible from a user
 * application.
 */
void init_ptab_entry(uint32_t * table, uint32_t vaddr,
         uint32_t paddr, uint32_t mode){
  int index = get_tab_idx(vaddr);
  table[index] =
    (paddr & PE_BASE_ADDR_MASK) | (mode & ~PE_BASE_ADDR_MASK);
  flush_tlb_entry(vaddr);
}

/* Insert a page table entry into the page directory. 
 *   
 * 'mode' sets bit [12..0] in the page table entry.
 */
void insert_ptab_dir(uint32_t * dir, uint32_t *tab, uint32_t vaddr, 
		       uint32_t mode){

  uint32_t access = mode & MODE_MASK;
  int idx = get_dir_idx(vaddr);

  dir[idx] = ((uint32_t)tab & PE_BASE_ADDR_MASK) | access;
}

/* Allocate a page. Return page index in the page_map directory.
 *
 * Marks page as pinned if pinned == TRUE.
 * Swap out a page if no space is available.
 */
int page_alloc(int pinned){
  int i, page = -1;

  /* First look for a page map slot that has never been used */
  for (i = 0; i < PAGEABLE_PAGES; ++i) {
    if (!page_map[i].in_use) {
      page = i;
      break;
    }
  }

  if (page == -1) {
    /* All slots are in use: pick a victim and store its contents on disk */
    page = page_replacement_policy();
    page_swap_out(page);
  }

  /* Claim the slot */
  page_map[page].in_use = TRUE;
  page_map[page].paged_in = TRUE;
  page_map[page].pinned = pinned;
  page_map[page].dirty = FALSE;

  /* Zero the physical page so the caller starts from a clean slate */
  bzero((char *) page_addr(page), PAGE_SIZE);

  return page;
}

/* Set up kernel memory for kernel threads to run.
 *
 * This method is only called once by _start() in kernel.c, and is only
 * supposed to set up the page directory and page tables for the kernel.
 */
void init_memory(void){
  int i;
  uint32_t vaddr;

  /* Allocate the kernel page directory and the kernel page tables.
   * These pages are pinned: they must never be swapped out. */
  kernel_pdir = page_addr(page_alloc(TRUE));
  bzero((char *) kernel_pdir, PAGE_SIZE);

  for (i = 0; i < N_KERNEL_PTS; ++i) {
    kernel_ptabs[i] = page_addr(page_alloc(TRUE));
    bzero((char *) kernel_ptabs[i], PAGE_SIZE);
  }

  /* Make the first kernel page table cover the low 4 MB of the
   * address space (index 0 of the directory) */
  insert_ptab_dir(kernel_pdir, kernel_ptabs[0], KERNEL_MEM_START,
      PE_P | PE_RW | PE_US);

  /* Identity-map everything the kernel needs in the low 4 MB:
   * the kernel image and stacks, the simulated physical memory,
   * and the VGA text buffer (user processes write to the screen,
   * so those entries are made user-accessible). */
  for (vaddr = 0; vaddr < 0x400000; vaddr += PAGE_SIZE) {
    uint32_t mode;

    if (vaddr >= VGA_MEM_START && vaddr < VGA_MEM_LIMIT) {
      /* VGA text buffer: user processes print to the screen */
      mode = PE_P | PE_RW | PE_US;
    } else if (vaddr < MAX_PHYSICAL_MEMORY) {
      mode = PE_P | PE_RW;
    } else {
      continue;
    }

    init_ptab_entry(kernel_ptabs[0], vaddr, vaddr, mode);
  }
}


/* Set up a page directory and page table for a new
 * user process or thread. */
void setup_page_table(pcb_t * p){
  int i;

  if (p->is_thread) {
    /* Kernel threads run in the (identity-mapped) kernel address
     * space: they can simply share the kernel page directory. */
    p->page_directory = kernel_pdir;
    return;
  }

  /* Allocate the process's own page directory (pinned) */
  p->page_directory = page_addr(page_alloc(TRUE));
  bzero((char *) p->page_directory, PAGE_SIZE);

  /* All jobs share the kernel mappings (directory entry 0) */
  p->page_directory[get_dir_idx(KERNEL_MEM_START)] =
    kernel_pdir[get_dir_idx(KERNEL_MEM_START)];

  /* Allocate a page table for the process image region (pinned) */
  i = page_alloc(TRUE);
  insert_ptab_dir(p->page_directory, page_addr(i), PROCESS_START,
      PE_P | PE_RW | PE_US);

  /* And one for the user stack region (pinned) */
  i = page_alloc(TRUE);
  insert_ptab_dir(p->page_directory, page_addr(i), PROCESS_STACK,
      PE_P | PE_RW | PE_US);

  /* Pre-allocate the user stack pages (pinned, and without any disk
   * backing store besides a hole in the image). */
  for (i = 0; i < N_PROCESS_STACK_PAGES; ++i) {
    uint32_t vaddr = PROCESS_STACK - (i + 1) * PAGE_SIZE;
    int page = page_alloc(TRUE);

    init_ptab_entry(
      (uint32_t *)(p->page_directory[get_dir_idx(vaddr)] & PE_BASE_ADDR_MASK),
      vaddr, (uint32_t) page_addr(page), PE_P | PE_RW | PE_US);

    /* Record the page in the page map */
    page_map[page].vaddr = vaddr;
    page_map[page].page_directory = p->page_directory;
    page_map[page].mode = PE_RW | PE_US;
    page_map[page].dirty = TRUE;
  }
}

/* Swap into a free page upon a page fault.
 * This method is called from interrupt.c: exception_14().
 * Should handle demand paging.
 */
void page_fault_handler(void){
  uint32_t vaddr = current_running->fault_addr & ~PAGE_MASK;
  uint32_t mode;
  uint32_t *dir, *tab;
  page_map_entry_t *pg = NULL;
  int i, page;

  /* The fault must have happened while running a job */
  ASSERT2(current_running->nested_count > 0, "page fault outside kernel!");

  /* Locate the page in the page map, if it has been touched before */
  for (i = 0; i < PAGEABLE_PAGES; ++i) {
    if (page_map[i].in_use &&
        page_map[i].page_directory == current_running->page_directory &&
        page_map[i].vaddr == vaddr) {
      pg = &page_map[i];
      break;
    }
  }

  if (pg == NULL) {
    /* First time this page is touched: demand paging. Allocate a
     * physical page (which may evict a victim) and set up the page
     * map entry for it. */
    page = page_alloc(FALSE);
    pg = &page_map[page];

    dir = current_running->page_directory;
    /* The page table for this region must have been set up */
    ASSERT2(dir[get_dir_idx(vaddr)] & PE_P, "Fault on unmapped region");

    tab = (uint32_t *)(dir[get_dir_idx(vaddr)] & PE_BASE_ADDR_MASK);

    /* User pages are readable, writable and accessible from user mode */
    mode = PE_RW | PE_US;

    pg->vaddr = vaddr;
    pg->page_directory = dir;
    pg->mode = mode;
    /* Backing store: the page's location in the process image */
    pg->swap_loc = current_running->swap_loc;
    pg->dirty = FALSE;

    /* Install the page table entry, absent for now */
    init_ptab_entry(tab, vaddr, (uint32_t) page_addr(page),
        pg->mode & ~PE_P);

    /* Remember one fault */
    current_running->page_fault_count++;
  } else {
    /* The page belongs to this process but is currently absent */
    page = i;
    ASSERT2(!pg->paged_in, "PTE present but the page map says otherwise!");
  }

  /* Read the page back from the process image and mark it
   * present in the page table */
  page_swap_in(page);

  /* Done: retry the faulting instruction */
}

/* Get the sector number on disk of a process image
 * Used for page swapping. */
int get_disk_sector(page_map_entry_t * page){
  return page->swap_loc +
    ((page->vaddr - PROCESS_START) / PAGE_SIZE) * SECTORS_PER_PAGE;
}

/* Swap i-th page in from disk (i.e. the image file) */
void page_swap_in(int i){
  page_map_entry_t *pg = &page_map[i];
  int sector;

  ASSERT(pg->in_use);

  sector = get_disk_sector(pg);
  ASSERT2(scsi_read(sector, SECTORS_PER_PAGE, (char *) page_addr(i)) >= 0,
      "Reading from disk failed");

  pg->paged_in = TRUE;
  pg->dirty = FALSE;

  /* Mark the page as present in the owner's page table */
  set_ptab_entry_flags(pg->page_directory, pg->vaddr, pg->mode | PE_P);
}

/* Swap i-th page out to disk.
 *
 * Write the page back to the process image.
 * There is no separate swap space on the USB.
 *
 */
void page_swap_out(int i){
  page_map_entry_t *pg = &page_map[i];
  int sector;

  ASSERT(pg->in_use);
  ASSERT2(!pg->pinned, "Attempt to evict a pinned page");
  ASSERT2(pg->paged_in, "Attempt to swap out a non-resident page");

  /* Write the page to its location in the process image */
  sector = get_disk_sector(pg);
  ASSERT2(scsi_write(sector, SECTORS_PER_PAGE, (char *) page_addr(i)) >= 0,
      "Writing to disk failed");

  pg->paged_in = FALSE;
  pg->dirty = FALSE;

  /* Mark the page as not present in the owner's page table */
  set_ptab_entry_flags(pg->page_directory, pg->vaddr, pg->mode & ~PE_P);
}

/* Decide which page to replace, return the page number  */
int page_replacement_policy(void){
  int i, t;

  /* Clock-style scan from the last chosen slot: pick the first
   * resident, unpinned page and remember the position for next time */
  for (t = 0; t < PAGEABLE_PAGES; ++t) {
    i = (clock_hand + t) % PAGEABLE_PAGES;
    if (page_map[i].in_use && page_map[i].paged_in &&
        !page_map[i].pinned) {
      clock_hand = (i + 1) % PAGEABLE_PAGES;
      return i;
    }
  }

  /* No page qualifies: the system is out of memory */
  HALT("Out of physical memory: all pages pinned");
  return -1; /* not reached */
}
