/*
 * Copyright (C) 2026 pdnguyen of HCMC University of Technology VNU-HCM
 */

/* Caitoa release
 * Source Code License Grant: The authors hereby grant to Licensee
 * personal permission to use and modify the Licensed Source Code
 * for the sole purpose of studying while attending the course CO2018.
 */

// #ifdef MM_PAGING
/*
 * System Library
 * Memory Module Library libmem.c 
 */

#include "string.h"
#include "mm.h"
#include "mm64.h"
#include "syscall.h"
#include "libmem.h"
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <pthread.h>

static pthread_mutex_t mmvm_lock = PTHREAD_MUTEX_INITIALIZER;

/*enlist_vm_freerg_list - add new rg to freerg_list
 *@mm:        memory region
 *@rg_elmt:   new region
 *
 */
int enlist_vm_freerg_list(struct mm_struct *mm, struct vm_rg_struct *rg_elmt)
{
  if (rg_elmt->rg_start >= rg_elmt->rg_end)
    return -1;

  // struct vm_rg_struct *rg_node = mm->mmap->vm_freerg_list;
  struct vm_area_struct *cur_vma = get_vma_by_num(mm, rg_elmt->vmaid);
  if (cur_vma == NULL) return -1;
  struct vm_rg_struct *cur = cur_vma->vm_freerg_list;

  struct vm_rg_struct *prev = NULL;

  // if (rg_node != NULL)
  //   rg_elmt->rg_next = rg_node;

  // /* Enlist the new region */
  // mm->mmap->vm_freerg_list = rg_elmt;

  /* Tìm vị trí chèn theo thứ tự tăng dần của địa chỉ bắt đầu */
  while (cur != NULL && cur->rg_start < rg_elmt->rg_start)
  {
    prev = cur;
    cur = cur->rg_next;
  }

  /* Chèn node mới vào giữa prev và cur */
  if (prev == NULL)
  {
    rg_elmt->rg_next = cur_vma->vm_freerg_list;
    cur_vma->vm_freerg_list = rg_elmt;
  }
  else
  {
    prev->rg_next = rg_elmt;
    rg_elmt->rg_next = cur;
  }

  /* Gộp với vùng kế tiếp nếu liền kề và cùng mode */
  if (rg_elmt->rg_next != NULL && rg_elmt->rg_end == rg_elmt->rg_next->rg_start && rg_elmt->mode_bit == rg_elmt->rg_next->mode_bit)
  {
    struct vm_rg_struct *next = rg_elmt->rg_next;
    rg_elmt->rg_end = next->rg_end;
    rg_elmt->rg_next = next->rg_next;
    free(next);
  }

  /* Gộp với vùng phía trước nếu liền kề và cùng mode */
  if (prev != NULL && prev->rg_end == rg_elmt->rg_start && prev->mode_bit == rg_elmt->mode_bit)
  {
    prev->rg_end = rg_elmt->rg_end;
    prev->rg_next = rg_elmt->rg_next;
    free(rg_elmt);
  }

  return 0;
}

/*get_symrg_byid - get mem region by region ID
 *@mm:    memory region
 *@rgid:  region ID act as symbol index of variable
 *
 */
struct vm_rg_struct *get_symrg_byid(struct mm_struct *mm, int rgid)
{
  if (rgid < 0 || rgid > PAGING_MAX_SYMTBL_SZ)
    return NULL;

  return &mm->symrgtbl[rgid];
}

/*__alloc - allocate a region memory
 *@caller:      caller
 *@vmaid:       ID vm area to alloc memory region
 *@rgid:        memory region ID (used to identify variable in symbole table)
 *@size:        allocated size
 *@alloc_addr:  address of allocated memory region
 *
 */
int __alloc(struct pcb_t *caller, int vmaid, int rgid, addr_t size, addr_t *alloc_addr)
{
  /*Allocate at the toproof */
  pthread_mutex_lock(&mmvm_lock);
  struct vm_rg_struct rgnode;
  struct vm_area_struct *cur_vma = get_vma_by_num(caller->krnl->mm, vmaid);
  int inc_sz = 0;

  /* kernel finds free and fit vm region (mode_bit=0), then hand over to user */
  if (get_free_vmrg_area(caller, vmaid, size, &rgnode) == 0)
  {
    caller->krnl->mm->symrgtbl[rgid].rg_start = rgnode.rg_start;
    caller->krnl->mm->symrgtbl[rgid].rg_end = rgnode.rg_end;
    caller->krnl->mm->symrgtbl[rgid].mode_bit = 1;
 
    *alloc_addr = rgnode.rg_start;

    pthread_mutex_unlock(&mmvm_lock);
    return 0;
  }

  /* TODO get_free_vmrg_area FAILED handle the region management (Fig.6)*/ // Fig.7?

  /*Attempt to increate limit to get space */
  addr_t old_sbrk = cur_vma->sbrk;

  /* TODO INCREASE THE LIMIT
   * SYSCALL 1 sys_memmap
   */
  struct sc_regs regs;
  regs.a1 = SYSMEM_INC_OP;
  regs.a2 = vmaid;
  /*
   * Page alligning is performed inside the syscall 
   * Pass in raw size here
   */ 
  regs.a3 = size;

  /** 
   * SYSCALL 17 sys_memmap 
   * Calls inc_vma_limit() in mm-vm.c
   */
  _syscall(caller->krnl, caller->pid, 17, &regs); 

  /*Successful increase limit */
  caller->krnl->mm->symrgtbl[rgid].rg_start = old_sbrk;
  caller->krnl->mm->symrgtbl[rgid].rg_end = old_sbrk + size;
  caller->krnl->mm->symrgtbl[rgid].mode_bit = 1;

  *alloc_addr = old_sbrk;

  pthread_mutex_unlock(&mmvm_lock);
  return 0;

}

/*__free - remove a region memory
 *@caller:  caller
 *@vmaid:   ID vm area to alloc memory region
 *@rgid:    memory region ID (used to identify variable in symbole table)
 *@size:    allocated size
 *
 */
int __free(struct pcb_t *caller, int vmaid, int rgid)
{
  pthread_mutex_lock(&mmvm_lock);

  if (rgid < 0 || rgid > PAGING_MAX_SYMTBL_SZ)
  {
    pthread_mutex_unlock(&mmvm_lock);
    return -1;
  }

  /* TODO: Manage the collect freed region to freerg_list */
  struct vm_rg_struct *rgnode = get_symrg_byid(caller->krnl->mm, rgid);

  if (rgnode->rg_start == 0 && rgnode->rg_end == 0)
  {
    pthread_mutex_unlock(&mmvm_lock);
    return -1;
  }
  struct vm_rg_struct *freerg_node = malloc(sizeof(struct vm_rg_struct));
  freerg_node->rg_start = rgnode->rg_start;
  freerg_node->rg_end = rgnode->rg_end;
  freerg_node->mode_bit = rgnode->mode_bit;
  freerg_node->rg_next = NULL;
  freerg_node->vmaid = vmaid;

  rgnode->rg_start = rgnode->rg_end = 0;
  rgnode->mode_bit = 0;
  rgnode->rg_next = NULL;

  /*enlist the obsoleted memory region */
  enlist_vm_freerg_list(caller->krnl->mm, freerg_node);

  pthread_mutex_unlock(&mmvm_lock);
  return 0;
}

/*liballoc - PAGING-based allocate a region memory
 *@proc:        Process executing the instruction
 *@size:        allocated size (number of bytes)
 *@reg_index:   memory region ID (used to identify variable in symbole table)
 */
int liballoc(struct pcb_t *proc, addr_t size, uint32_t reg_index)
{
  addr_t  addr;
  int val = __alloc(proc, 1, reg_index, size, &addr);
  if (val == -1)
  {
    return -1;
  }
#ifdef IODUMP
  /* TODO dump IO content (if needed) */
  printf("%s:%d\n",__func__,__LINE__);
#ifdef PAGETBL_DUMP
  print_pgtbl(proc, 0, -1); // print max TBL
#endif
#endif

  /* By default using vmaid = 0 */
  return val;
}

/*libfree - PAGING-based free a region memory
 *@proc:        Process executing the instruction
 *@size:        allocated size
 *@reg_index:   memory region ID (used to identify variable in symbole table)
 */

int libfree(struct pcb_t *proc, uint32_t reg_index)
{
  int val = __free(proc, 0, reg_index);
  if (val == -1)
  {
    return -1;
  }

#ifdef IODUMP
  /* TODO dump IO content (if needed) */
  printf("%s:%d\n",__func__,__LINE__);
#ifdef PAGETBL_DUMP
  print_pgtbl(proc, 0, -1); // print max TBL
#endif
#endif
  return 0;//val;
}

/*pg_getpage - get the page in ram
 *@mm:        memory region
 *@pgn:       PGN
 *@fpn:       return FPN
 *@caller:    caller
 *
 */
int pg_getpage(struct mm_struct *mm, addr_t pgn, int *fpn, struct pcb_t *caller)
{

  uint32_t pte = pte_get_entry(caller, pgn);

  if (!PAGING_PAGE_PRESENT(pte))
  { /* Page is not online, make it actively living */
    addr_t vicpgn, swpfpn;
    addr_t vicfpn;
    uint32_t vicpte;

    /* TODO Initialize the target frame storing our variable */
    addr_t tgtfpn = PAGING_SWP(pte);

    /* TODO: Play with your paging theory here */
    /* Find victim page */
    if (find_victim_page(caller->krnl->mm, &vicpgn) == -1)
    {
      return -1;
    }

    vicpte = pte_get_entry(caller, vicpgn);
    vicfpn = PAGING_FPN(vicpte);

    /* Get free frame in MEMSWP */
    if (MEMPHY_get_freefp(caller->krnl->active_mswp, &swpfpn) == -1)
    {
      return -1;
    }

    /* TODO: Implement swap frame from MEMRAM to MEMSWP and vice versa*/

    /* TODO copy victim frame to swap 
     * SWP(vicfpn <--> swpfpn)
     * SYSCALL 1 sys_memmap ()
     */
    struct sc_regs regs;
    regs.a1 = SYSMEM_SWP_OP;
    regs.a2 = vicfpn;
    regs.a3 = swpfpn;
    _syscall(caller->krnl, caller->pid, 17, &regs);

    __swap_cp_page(caller->krnl->active_mswp, tgtfpn, caller->krnl->mram, vicfpn);

    /* Update page table */
    pte_set_swap(caller, vicpgn, 0, swpfpn);

    /* Update its online status of the target page */
    pte_set_fpn(caller, pgn, vicfpn);

    enlist_pgn_node(&caller->krnl->mm->fifo_pgn, pgn);
  }

  *fpn = PAGING_FPN(pte_get_entry(caller,pgn));

  return 0;
}

/*pg_getval - read value at given offset
 *@mm:      memory region
 *@addr:    virtual address to acess
 *@value:   value
 *
 */
int pg_getval(struct mm_struct *mm, addr_t addr, BYTE *data, struct pcb_t *caller)
{
#ifdef MM64
  addr_t pgn = addr >> PAGING64_ADDR_PT_SHIFT;
  int off = addr & 0xFFF;
#else
  addr_t pgn = PAGING_PGN(addr);
  int off = PAGING_OFFST(addr);
#endif
  int fpn;

  if (pg_getpage(mm, pgn, &fpn, caller) != 0)
    return -1; /* invalid page access */

  addr_t phyaddr;
#ifdef MM64
  phyaddr = fpn * PAGING64_PAGESZ + off;
#else
  phyaddr = fpn * PAGING_PAGESZ + off;
#endif
  //int phyaddr = (fpn << PAGING_ADDR_FPN_LOBIT) + off;

  /* TODO 
   *  MEMPHY_read(caller->krnl->mram, phyaddr, data);
   *  MEMPHY READ 
   *  SYSCALL 17 sys_memmap with SYSMEM_IO_READ
   */

#ifdef IODUMP
  printf("%s:%d\n",__func__,__LINE__);
#endif

struct sc_regs regs;
  regs.a1 = SYSMEM_IO_READ;
  regs.a2 = phyaddr;
  regs.a3 = data;
  _syscall(caller->krnl, caller->pid, 17, &regs);
  *data = (BYTE)regs.a3;
  return 0;
}

/*pg_setval - write value to given offset
 *@mm:      memory region
 *@addr:    virtual address to acess
 *@value:   value
 *
 */
int pg_setval(struct mm_struct *mm, addr_t addr, BYTE value, struct pcb_t *caller)
{
#ifdef MM64
  addr_t pgn = addr >> PAGING64_ADDR_PT_SHIFT;
  int off = addr & 0xFFF;
#else
  addr_t pgn = PAGING_PGN(addr);
  int off = PAGING_OFFST(addr);
#endif
  int fpn;

  /* Get the page to MEMRAM, swap from MEMSWAP if needed */
  if (pg_getpage(mm, pgn, &fpn, caller) != 0)
    return -1; /* invalid page access */

  addr_t phyaddr;
#ifdef MM64
  phyaddr = fpn * PAGING64_PAGESZ + off;
#else
  phyaddr = fpn * PAGING_PAGESZ + off;
#endif

  /* TODO 
   *  MEMPHY_write(caller->krnl->mram, phyaddr, value);
   *  MEMPHY WRITE with SYSMEM_IO_WRITE 
   * SYSCALL 17 sys_memmap
   */

#ifdef IODUMP
  printf("%s:%d\n",__func__,__LINE__);
#endif

struct sc_regs regs;
  regs.a1 = SYSMEM_IO_WRITE;
  regs.a2 = phyaddr;
  regs.a3 = value;
  _syscall(caller->krnl, caller->pid, 17, &regs);

  return 0;
}

/*__read - read value in region memory
 *@caller:  caller
 *@vmaid:   ID vm area to alloc memory region
 *@offset:  offset to acess in memory region
 *@rgid:    memory region ID (used to identify variable in symbole table)
 *@size:    allocated size
 *
 */
int __read(struct pcb_t *caller, int vmaid, int rgid, addr_t offset, BYTE *data)
{
  pthread_mutex_lock(&mmvm_lock);
  struct vm_rg_struct *currg = get_symrg_byid(caller->krnl->mm, rgid);

//struct vm_area_struct *cur_vma = get_vma_by_num(caller->krnl->mm, vmaid);

  /* TODO Invalid memory identify */
  if (currg == NULL || currg->mode_bit != 1)
  {
    pthread_mutex_unlock(&mmvm_lock);
    return -1;
  }

  pg_getval(caller->krnl->mm, currg->rg_start + offset, data, caller);

  pthread_mutex_unlock(&mmvm_lock);
  return 0;
}

/*libread - PAGING-based read a region memory */
int libread(
    struct pcb_t *proc, // Process executing the instruction
    uint32_t source,    // Index of source register
    addr_t offset,    // Source address = [source] + [offset]
    uint32_t* destination)
{
  BYTE data;

  int val = __read(proc, 0, source, offset, &data);

  *destination = data;
#ifdef IODUMP
  /* TODO dump IO content (if needed) */
  printf("%s:%d\n",__func__,__LINE__);
#ifdef PAGETBL_DUMP
  print_pgtbl(proc, 0, -1); // print max TBL
#endif
#endif

  return val;
}

/*__write - write a region memory
 *@caller:    caller
 *@vmaid:     ID vm area to alloc memory region
 *@offset:    offset to acess in memory region
 *@rgid:      memory region ID (used to identify variable in symbole table)
 *@size:      allocated size
 *
 */
int __write(struct pcb_t *caller, int vmaid, int rgid, addr_t offset, BYTE value)
{
  pthread_mutex_lock(&mmvm_lock);
  struct vm_rg_struct *currg = get_symrg_byid(caller->krnl->mm, rgid);

  struct vm_area_struct *cur_vma = get_vma_by_num(caller->krnl->mm, vmaid);

  if (currg == NULL || cur_vma == NULL || currg->mode_bit != 1) /* Invalid memory identify */
  {
    pthread_mutex_unlock(&mmvm_lock);
    return -1;
  }

  pg_setval(caller->krnl->mm, currg->rg_start + offset, value, caller);

  pthread_mutex_unlock(&mmvm_lock);
  return 0;
}

/*libwrite - PAGING-based write a region memory */
int libwrite(
    struct pcb_t *proc,   // Process executing the instruction
    BYTE data,            // Data to be wrttien into memory
    uint32_t destination, // Index of destination register
    addr_t offset)
{
  int val = __write(proc, 0, destination, offset, data);
  if (val == -1)
  {
    return -1;
  }
#ifdef IODUMP
  /* TODO dump IO content (if needed) */
  printf("%s:%d\n",__func__,__LINE__);
#ifdef PAGETBL_DUMP
  print_pgtbl(proc, 0, -1); // print max TBL
#endif
#endif

  return val;
}


/*libkmem_malloc- alloc region memory in kmem
 *@caller:  caller
 *@rgid:    memory region ID (used to identify variable in symbole table)
 *@size:    memory size
 */

int libkmem_malloc(struct pcb_t * caller, uint32_t size, uint32_t reg_index)
{
  /* TODO: provide OS level management
   *       and forward the request to helper
   */
  addr_t  addr;
  int val = __kmalloc(caller, -1, reg_index, size, &addr);

  /* TODO: provide OS kmem allocation validation
   */
#ifdef IODUMP
  printf("%s:%d\n",__func__,__LINE__);
#ifdef PAGETBL_DUMP
  print_pgtbl(caller, 0, -1); // print max TBL
#endif
#endif

  return val;
}



struct buddy_block *buddy_free_lists[BUDDY_MAX_ORDER + 1] = {NULL};
int buddy_initialized = 0;


#ifdef MM64
static addr_t slab_brk = VMEMMAP_BASE;
#endif


void init_kmem_pool(struct pcb_t *caller) {
    if (buddy_initialized) return;
    buddy_initialized = 1;

#ifdef MM64
    int page_sz = PAGING64_PAGESZ;
#else
    int page_sz = PAGING_PAGESZ;
#endif

    int total_pages = 1 << BUDDY_MAX_ORDER;
    struct vm_rg_struct ret_rg;
    
    if(KMALLOC_BASE + (total_pages) * page_sz > KMALLOC_END) return -1;
    // Request a large block mapping from Kernel base

    if (vm_map_kernel(caller, KMALLOC_BASE, KMALLOC_BASE + (total_pages * page_sz), 
                      KMALLOC_BASE, total_pages, &ret_rg) < 0) {
        return; 
    }

    struct buddy_block *root = malloc(sizeof(struct buddy_block));
    root->addr = KMALLOC_BASE;
    root->order = BUDDY_MAX_ORDER;
    root->is_free = 1;
    root->next_free = NULL;

    buddy_free_lists[BUDDY_MAX_ORDER] = root;
}



/*kmalloc - alloc region memory in kmem
 *@caller:        caller
 *@vmaid:         ID vm area to alloc memory region
 *@rgid:          memory region ID (used to identify variable in symbole table)
 *@size:          memory size
 *@alloc_addr:    allocated address
 */
addr_t __kmalloc(struct pcb_t *caller, int vmaid, int rgid, addr_t size, addr_t *alloc_addr)
{
  init_kmem_pool(caller);
  
#ifdef MM64
  int pgnum = PAGING64_PAGE_ALIGNSZ(size) / PAGING64_PAGESZ;
#else
  int pgnum = PAGING_PAGE_ALIGNSZ(size) / PAGING_PAGESZ;
#endif

  int order = 0;
  while ((1 << order) < pgnum) order++;

  if (order > BUDDY_MAX_ORDER) return -1; // Request too large

  // Find free block
  int cur_order = order;
  while (cur_order <= BUDDY_MAX_ORDER && buddy_free_lists[cur_order] == NULL) {
      cur_order++;
  }

  if (cur_order > BUDDY_MAX_ORDER) return -1; // Out of memory

  // Split block until we reach requested order
  while (cur_order > order) {
      struct buddy_block *block = buddy_free_lists[cur_order];
      buddy_free_lists[cur_order] = block->next_free;

      cur_order--;
      struct buddy_block *buddy = malloc(sizeof(struct buddy_block));
      buddy->addr = block->addr + (1 << cur_order) * PAGING64_PAGESZ;
      buddy->order = cur_order;
      buddy->is_free = 1;

      block->order = cur_order;

      buddy->next_free = buddy_free_lists[cur_order];
      block->next_free = buddy;
      buddy_free_lists[cur_order] = block;
  }

  // Allocate from target order
  struct buddy_block *alloc_block = buddy_free_lists[order];
  buddy_free_lists[order] = alloc_block->next_free;
  alloc_block->is_free = 0;

  addr_t ret_addr = alloc_block->addr;

  if (rgid >= 0 && rgid < PAGING_MAX_SYMTBL_SZ)
  {
    caller->krnl->mm->symrgtbl[rgid].rg_start = ret_addr;
    caller->krnl->mm->symrgtbl[rgid].rg_end = ret_addr + (1 << order) * PAGING64_PAGESZ;
    caller->krnl->mm->symrgtbl[rgid].mode_bit = 0;
  }

  *alloc_addr = ret_addr;
  return ret_addr;
}

/*libkmem_cache_pool_create - create cache pool in kmem
 *@caller:          caller
 *@size:            memory size
 *@align:           alignment size of each cache slot (identical cache slot size)
 *@cache_pool_id:   cache pool ID
 */
int libkmem_cache_pool_create(struct pcb_t *caller, uint32_t size, uint32_t align, uint32_t cache_pool_id)
{
  struct krnl_t *krnl = caller->krnl;

  if (cache_pool_id >= PAGING_MAX_SYMTBL_SZ) return -1;

  if (krnl->mm->kcpooltbl == NULL) {
      krnl->mm->kcpooltbl = calloc(PAGING_MAX_SYMTBL_SZ, sizeof(struct kcache_pool_struct));
  }

  struct kcache_pool_struct *pool = &krnl->mm->kcpooltbl[cache_pool_id];
  pool->size = size;
  pool->align = align;
  pool->partial_list = NULL;
  pool->full_list = NULL;
  pool->empty_list = NULL;

  // Allocate first slab
  addr_t slab_addr;
#ifdef MM64
  int page_sz = PAGING64_PAGESZ;
#else
  int page_sz = PAGING_PAGESZ;
#endif
  if (slab_brk + page_sz > VMEMMAP_END) return -1; 

  struct vm_rg_struct ret_rg;
  if (vm_map_kernel(caller, slab_brk, slab_brk + page_sz, slab_brk, 1, &ret_rg) < 0) {
      return -1; 
  }
  slab_addr = slab_brk;
  slab_brk += page_sz; 

  struct slab_struct *slab = malloc(sizeof(struct slab_struct));
  slab->start_addr = slab_addr;
  slab->num_slots = page_sz / align;
  slab->free_slots = slab->num_slots;
  slab->bitmap = calloc(slab->num_slots, sizeof(BYTE));
  slab->next = NULL;

  pool->empty_list = slab;

#ifdef IODUMP
  printf("%s:%d\n",__func__,__LINE__);
#ifdef PAGETBL_DUMP
  print_pgtbl(caller, 0, -1); // print max TBL
#endif
#endif

  return 0;
}

/*libkmem_cache_alloc - allocate cache slot in cache pool, cache slot has identical size
 * the allocated size is embedded in pool management mechanism
 *@caller:          caller
 *@cache_pool_id:   cache pool ID
 *@reg_index:       memory region index
 */
int libkmem_cache_alloc(struct pcb_t *proc, uint32_t cache_pool_id, uint32_t reg_index)
{
  addr_t addr;
  int val = __kmem_cache_alloc(proc, -1, reg_index, cache_pool_id, &addr);

#ifdef IODUMP
  printf("%s:%d\n",__func__,__LINE__);
#ifdef PAGETBL_DUMP
  print_pgtbl(proc, 0, -1); // print max TBL
#endif
#endif

  return val;
}

/*kmem_cache_alloc - alloc region memory in kmem cache
 *@caller:          caller
 *@vmaid:           ID vm area to alloc memory region
 *@rgid:            memory region ID (used to identify variable in symbole table)
 *@cache_pool_id:   cached pool ID
 *@alloc_addr:      allocated address
 */
addr_t __kmem_cache_alloc(struct pcb_t *caller, int vmaid, int rgid, int cache_pool_id, addr_t *alloc_addr)
{
  struct krnl_t *krnl = caller->krnl;

  if (cache_pool_id < 0 || cache_pool_id >= PAGING_MAX_SYMTBL_SZ) return -1;
  if (rgid < 0 || rgid >= PAGING_MAX_SYMTBL_SZ) return -1;
  if (krnl->mm->kcpooltbl == NULL) return -1;

  struct kcache_pool_struct *pool = &krnl->mm->kcpooltbl[cache_pool_id];
  if (pool->align == 0) return -1; // Uninitialized pool

  struct slab_struct *slab = NULL;

  if (pool->partial_list != NULL) {
      slab = pool->partial_list;
  } else if (pool->empty_list != NULL) {
      slab = pool->empty_list;
      pool->empty_list = slab->next;
      slab->next = pool->partial_list;
      pool->partial_list = slab;
  } else {
      // Need a new slab
#ifdef MM64
      int page_sz = PAGING64_PAGESZ;
#else
      int page_sz = PAGING_PAGESZ;
#endif
      addr_t slab_addr;
      if(slab_brk + page_sz > VMEMMAP_END) return -1;

      struct vm_rg_struct ret_rg;
      if(vm_map_kernel(caller, slab_brk, slab_brk + page_sz, slab_brk, 1, &ret_rg) < 0) return -1;
      slab_addr = slab_brk;
      slab_brk += page_sz;

      slab = malloc(sizeof(struct slab_struct));
      slab->start_addr = slab_addr;
      slab->num_slots = page_sz / pool->align;
      slab->free_slots = slab->num_slots;
      slab->bitmap = calloc(slab->num_slots, sizeof(BYTE));

      slab->next = pool->partial_list;
      pool->partial_list = slab;
  }

  // Find free slot
  int slot_idx = -1;
  for (int i = 0; i < slab->num_slots; i++) {
      if (slab->bitmap[i] == 0) {
          slot_idx = i;
          slab->bitmap[i] = 1;
          slab->free_slots--;
          break;
      }
  }

  if (slot_idx == -1) return -1;

  // If full, move to full_list
  if (slab->free_slots == 0) {
      if (pool->partial_list == slab) {
          pool->partial_list = slab->next;
      } else {
          struct slab_struct *prev = pool->partial_list;
          while (prev != NULL && prev->next != slab) prev = prev->next;
          if (prev != NULL) prev->next = slab->next;
      }
      slab->next = pool->full_list;
      pool->full_list = slab;
  }

  addr_t ret_addr = slab->start_addr + (slot_idx * pool->align);

  krnl->mm->symrgtbl[rgid].rg_start = ret_addr;
  krnl->mm->symrgtbl[rgid].rg_end = ret_addr + pool->align;
  krnl->mm->symrgtbl[rgid].mode_bit = 0;

  *alloc_addr = ret_addr;
  return ret_addr;
}


int libkmem_copy_from_user(struct pcb_t *caller, uint32_t source, uint32_t destination, uint32_t offset, uint32_t size)
{
  /* TODO: provide OS level management kmem
   */
  BYTE data;
  int i;
  for (i = 0; i < size; i++)
  {
    if (__read_user_mem(caller, 0, source, offset + i, &data) < 0)
      return -1;
    if (__write_kernel_mem(caller, 0, destination, i, data) < 0)
      return -1;
  }
#ifdef IODUMP
  printf("%s:%d\n",__func__,__LINE__);
#ifdef PAGETBL_DUMP
  print_pgtbl(caller, 0, -1); // print max TBL
#endif
#endif

  return 0;
}

int libkmem_copy_to_user(struct pcb_t *caller, uint32_t source, uint32_t destination, uint32_t offset, uint32_t size)
{
  /* TODO: provide OS level management kmem
   */
  BYTE data;
  int i;
  for (i = 0; i < size; i++)
  {
    if (__read_kernel_mem(caller, 0, source, i, &data) < 0)
      return -1;
    if (__write_user_mem(caller, 0, destination, offset + i, data) < 0)
      return -1;
  }
#ifdef IODUMP
  printf("%s:%d\n",__func__,__LINE__);
#ifdef PAGETBL_DUMP
  print_pgtbl(caller, 0, -1); // print max TBL
#endif
#endif

  return 0;
}


/*__read_kernel_mem - read value in kernel region memory
 *@caller:    caller
 *@vmaid:     ID vm area to alloc memory region
 *@rgid:      memory region ID (used to identify variable in symbole table)
 *@offset:    offset to acess in memory region
 *@value:     data value
 */
int __read_kernel_mem(struct pcb_t *caller, int vmaid, int rgid, addr_t offset, BYTE *data)
{
  struct vm_rg_struct *krg = &caller->krnl->mm->symrgtbl[rgid];
  if (krg->rg_start == 0 || krg->rg_end == 0 || krg->mode_bit != 0)
    return -1;
  
  addr_t read_addr = krg->rg_start + offset;
  if (read_addr >= krg->rg_end)
    return -1;
  
  addr_t pgn = read_addr >> PAGING64_ADDR_PT_SHIFT;
  int off = read_addr & 0xFFF;
  addr_t pgd_i, p4d_i, pud_i, pmd_i, pt_i;
  
#ifdef MM64
  get_pd_from_pagenum(pgn, &pgd_i, &p4d_i, &pud_i, &pmd_i, &pt_i);
  addr_t *p4d = (addr_t *)caller->krnl->krnl_pgd[pgd_i];
  if (!p4d) return -1;
  addr_t *pud = (addr_t *)p4d[p4d_i];
  if (!pud) return -1;
  addr_t *pmd = (addr_t *)pud[pud_i];
  if (!pmd) return -1;
  addr_t *pt = (addr_t *)pmd[pmd_i];
  if (!pt) return -1;
  addr_t pte = pt[pt_i];
#else
  addr_t pte = caller->krnl->krnl_pgd[pgn];
#endif

  if (!PAGING_PAGE_PRESENT(pte)) return -1;
  int fpn = PAGING_FPN(pte);
  int phyaddr;
#ifdef MM64
  phyaddr = fpn * PAGING64_PAGESZ + off;
#else
  phyaddr = fpn * PAGING_PAGESZ + off;
#endif

  if (phyaddr < 0 || phyaddr >= caller->krnl->mram->maxsz)
    return -1;
  
  MEMPHY_read(caller->krnl->mram, phyaddr, data);

  return 0;
}

/*__write_kernel_mem - write a kernel region memory
 *@caller:    caller
 *@vmaid:     ID vm area to alloc memory region
 *@rgid:      memory region ID (used to identify variable in symbole table)
 *@offset:    offset to acess in memory region
 *@value:     data value
 */
int __write_kernel_mem(struct pcb_t *caller, int vmaid, int rgid, addr_t offset, BYTE value)
{
  struct vm_rg_struct *krg = &caller->krnl->mm->symrgtbl[rgid];
  if (krg->rg_start == 0 || krg->rg_end == 0 || krg->mode_bit != 0)
    return -1;
  
  addr_t write_addr = krg->rg_start + offset;
  if (write_addr >= krg->rg_end)
    return -1;
  
  addr_t pgn = write_addr >> PAGING64_ADDR_PT_SHIFT;
  int off = write_addr & 0xFFF;
  addr_t pgd_i, p4d_i, pud_i, pmd_i, pt_i;

#ifdef MM64
  get_pd_from_pagenum(pgn, &pgd_i, &p4d_i, &pud_i, &pmd_i, &pt_i);
  addr_t *p4d = (addr_t *)caller->krnl->krnl_pgd[pgd_i];
  if (!p4d) return -1;
  addr_t *pud = (addr_t *)p4d[p4d_i];
  if (!pud) return -1;
  addr_t *pmd = (addr_t *)pud[pud_i];
  if (!pmd) return -1;
  addr_t *pt = (addr_t *)pmd[pmd_i];
  if (!pt) return -1;
  addr_t pte = pt[pt_i];
#else
  addr_t pte = caller->krnl->krnl_pgd[pgn];
#endif

  if (!PAGING_PAGE_PRESENT(pte)) return -1;
  int fpn = PAGING_FPN(pte);
  int phyaddr;
#ifdef MM64
  phyaddr = fpn * PAGING64_PAGESZ + off;
#else
  phyaddr = fpn * PAGING_PAGESZ + off;
#endif

  if (phyaddr < 0 || phyaddr >= caller->krnl->mram->maxsz)
    return -1;
  
  MEMPHY_write(caller->krnl->mram, phyaddr, value);

  return 0;
}

/*__read_user_mem - read value in user region memory
 *@caller:    caller
 *@vmaid:     ID vm area to alloc memory region
 *@rgid:      memory region ID (used to identify variable in symbole table)
 *@offset:    offset to acess in memory region
 *@value:     data value
 */
int __read_user_mem(struct pcb_t *caller, int vmaid, int rgid, addr_t offset, BYTE *data)
{
  /* TODO: provide OS level management user memory access */
  struct vm_rg_struct *urg = &caller->krnl->mm->symrgtbl[rgid];
  if (urg->rg_start == 0 || urg->rg_end == 0 || urg->mode_bit != 1)
    return -1;
  
  addr_t read_addr = urg->rg_start + offset;
  if (read_addr >= urg->rg_end)
    return -1;
  
  return pg_getval(caller->krnl->mm, read_addr, data, caller);
}


/*__write_user_mem - write a user region memory
 *@caller:    caller
 *@vmaid:     ID vm area to alloc memory region
 *@rgid:      memory region ID (used to identify variable in symbole table)
 *@offset:    offset to acess in memory region
 *@value:     data value
 */
int __write_user_mem(struct pcb_t *caller, int vmaid, int rgid, addr_t offset, BYTE value)
{
  /* TODO: provide OS level management user memory access */
  struct vm_rg_struct *urg = &caller->krnl->mm->symrgtbl[rgid];
  if (urg->rg_start == 0 || urg->rg_end == 0 || urg->mode_bit != 1)
    return -1;
  
  addr_t write_addr = urg->rg_start + offset;
  if (write_addr >= urg->rg_end)
    return -1;
  
  return pg_setval(caller->krnl->mm, write_addr, value, caller);
}


/*free_pcb_memphy - collect all memphy of pcb
 *@caller:      caller
 *@vmaid:       ID vm area to alloc memory region
 *@incpgnum:    number of page
 */
int free_pcb_memph(struct pcb_t *caller)
{
  pthread_mutex_lock(&mmvm_lock);
  int pagenum, fpn;
  uint32_t pte;

  for (pagenum = 0; pagenum < PAGING_MAX_PGN; pagenum++)
  {
    pte = caller->krnl->mm->pgd[pagenum];

    if (PAGING_PAGE_PRESENT(pte))
    {
      fpn = PAGING_FPN(pte);
      MEMPHY_put_freefp(caller->krnl->mram, fpn);
    }
    else
    {
      fpn = PAGING_SWP(pte);
      MEMPHY_put_freefp(caller->krnl->active_mswp, fpn);
    }
  }

  pthread_mutex_unlock(&mmvm_lock);
  return 0;
}


/*find_victim_page - find victim page
 *@caller:  caller
 *@pgn:     return page number
 *
 */
int find_victim_page(struct mm_struct *mm, addr_t *retpgn)
{
  struct pgn_t *pg = mm->fifo_pgn;

  /* TODO: Implement the theorical mechanism to find the victim page */
  if (!pg)
  {
    return -1;
  }
  if (pg->pg_next == NULL)
  {
    *retpgn = pg->pgn;
    mm->fifo_pgn = NULL;
    free(pg);
    return 0;
  }
  struct pgn_t *prev = NULL;
  while (pg->pg_next)
  {
    prev = pg;
    pg = pg->pg_next;
  }
  *retpgn = pg->pgn;
  if (prev)
    prev->pg_next = NULL;
  else
    mm->fifo_pgn = NULL;

  free(pg);

  return 0;
}

/*get_free_vmrg_area - get a free vm region
 *@caller:    caller
 *@vmaid:     ID vm area to alloc memory region
 *@size:      allocated size
 *
 */
int get_free_vmrg_area(struct pcb_t *caller, int vmaid, int size, struct vm_rg_struct *newrg)
{
  struct vm_area_struct *cur_vma = get_vma_by_num(caller->krnl->mm, vmaid);

  struct vm_rg_struct *rgit = cur_vma->vm_freerg_list;

  if (rgit == NULL)
    return -1;

  /* Probe unintialized newrg */
  newrg->rg_start = newrg->rg_end = -1;
  newrg->mode_bit = 0;

  /* Traverse on list of free vm region to find a fit space */
  while (rgit != NULL)
  {
    if (rgit->rg_start + size <= rgit->rg_end)
    { /* Current region has enough space */
      newrg->rg_start = rgit->rg_start;
      newrg->rg_end = rgit->rg_start + size;
      newrg->mode_bit = rgit->mode_bit;

      /* Update left space in chosen region */
      if (rgit->rg_start + size < rgit->rg_end)
      {
        rgit->rg_start = rgit->rg_start + size;
      }
      else /* rg_start + size == rg_end */
      { /*Use up all space, remove current node */
        /*Clone next rg node */
        struct vm_rg_struct *nextrg = rgit->rg_next;

        /*Cloning */
        if (nextrg != NULL)
        {
          rgit->rg_start = nextrg->rg_start;
          rgit->rg_end = nextrg->rg_end;
          rgit->mode_bit = nextrg->mode_bit;
          rgit->rg_next = nextrg->rg_next;

          free(nextrg);
        }
        else
        { /* End of free list */                          
          rgit->rg_start = rgit->rg_end; // dummy, size 0 region
          rgit->rg_next = NULL;
        }
      }
      break;
    }
    else
    { /* No space in current region */
      rgit = rgit->rg_next; // Traverse next rg
    }
  }

  if (newrg->rg_start == -1) // new region not found
    return -1;

  return 0;
}

// #endif // MM_PAGING
