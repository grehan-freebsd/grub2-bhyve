/*
 *  Bhyve host interface - VM allocation, mapping
 * guest memory, populating guest register state etc
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <assert.h>
#include <sys/types.h>
#include <sys/errno.h>

#include <x86/segments.h>
#include <x86/specialreg.h>
#include <machine/vmm.h>

#include <vmmapi.h>

#include <grub/err.h>
#include <grub/dl.h>
#include <grub/misc.h>
#include <grub/i386/memory.h>
#include <grub/emu/bhyve.h>

#define	GB	(1024*1024*1024ULL)

#define	DESC_UNUSABLE		0x00010000

#define GUEST_NULL_SEL		0
#define GUEST_CODE_SEL		2
#define GUEST_DATA_SEL		3
#define GUEST_TSS_SEL		4

#define	GUEST_GDTR_LIMIT	(5 * 8 - 1)

static uint16_t bhyve_gdt[] = {
  0x0000, 0x0000, 0x0000, 0x0000,       /* Null */
  0x0000, 0x0000, 0x0000, 0x0000,       /* Null #2 */
  0xffff, 0x0000, 0x9a00, 0x00cf,       /* code */
  0xffff, 0x0000, 0x9200, 0x00cf,       /* data */
  0x0000, 0x0000, 0x8900, 0x0080        /* tss */
};

static struct vmctx *bhyve_ctx;

static int bhyve_cinsert = 1;
static int bhyve_vgainsert = 1;

#define BHYVE_MAXSEGS	5
struct {
  grub_uint64_t lomem, himem;
  void *lomem_ptr, *himem_ptr;
} bhyve_g2h;

static struct grub_mmap_region bhyve_mm[BHYVE_MAXSEGS];
static struct grub_bhyve_info bhyve_info;

int
grub_emu_bhyve_init(const char *name, grub_uint64_t memsz)
{
  int err;
  int val;
  grub_uint64_t lomemsz;
#ifdef VMMAPI_VERSION
  int need_reinit = 0;
#endif

  err = vm_create (name);
  if (err != 0)
    {
      if (errno != EEXIST)
        {
          fprintf (stderr, "Could not create VM %s\n", name);
          return GRUB_ERR_ACCESS_DENIED;
        }
#ifdef VMMAPI_VERSION
        need_reinit = 1;
#endif
    }

  bhyve_ctx = vm_open (name);
  if (bhyve_ctx == NULL)
    {
      fprintf (stderr, "Could not open VM %s\n", name);
      return GRUB_ERR_BUG;
    }

#ifdef VMMAPI_VERSION
  if (need_reinit)
    {
      err = vm_reinit (bhyve_ctx);
      if (err != 0)
        {
          fprintf (stderr, "Could not reinit VM %s\n", name);
          return GRUB_ERR_BUG;
        }
    }
#endif

  val = 0;
  err = vm_get_capability (bhyve_ctx, 0, VM_CAP_UNRESTRICTED_GUEST, &val);
  if (err != 0)
    {
      fprintf (stderr, "VM unrestricted guest capability required\n");
      return GRUB_ERR_BAD_DEVICE;
    }

  err = vm_set_capability (bhyve_ctx, 0, VM_CAP_UNRESTRICTED_GUEST, 1);
  if (err != 0)
    {
      fprintf (stderr, "Could not enable unrestricted guest for VM\n");
      return GRUB_ERR_BUG;
    }

  err = vm_setup_memory (bhyve_ctx, memsz, VM_MMAP_ALL);
  if (err) {
    fprintf (stderr, "Could not setup memory for VM\n");
    return GRUB_ERR_OUT_OF_MEMORY;
  }

  lomemsz = vm_get_lowmem_limit(bhyve_ctx);

  /*
   * Extract the virtual address of the mapped guest memory.
   */
  if (memsz >= lomemsz) {
    bhyve_g2h.lomem = lomemsz;
    bhyve_g2h.himem = memsz - lomemsz;
    bhyve_g2h.himem_ptr = vm_map_gpa(bhyve_ctx, 4*GB, bhyve_g2h.himem);
  } else {
    bhyve_g2h.lomem = memsz;
    bhyve_g2h.himem = 0;    
  }
  bhyve_g2h.lomem_ptr = vm_map_gpa(bhyve_ctx, 0, bhyve_g2h.lomem);

  /*
   * bhyve is going to return the following memory segments
   *
   * 0 - 640K    - usable
   * 640K- 1MB   - vga hole, BIOS, not usable.
   * 1MB - lomem - usable
   * lomem - 4G  - not usable
   * 4G - himem  - usable [optional if himem != 0]
   */
  bhyve_info.nsegs = 2;
  bhyve_info.segs = bhyve_mm;

  bhyve_mm[0].start = 0x0;
  bhyve_mm[0].end = 640*1024 - 1;		/* 640K */
  bhyve_mm[0].type = GRUB_MEMORY_AVAILABLE;

  bhyve_mm[1].start = 1024*1024;
  bhyve_mm[1].end = (memsz > lomemsz) ? lomemsz : memsz;
  bhyve_mm[1].type = GRUB_MEMORY_AVAILABLE;

  if (memsz > lomemsz) {
    bhyve_info.nsegs++;
    bhyve_mm[2].start = 4*GB;
    bhyve_mm[2].end = (memsz - lomemsz) + bhyve_mm[2].start;
    bhyve_mm[2].type = GRUB_MEMORY_AVAILABLE;
  }

  /* The boot-code size is just the GDT that needs to be copied */
  bhyve_info.bootsz = sizeof(bhyve_gdt);

  return 0;
}

/*
 * 32-bit boot state initialization. The Linux sequence appears to
 * work fine for Net/OpenBSD kernel entry. Use the GP register state
 * passed in, and copy other info to the allocated phys address, bt.
 */
void
grub_emu_bhyve_boot32(grub_uint32_t bt, struct grub_relocator32_state rs)
{
  uint64_t cr0, cr4, rflags, desc_base;
  uint32_t desc_access, desc_limit;
  uint16_t gsel;

  /*
   * "At entry, the CPU must be in 32-bit protected mode with paging
   * disabled;"
   */
  cr0 = CR0_PE;
  assert(vm_set_register(bhyve_ctx, 0, VM_REG_GUEST_CR0, cr0) == 0);

  cr4 = 0;
  assert(vm_set_register(bhyve_ctx, 0, VM_REG_GUEST_CR4, cr4) == 0);

  /*
   * Reserved bit 1 set to 1. "interrupt must be disabled"
   */
  rflags = 0x2;
  assert(vm_set_register(bhyve_ctx, 0, VM_REG_GUEST_RFLAGS, rflags) == 0);

  /*
   * "__BOOT_CS(0x10) and __BOOT_DS(0x18); both descriptors must be 4G
   * flat segment; __BOOS_CS must have execute/read permission, and
   * __BOOT_DS must have read/write permission; CS must be __BOOT_CS"
   */
  desc_base = 0;
  desc_limit = 0xffffffff;
  desc_access = 0x0000C09B;
  assert(vm_set_desc(bhyve_ctx, 0, VM_REG_GUEST_CS,
		     desc_base, desc_limit, desc_access) == 0);

  desc_access = 0x0000C093;
  assert(vm_set_desc(bhyve_ctx, 0, VM_REG_GUEST_DS,
		     desc_base, desc_limit, desc_access) == 0);

  /*
   * ... "and DS, ES, SS must be __BOOT_DS;"
   */
  assert(vm_set_desc(bhyve_ctx, 0, VM_REG_GUEST_ES,
		     desc_base, desc_limit, desc_access) == 0);
  assert(vm_set_desc(bhyve_ctx, 0, VM_REG_GUEST_FS,
		     desc_base, desc_limit, desc_access) == 0);
  assert(vm_set_desc(bhyve_ctx, 0, VM_REG_GUEST_GS,
		     desc_base, desc_limit, desc_access) == 0);
  assert(vm_set_desc(bhyve_ctx, 0, VM_REG_GUEST_SS,
		     desc_base, desc_limit, desc_access) == 0);

  /*
   * XXX TR is pointing to null selector even though we set the
   * TSS segment to be usable with a base address and limit of 0.
   * Has to be 8b or vmenter will fail
   */
  desc_access = 0x0000008b;
  assert(vm_set_desc(bhyve_ctx, 0, VM_REG_GUEST_TR, 0x1000, 0x67,
		     desc_access) == 0);

  assert(vm_set_desc(bhyve_ctx, 0, VM_REG_GUEST_LDTR, 0, 0xffff,
		     DESC_UNUSABLE | 0x82) == 0);

  gsel = GSEL(GUEST_CODE_SEL, SEL_KPL);
  assert(vm_set_register(bhyve_ctx, 0, VM_REG_GUEST_CS, gsel) == 0);

  gsel = GSEL(GUEST_DATA_SEL, SEL_KPL);
  assert(vm_set_register(bhyve_ctx, 0, VM_REG_GUEST_DS, gsel) == 0);
  assert(vm_set_register(bhyve_ctx, 0, VM_REG_GUEST_ES, gsel) == 0);
  assert(vm_set_register(bhyve_ctx, 0, VM_REG_GUEST_FS, gsel) == 0);
  assert(vm_set_register(bhyve_ctx, 0, VM_REG_GUEST_GS, gsel) == 0);
  assert(vm_set_register(bhyve_ctx, 0, VM_REG_GUEST_SS, gsel) == 0);

  /* XXX TR is pointing to selector 1 */
  gsel = GSEL(GUEST_TSS_SEL, SEL_KPL);
  assert(vm_set_register(bhyve_ctx, 0, VM_REG_GUEST_TR, gsel) == 0);

  /* LDTR is pointing to the null selector */
  assert(vm_set_register(bhyve_ctx, 0, VM_REG_GUEST_LDTR, 0) == 0);

  /*
   * "In 32-bit boot protocol, the kernel is started by jumping to the
   * 32-bit kernel entry point, which is the start address of loaded
   * 32/64-bit kernel."
   */
  assert(vm_set_register(bhyve_ctx, 0, VM_REG_GUEST_RIP, rs.eip) == 0);

  /*
   * Set up the GDT by copying it into low memory, and then pointing
   * the guest's GDT descriptor at it
   */
  memcpy(grub_emu_bhyve_virt(bt), bhyve_gdt, sizeof(bhyve_gdt));
  desc_base = bt;
  desc_limit = GUEST_GDTR_LIMIT;
  assert(vm_set_desc(bhyve_ctx, 0, VM_REG_GUEST_GDTR, desc_base,
    desc_limit, 0) == 0);;
  
  /*
   * Set the stack to be just below the params real-mode area
   */
  assert(vm_set_register(bhyve_ctx, 0, VM_REG_GUEST_RSP, rs.esp) == 0);

  /*
   * "%esi must hold the base address of the struct boot_params"
   */
  assert(vm_set_register(bhyve_ctx, 0, VM_REG_GUEST_RSI, rs.esi) == 0);

  /*
   * "%ebp, %edi and %ebx must be zero."
   * Assume that grub set these up correctly - might be different for
   * *BSD. While at it, init the remaining passed-in register state.
   */
  assert(vm_set_register(bhyve_ctx, 0, VM_REG_GUEST_RBP, rs.ebp) == 0);
  assert(vm_set_register(bhyve_ctx, 0, VM_REG_GUEST_RDI, rs.edi) == 0);
  assert(vm_set_register(bhyve_ctx, 0, VM_REG_GUEST_RBX, rs.ebx) == 0);

  assert(vm_set_register(bhyve_ctx, 0, VM_REG_GUEST_RAX, rs.eax) == 0);
  assert(vm_set_register(bhyve_ctx, 0, VM_REG_GUEST_RCX, rs.ecx) == 0);
  assert(vm_set_register(bhyve_ctx, 0, VM_REG_GUEST_RDX, rs.edx) == 0);

  /*
   * XXX debug: turn on tracing
   */
#if 0
  assert(vm_set_capability(bhyve_ctx, 0, VM_CAP_MTRAP_EXIT, 1) == 0);
#endif

  /*
   * Exit cleanly, using the conditional test to avoid the noreturn
   * warning.
   */
  if (bt)
    grub_reboot();
}

/*
 * 64-bit boot state initilization. This is really only used for FreeBSD.
 * It is assumed that the repeating 1GB page tables have already been
 * setup. The bhyve library call does almost everything - remaining
 * GP register state is set here
 */
void
grub_emu_bhyve_boot64(struct grub_relocator64_state rs)
{
  uint64_t gdt64[3];
  uint64_t gdt64_addr;
  int error;

  /*
   * Set up the GDT by copying it to just below the top of low memory
   * and point the guest's GDT descriptor at it
   */
  gdt64_addr = bhyve_g2h.lomem - 2 * sizeof(gdt64);
  vm_setup_freebsd_gdt(gdt64);
  memcpy(grub_emu_bhyve_virt(gdt64_addr), gdt64, sizeof(gdt64));

  /*
   * Use the library API to set up a FreeBSD entry reg state
   */
  error = vm_setup_freebsd_registers(bhyve_ctx, 0, rs.rip, rs.cr3, 
				     gdt64_addr, rs.rsp);
  assert(error == 0);

  /* Set up the remaining regs */
  assert(vm_set_register(bhyve_ctx, 0, VM_REG_GUEST_RAX, rs.rax) == 0);
  assert(vm_set_register(bhyve_ctx, 0, VM_REG_GUEST_RBX, rs.rbx) == 0);
  assert(vm_set_register(bhyve_ctx, 0, VM_REG_GUEST_RCX, rs.rcx) == 0);
  assert(vm_set_register(bhyve_ctx, 0, VM_REG_GUEST_RDX, rs.rdx) == 0);
  assert(vm_set_register(bhyve_ctx, 0, VM_REG_GUEST_RSI, rs.rsi) == 0);

  /*
   * Exit cleanly, using the conditional test to avoid the noreturn
   * warning.
   */
  if (gdt64_addr)
    grub_reboot();
}

const struct grub_bhyve_info *
grub_emu_bhyve_info(void)
{
  return &bhyve_info;
}

void *
grub_emu_bhyve_virt(grub_uint64_t physaddr)
{
  void *virt;

  virt = NULL;

  if (physaddr < bhyve_g2h.lomem)
    virt = (char *)bhyve_g2h.lomem_ptr + physaddr;
  else if (physaddr >= 4*GB && physaddr < (4*GB + bhyve_g2h.himem))
    virt = (char *)bhyve_g2h.himem_ptr + (physaddr - 4*GB);

  return (virt);
}

int
grub_emu_bhyve_parse_memsize(const char *arg, grub_uint64_t *size)
{
  /*
   * Assume size_t == uint64_t. Safe for amd64, which is the
   * only platform grub-bhyve will ever run on.
   */
  return (vm_parse_memsize(arg, size));
}

void
grub_emu_bhyve_unset_cinsert(void)
{
  bhyve_cinsert = 0;
}

int
grub_emu_bhyve_cinsert(void)
{
  return bhyve_cinsert;
}

void
grub_emu_bhyve_unset_vgainsert(void)
{
  bhyve_vgainsert = 0;
}

int
grub_emu_bhyve_vgainsert(void)
{
  return bhyve_vgainsert;
}
