/*
 * Bhyve module to implement target memory allocation
 * (from guest memory) and various sundry APIs that
 * would normally be satisfied in non-emu grub from
 * other modules that don't compile easily or work in
 * an emu build.
 */
#include <sys/queue.h>

#include <grub/err.h>
#include <grub/misc.h>
#include <grub/mm.h>
#include <grub/relocator.h>
#include <grub/dl.h>
#include <grub/file.h>
#include <grub/aout.h>
#include <grub/ns8250.h>

#include <grub/i386/io.h>
#include <grub/i386/relocator.h>
#include <grub/i386/cpuid.h>
#include <grub/i386/memory.h>

#include <grub/emu/bhyve.h>

SLIST_HEAD(grub_rlc_head, grub_relocator_chunk);

/* dummy struct for now */
struct grub_relocator
{
  struct grub_rlc_head head;
};

struct grub_relocator_chunk
{
  SLIST_ENTRY(grub_relocator_chunk) next;
  grub_phys_addr_t target;
  grub_size_t size;
};

/* Resove ext-ref. Longmode is always 1 with bhyve */
unsigned char grub_cpuid_has_longmode = 1;

static const struct grub_bhyve_info *binfo;

/*
 * Functions used by o/s loaders
 */
void *
get_virtual_current_address (grub_relocator_chunk_t in)
{
  return grub_emu_bhyve_virt((grub_uint64_t)in->target);
}

grub_phys_addr_t
get_physical_target_address (grub_relocator_chunk_t in)
{
  return in->target;
}

struct grub_relocator *
grub_relocator_new (void)
{
  struct grub_relocator *ret;

  ret = grub_zalloc (sizeof (struct grub_relocator));
  if (!ret)
    return NULL;
  
  SLIST_INIT(&ret->head);
  
  return ret;
}

/* Return true if [point,point+size) is disjoint from [otarget,otarget+osize) */
static int
grub_relocator_disjoint(grub_phys_addr_t point, grub_size_t size,
		       grub_phys_addr_t otarget, grub_size_t osize)
{
  if ((point >= (otarget + osize)) || ((point + size) < otarget))
    return 1;

  return 0;
}

/* Return true if point is within [target, target+size) */
static int
grub_relocator_within(grub_phys_addr_t point, grub_phys_addr_t target,
                      grub_size_t size)
{
  if (point >= target && point < (target + size))
    return 1;

  return 0;
}

static grub_phys_addr_t
grub_relocator_end(struct grub_relocator *rel)
{
  struct grub_relocator_chunk *cp, *cp_last;
  
  cp_last = NULL;
  SLIST_FOREACH(cp, &rel->head, next) {
    cp_last = cp;
  }

  return (cp_last ? cp_last->target + cp_last->size : 0);
}

grub_err_t
grub_relocator_alloc_chunk_addr (struct grub_relocator *rel,
                                 grub_relocator_chunk_t *out,
                                 grub_phys_addr_t target, grub_size_t size)
{
  struct grub_relocator_chunk *cp, *ncp, *prev;
  grub_phys_addr_t end, ptarget;
  grub_size_t psize;
  grub_err_t err;
  int i;

  end = target + size - 1;
  *out = NULL;

  /*
   * Make sure there are no existing allocations that this request
   * overlaps with
   */
  SLIST_FOREACH(cp, &rel->head, next) {
    if (!grub_relocator_disjoint(target, size, cp->target, cp->size))
      {
	err = GRUB_ERR_BAD_ARGUMENT;
	goto done;
      }
  }

  /*
   * See if the allocation fits within physical segments
   */
  for (i = 0; i < binfo->nsegs; i++) {
    ptarget = binfo->segs[i].start;
    psize = binfo->segs[i].end - ptarget + 1;
    if (grub_relocator_within(target, ptarget, psize) &&
	grub_relocator_within(end, ptarget, psize))
      break;
  }

  if (i == binfo->nsegs) {
    err = GRUB_ERR_OUT_OF_RANGE;
    goto done;
  }

  /*
   * Located a memory segment: allocate a chunk and insert it into
   * the list
   */
  ncp = grub_zalloc (sizeof (struct grub_relocator_chunk));
  if (!ncp) {
    err = GRUB_ERR_OUT_OF_MEMORY;
    goto done;
  }
  
  ncp->target = target;
  ncp->size = size;

  /*
   * Insert at the head if the list is empty or the first element is
   * at a higher address
   */
  if (SLIST_EMPTY(&rel->head) || (SLIST_FIRST(&rel->head))->target > target) {
    SLIST_INSERT_HEAD(&rel->head, ncp, next);
  } else {
    /*
     * At least one element in the list that is less than target, so prev
     * is guaranteed to exist for the list insertion.
     */
    SLIST_FOREACH(cp, &rel->head, next) {
      if (cp->target > target) {
	break;
      }
      prev = cp;
    }
    SLIST_INSERT_AFTER(prev, ncp, next);
  }
  
  *out = ncp;
  err = 0;

 done:
  return err;
}

grub_err_t
grub_relocator_alloc_chunk_align (struct grub_relocator *rel,
                                  grub_relocator_chunk_t *out,
                                  grub_phys_addr_t min_addr,
                                  grub_phys_addr_t max_addr,
                                  grub_size_t size, grub_size_t align,
                                  int preference __attribute__ ((unused)),
                                  int avoid_efi_boot_services __attribute__ ((unused)))
{
  grub_err_t err;
  grub_uint64_t addr;

  /*
   * Filter/modify allocations that specify a min address <= 1MB.
   * This is really a no-go area on x86, but loader code is often
   * machine-independent (e.g. multiboot) and has no concept of
   * this, resulting in requests for ranges starting at 0.
   *
   * If the size/alignment will fit into 1MB<->max_addr, change
   * the start to 1MB.
   */
#define ONE_MB	(1024*1024)
  if (min_addr < ONE_MB) {
    if ((align + size + ONE_MB) < max_addr)
      min_addr = ONE_MB;
  }

  /*
   * Extremely simplistic - run through the given address space and
   * attempt allocations at the specified alignment until successful,
   * or no allocations could be found
   * XXX only deal with preferences of LOW and NONE
   */
  addr = min_addr;
  for (addr = ALIGN_UP(addr, align); addr <= max_addr; addr += align) {
    err = grub_relocator_alloc_chunk_addr(rel, out, addr, size);
    if (err == 0)
      break;
  }

  return err;
}

void
grub_relocator_unload (struct grub_relocator *rel)
{
  struct grub_relocator_chunk *cp;

  if (!rel)
    return;

  while (!SLIST_EMPTY(&rel->head)) {
    cp = SLIST_FIRST(&rel->head);
    SLIST_REMOVE_HEAD(&rel->head, next);
    grub_free(cp);
  }

  grub_free (rel);
}

grub_err_t
grub_mmap_iterate (grub_memory_hook_t hook)
{
  int i;

  for (i = 0; i < binfo->nsegs; i++) {
      (*hook)(binfo->segs[i].start,
	      binfo->segs[i].end - binfo->segs[i].start,
	      binfo->segs[i].type);
  }

  return GRUB_ERR_NONE;
}

/*
 * Boot handoff
 */
grub_err_t
grub_relocator32_boot (struct grub_relocator *rel,
		       struct grub_relocator32_state state,
		       int avoid_efi_bootservices __attribute__ ((unused)))
{
  grub_relocator_chunk_t ch;
  grub_err_t err;

  /*
   * Attempt to allocate guest low memory (0x0200<->0xF000) for the boot state 
   * (GDT etc). Use 8-byte alignment as spec'd in the SDM 3A, 3.5.1.
   */
  err = grub_relocator_alloc_chunk_align (rel, &ch,
					  0x0200, 0xF000,
					  binfo->bootsz, 8,
					  GRUB_RELOCATOR_PREFERENCE_NONE,
					  0);

  /*
   * It's possible that all of that mem has been allocated. In that
   * case, go to the end of allocated memory and try there
   */
  if (err) {
    grub_phys_addr_t target;

    target = ALIGN_UP(grub_relocator_end(rel), 8);
    err = grub_relocator_alloc_chunk_addr (rel, &ch, target, binfo->bootsz);
  }
  
  if (err == GRUB_ERR_NONE)
    grub_emu_bhyve_boot32(get_physical_target_address (ch), state);

  return err;
}

grub_err_t
grub_relocator64_boot (struct grub_relocator *rel __attribute__ ((unused)),
		       struct grub_relocator64_state state __attribute ((unused)),
		       grub_addr_t min_addr __attribute__ ((unused)),
		       grub_addr_t max_addr __attribute__ ((unused)))
{

  grub_emu_bhyve_boot64(state);

  return GRUB_ERR_NONE;
}

/*
 * Stubs to satisfy BSD module references
 */
int
grub_aout_get_type(union grub_aout_header *header __attribute__ ((unused)))
{
  return AOUT_TYPE_NONE;
}

grub_err_t
grub_aout_load(grub_file_t file __attribute__ ((unused)),
	       int offset __attribute__ ((unused)),
	       void *load_addr __attribute__ ((unused)),
	       int load_size __attribute__ ((unused)),
	       grub_size_t bss_size __attribute__ ((unused)))
{
  return GRUB_ERR_NOT_IMPLEMENTED_YET;
}

grub_uint64_t
grub_mmap_get_upper (void)
{
  grub_uint64_t upper;
  int i;
  
  upper = 0;
  for (i = 0; i < binfo->nsegs; i++) {
    if (binfo->segs[i].start <= 0x100000 &&
	binfo->segs[i].end > 0x100000)
      upper = binfo->segs[i].end - 0x100000;
  }

  return (upper);
}

grub_uint64_t
grub_mmap_get_lower (void)
{
  /* Always 1MB available in bhyve */
  return 0x100000;
}

/*
 * For bhyve, assume that serial port 0 always exists
 */
grub_port_t
grub_ns8250_hw_get_port (const unsigned int unit)
{
  if (unit == 0)
    return 0x3f8;
  else
    return 0;
}


/*
 * Module loader glue
 */
GRUB_MOD_INIT (bhyve_relocator)
{
  binfo = grub_emu_bhyve_info();
  return;
}

GRUB_MOD_FINI (bhyve_relocator)
{
  return;
}
