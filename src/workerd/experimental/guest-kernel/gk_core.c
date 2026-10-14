// gk: gk_init, the run entry points, arena-root syncing, the host-frame wall and
// the diagnostic interface.
#include "gk_internal.h"

// ---- the runtime image ------------------------------------------------------
// The set of ranges gk_mapping_change_ok refuses on behalf of the runtime
// itself (see the mapping-change policy section): the loaded segments (text,
// rodata, data and bss, from the program headers) of the main executable and
// of every shared object the dynamic loader has mapped when gk_init runs
// (libc, ld.so, the vDSO), and the main thread's stack. Recorded once here, in
// the supervisor control block, so the syscall path consults a small array
// rather than re-reading /proc/self/maps, and so the set cannot be edited
// from ring 3. An object loaded later (dlopen) is not in the set.
static void image_compact(void);

static int image_add(uintptr_t s, uintptr_t e) {
  if (G->image_n >= GK_MAX_IMAGE_RANGES) {
    // Full of segments, which is not the same as full of distinct ranges: the
    // loader maps an object's PT_LOADs next to each other, so most of them
    // merge. Compact and look again, once -- if that frees nothing the ranges
    // really are disjoint and the table is too small for this address space.
    int before = G->image_n;
    image_compact();
    if (G->image_n >= before) { G->image_full = 1; return -1; }
  }
  G->image[G->image_n++] = (gk_span){HOST_FLOOR(s), HOST_CEIL(e)};  // the loader maps host pages
  return 0;
}

// What the walk saw, for the report: how the counts relate says whether a
// binary is near the table's bound and why.
typedef struct { int objects, segments; } gk_image_walk;

// One loaded object: its PT_LOAD segments, with p_memsz (which extends past
// the file for bss), join the image. The walk stops once the table is full,
// which image_add has recorded in G->image_full.
static int image_phdr_cb(struct dl_phdr_info *info, size_t size, void *data) {
  (void)size;
  gk_image_walk *w = data;
  w->objects++;
  for (int i = 0; i < info->dlpi_phnum; i++) {
    const ElfW(Phdr) *p = &info->dlpi_phdr[i];
    if (p->p_type != PT_LOAD || p->p_memsz == 0) continue;
    uintptr_t s = info->dlpi_addr + p->p_vaddr;
    w->segments++;
    if (image_add(s, s + p->p_memsz) < 0) return 1;
  }
  return 0;
}

// The main thread's stack is the kernel's [stack] mapping, which grows down on
// demand: the recorded range reaches below the mapping's current start by the
// room the kernel will grant it, RLIMIT_STACK (1GiB when that is unlimited,
// in which case the kernel's legacy mmap layout keeps everything else far
// away). Nothing but the stack is ever placed in that room by the kernel.
static int image_record_stack(void) {
  FILE *f = fopen("/proc/self/maps", "r");
  if (!f) return -1;
  char *line = NULL;
  size_t cap = 0;
  int found = 0;
  uintptr_t ss = 0, se = 0;
  while (getline(&line, &cap, f) >= 0) {
    size_t n = strlen(line);
    const char *tag = "[stack]\n";
    size_t tl = strlen(tag);
    if (n < tl || memcmp(line + n - tl, tag, tl) != 0) continue;
    int perms;
    if (parse_maps_line(line, line + n, &ss, &se, &perms)) found = 1;
    break;
  }
  free(line);
  fclose(f);
  if (!found) return -1;
  struct rlimit rl;
  uintptr_t room = 1UL << 30;
  if (getrlimit(RLIMIT_STACK, &rl) == 0 && rl.rlim_cur != RLIM_INFINITY && rl.rlim_cur < room)
    room = (uintptr_t)rl.rlim_cur;
  uintptr_t lo = se > room ? se - room : 0;
  if (lo > ss) lo = ss;
  return image_add(lo, se);
}

// Sorts the spans by start and merges those that overlap or touch. The result
// covers exactly the same addresses -- only spans whose rounded ranges meet are
// merged, so no address joins the set that was not already in it -- and it buys
// two things: range_clear_locked can binary-search a sorted disjoint array
// instead of scanning, and an object's page-adjacent PT_LOAD segments cost one
// entry rather than four. Insertion sort: this runs once, at init, over a few
// hundred entries, and allocates nothing.
static void image_compact(void) {
  if (G->image_n <= 0) return;
  for (int i = 1; i < G->image_n; i++) {
    gk_span v = G->image[i];
    int j = i - 1;
    while (j >= 0 && G->image[j].start > v.start) { G->image[j + 1] = G->image[j]; j--; }
    G->image[j + 1] = v;
  }
  int w = 0;
  for (int i = 1; i < G->image_n; i++) {
    if (G->image[i].start <= G->image[w].end) {
      if (G->image[i].end > G->image[w].end) G->image[w].end = G->image[i].end;
    } else {
      G->image[++w] = G->image[i];
    }
  }
  G->image_n = w + 1;
}

// Records the image, or fails with a reason the caller can act on. Every
// failure here leaves gk unstarted, which is safe -- nothing is running in a
// guest yet -- so none of them is gk_fatal; what each one must not be is
// indistinguishable from "this machine has no /dev/kvm", which is the other
// reason gk_init fails and the only one a caller may answer by carrying on
// without gk. An overflow in particular is reported on stderr as well, because
// it is a property of how the binary was linked rather than of the machine,
// and because the alternative to failing is running gk_mapping_change_ok
// against an incomplete picture of what a guest munmap may remove.
static int image_record(void) {
  gk_image_walk walk = {0, 0};
  dl_iterate_phdr(image_phdr_cb, &walk);
  if (!G->image_full) {
    if (G->image_n == 0) {
      G->err = "the runtime image has no loaded object";
      return -1;
    }
    // A failure here is the stack's own unless the add overflowed, which the
    // report below is for.
    if (image_record_stack() < 0 && !G->image_full) {
      G->err = "the runtime image has no main-thread stack";
      return -1;
    }
  }
  if (G->image_full) {
    fprintf(stderr,
            "[gk] the runtime image does not fit in %d ranges (%d objects, %d segments so "
            "far); gk will not start\n",
            GK_MAX_IMAGE_RANGES, walk.objects, walk.segments);
    G->err = "the runtime image does not fit in the image table";
    return -1;
  }
  image_compact();
  if (G->dbg)
    fprintf(stderr, "[gk] runtime image: %d objects, %d segments, %d ranges\n", walk.objects,
            walk.segments, G->image_n);
  return 0;
}

#if defined(__x86_64__)
// XCR0 state components, in the groups XSETBV accepts: x87 is mandatory, SSE
// is required for AVX, the three AVX-512 components go together and require
// AVX, the two AMX components go together. KVM_SET_XCRS applies the same rules
// and refuses a value the vCPU's CPUID does not support.
#define XCR0_X87 (1ULL << 0)
#define XCR0_SSE (1ULL << 1)
#define XCR0_AVX (1ULL << 2)
#define XCR0_AVX512 ((1ULL << 5) | (1ULL << 6) | (1ULL << 7))  // opmask, ZMM_Hi256, Hi16_ZMM
#define XCR0_PKRU (1ULL << 9)
#define XCR0_AMX ((1ULL << 17) | (1ULL << 18))                // XTILECFG, XTILEDATA

// Pick the XCR0 every vCPU is given: the state components that the CPUID the
// vCPUs get advertises (leaf 0xD subleaf 0, EDX:EAX, which KVM has already
// cut down to what the host enables and lets a guest use), limited to the
// groups above. The guest runs the host's own binaries, whose ifunc resolvers
// and JITs choose a memcpy, strlen or code path from that CPUID: a component
// the CPUID advertises but XCR0 leaves off makes the first such instruction
// #UD (an AVX-512 memcpy inside the guest, say), and a component the CPUID
// does not advertise is never selected. XCR0 therefore follows the CPUID,
// which also keeps code inside the guest on the same instruction set as the
// same code outside it. gk holds no XSAVE area of its own -- KVM sizes the
// vCPU's from the components the CPUID permits, and glibc's lazy-binding
// trampoline sized its save slot from the host's XCR0 at process start, of
// which this value is a subset -- so a wider XCR0 costs nothing else. A value
// equal to the host's XCR0 spares KVM an XSETBV pair per VM exit.
static void xcr0_select(void) {
  uint64_t have = 0;
  for (unsigned i = 0; i < GK_CPUID->nent; i++) {
    const struct kvm_cpuid_entry2 *e = &GK_CPUID->entries[i];
    if (e->function == 0xd && e->index == 0) {
      have = ((uint64_t)e->edx << 32) | e->eax;
      break;
    }
  }
  uint64_t x = XCR0_X87;
  if (have & XCR0_SSE) x |= XCR0_SSE;
  if ((x & XCR0_SSE) && (have & XCR0_AVX)) x |= XCR0_AVX;
  if ((x & XCR0_AVX) && (have & XCR0_AVX512) == XCR0_AVX512) x |= XCR0_AVX512;
  if (have & XCR0_PKRU) x |= XCR0_PKRU;
  if ((have & XCR0_AMX) == XCR0_AMX) x |= XCR0_AMX;
  G->xcr0 = x;
  if (G->dbg)
    fprintf(stderr, "[gk] XCR0 %#llx (CPUID leaf 0xD offers %#llx)\n",
            (unsigned long long)x, (unsigned long long)have);
}
#endif

int gk_init(void) {
  // The control block first: it is registered before anything is mapped, so
  // no root ever holds a user PTE for it (see the control-data note). Its
  // non-zero initial values are set here rather than statically, which would
  // move the whole multi-megabyte block from .bss into the file.
  pthread_mutex_init(&G->lock, NULL);
  G->root_gen = 1;
  G->pml4_gen = 1;
  G->rand_state = 0x9e3779b9u;
  prot_add(GK_CTL_START, GK_CTL_END, GK_PROT_SUPER);
  // The resolve table too: ring 0 reads it inside the guest, ring 3 must fault.
  prot_add(GK_RESOLVE_START, GK_RESOLVE_END, GK_PROT_SUPER);
  G->dbg = getenv("GK_DEBUG") != NULL;
  G->thp = getenv("GK_NO_THP") == NULL;
  // vCPU runners (see the runner section). The pool of records is host memory
  // the guest never needs: refused, like the gk-loop side stacks.
  {
    const char *v = getenv("GK_RUNNER");
    G->runner_mode = v && *v && strcmp(v, "0") != 0;
    G->resident_mode = v && strcmp(v, "resident") == 0;
#if GK_HVF
    // An HVF vCPU can only be run by the thread that created it (see
    // vcpu_park), so no other thread can run it on the owner's behalf.
    if (G->runner_mode) {
      fprintf(stderr, "[gk] GK_RUNNER: vCPU runners need KVM; ignored\n");
      G->runner_mode = G->resident_mode = 0;
    }
#endif
#if !defined(__x86_64__)
    // The park loop is x86-64 code (gk_park_loop in gk_asm.S, kicked by an
    // NMI): elsewhere a runner returns to userspace after every turn.
    if (G->resident_mode) {
      fprintf(stderr, "[gk] GK_RUNNER=resident: resident vCPUs are x86-64 only; runners will "
                      "return to userspace after every turn\n");
      G->resident_mode = 0;
    }
#endif
    v = getenv("GK_RUNNER_SPIN");
    G->runner_spin = v ? atol(v) : 0;
    v = getenv("GK_RUNNER_PIN");
    G->runner_pin = GK_RUNNER_PIN_NONE;
    if (v && strcmp(v, "sibling") == 0) G->runner_pin = GK_RUNNER_PIN_SIBLING;
    else if (v && strcmp(v, "other") == 0) G->runner_pin = GK_RUNNER_PIN_OTHER;
    else if (v && *v >= '0' && *v <= '9') G->runner_pin = GK_RUNNER_PIN_CPU + atoi(v);
    if (G->runner_mode) {
      size_t bytes = (sizeof(gk_runner) * GK_MAX_VCPUS + 0xfff) & ~(size_t)0xfff;
      void *pool = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
      if (pool == MAP_FAILED) { G->err = "mmap runner pool"; return -1; }
      prot_add((uintptr_t)pool, (uintptr_t)pool + bytes, GK_PROT_REFUSE);
      G->runner_pool = pool;
    }
  }
#if defined(__x86_64__)
  *(void **)&G->tls_static_info = dlsym(RTLD_DEFAULT, "_dl_get_tls_static_info");
  if (!G->tls_static_info)
    fprintf(stderr, "[gk] _dl_get_tls_static_info not found: gk_run_here_user builds no "
                    "host-frame wall on pthread stacks\n");
#elif !GK_HVF
  // glibc publishes sizeof(struct pthread) for libthread_db (GLIBC_PRIVATE).
  const uint32_t *psz = dlsym(RTLD_DEFAULT, "_thread_db_sizeof_pthread");
  G->pthread_size = psz ? *psz : 0;
  if (!G->pthread_size)
    fprintf(stderr, "[gk] _thread_db_sizeof_pthread not found: gk_run_here_user builds no "
                    "host-frame wall on pthread stacks\n");
#endif
#if defined(__x86_64__)
  if (sysconf(_SC_PAGESIZE) != 4096) { G->err = "host page size is not 4KiB"; return -1; }
#else
  // Any power of two the windows are multiples of (see the page-sizes section).
  long pagesz = sysconf(_SC_PAGESIZE);
  if (pagesz < 4096 || (pagesz & (pagesz - 1)) || GK_NEIGHBOR_WIN % (unsigned long)pagesz) {
    G->err = "unsupported host page size";
    return -1;
  }
  G->host_page = (size_t)pagesz;
#endif
#if GK_HVF
  // The VM, with the largest guest-physical space the framework offers (see
  // the guest-physical allocator), and the shadow pool (see the HVF section).
  hv_vm_config_t cfg = hv_vm_config_create();
  uint32_t ipa = 36;
  if (!cfg || hv_vm_config_get_max_ipa_size(&ipa) != HV_SUCCESS ||
      hv_vm_config_set_ipa_size(cfg, ipa) != HV_SUCCESS || hv_vm_create(cfg) != HV_SUCCESS) {
    G->err = "hv_vm_create (is the binary signed with the hypervisor entitlement?)";
    return -1;
  }
  os_release(cfg);
  static const int ips_bits[] = {32, 36, 40, 42, 44, 48};
  for (G->ips = 0; G->ips < 5 && ips_bits[G->ips + 1] <= (int)ipa; G->ips++) {}
  ipa = (uint32_t)ips_bits[G->ips];
  G->gpa_next = GK_BACK_WIN;  // guest-physical 0 stays unused
  G->gpa_limit = 1ULL << ipa;
  void *pool = mmap(NULL, (size_t)GK_MAX_SHADOWS * GK_HOST_PAGE, PROT_NONE,
                    MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
  if (pool == MAP_FAILED) { G->err = "mmap shadow pool"; return -1; }
  G->shadow_pool = (uintptr_t)pool;
  prot_add(G->shadow_pool, G->shadow_pool + (size_t)GK_MAX_SHADOWS * GK_HOST_PAGE, GK_PROT_REFUSE);
  size_t map_bytes = HOST_CEIL(G->gpa_limit / GK_HOST_PAGE / 8);  // touched only where used
  void *map = mmap(NULL, map_bytes, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
  if (map == MAP_FAILED) { G->err = "mmap stage-2 map"; return -1; }
  G->s2_map = map;
  prot_add((uintptr_t)map, (uintptr_t)map + map_bytes, GK_PROT_REFUSE);
  if (G->dbg) fprintf(stderr, "[gk] Hypervisor.framework VM, %u-bit guest-physical space\n", ipa);
#else
  G->kvm = open("/dev/kvm", O_RDWR | O_CLOEXEC);
  if (G->kvm < 0) { G->err = "open /dev/kvm"; return -1; }
  if (ioctl(G->kvm, KVM_GET_API_VERSION, 0) != 12) { G->err = "KVM API != 12"; return -1; }
#if defined(__x86_64__)
  G->vmfd = ioctl(G->kvm, KVM_CREATE_VM, 0);
  if (G->vmfd < 0) { G->err = "KVM_CREATE_VM"; return -1; }
  // Resident vCPUs halt inside KVM_RUN and are woken by an NMI, both of
  // which need an in-kernel LAPIC: a split irqchip (LAPICs in the kernel,
  // no PIC or IOAPIC), enabled before any vCPU exists. Without it the
  // runners return to userspace after every turn as GK_RUNNER=1 does.
  if (G->resident_mode) {
    struct kvm_enable_cap cap = {.cap = KVM_CAP_SPLIT_IRQCHIP};
    cap.args[0] = 24;
    if (ioctl(G->vmfd, KVM_ENABLE_CAP, &cap) != 0) {
      fprintf(stderr, "[gk] GK_RUNNER=resident: KVM_CAP_SPLIT_IRQCHIP: %s; runners will return "
                      "to userspace after every turn\n", strerror(errno));
      G->resident_mode = 0;
    }
  }
#else
  // The largest guest-physical space KVM offers (see the guest-physical
  // allocator), and TCR_EL1.IPS to match.
  int ipa = ioctl(G->kvm, KVM_CHECK_EXTENSION, KVM_CAP_ARM_VM_IPA_SIZE);
  if (ipa <= 0) ipa = 40;
  if (ipa > 48) ipa = 48;
  static const int ips_bits[] = {32, 36, 40, 42, 44, 48};
  for (G->ips = 0; G->ips < 5 && ips_bits[G->ips + 1] <= ipa; G->ips++) {}
  ipa = ips_bits[G->ips];
  G->vmfd = ioctl(G->kvm, KVM_CREATE_VM, KVM_VM_TYPE_ARM_IPA_SIZE(ipa));
  if (G->vmfd < 0) { G->err = "KVM_CREATE_VM"; return -1; }
  G->gpa_next = GK_BACK_WIN;  // guest-physical 0 stays unused
  G->gpa_limit = 1ULL << ipa;
  // The vector handlers' HVCs come to the host as KVM_EXIT_HYPERCALL; without
  // the filter KVM would take them as (failed) PSCI calls itself.
  struct kvm_smccc_filter filter = {.base = GK_HC_BASE, .nr_functions = GK_HC_COUNT,
                                    .action = KVM_SMCCC_FILTER_FWD_TO_USER};
  struct kvm_device_attr fa = {.group = KVM_ARM_VM_SMCCC_CTRL, .attr = KVM_ARM_VM_SMCCC_FILTER,
                               .addr = (uint64_t)(uintptr_t)&filter};
  if (ioctl(G->vmfd, KVM_SET_DEVICE_ATTR, &fa) < 0) { G->err = "KVM_ARM_VM_SMCCC_FILTER"; return -1; }
  // The guest runs the host's vDSO, which reads the virtual counter: with a
  // zero offset it reads the host's, as the zero TSC offset does on x86-64.
  // Best effort: older kernels lack the ioctl, and the guest's clock is then
  // off by the VM's start time.
  struct kvm_arm_counter_offset co = {.counter_offset = 0};
  if (ioctl(G->vmfd, KVM_ARM_SET_COUNTER_OFFSET, &co) < 0)
    fprintf(stderr, "[gk] KVM_ARM_SET_COUNTER_OFFSET failed (%s): guest clocks are off\n",
            strerror(errno));
  if (ioctl(G->vmfd, KVM_ARM_PREFERRED_TARGET, &G->vcpu_target) < 0) {
    G->err = "KVM_ARM_PREFERRED_TARGET";
    return -1;
  }
#endif
#endif  // GK_HVF

  G->pt_bytes = 64 * 1024 * 1024;
  uint8_t *pt = mmap(NULL, G->pt_bytes, PROT_READ | PROT_WRITE,
                     MAP_SHARED | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
  if (pt == MAP_FAILED) { G->err = "mmap PT arena"; return -1; }
  G->pt_next = pt; G->pt_end = pt + G->pt_bytes; G->pt_base = (uintptr_t)pt;
  // The page-table arena is walked by the CPU through guest-physical addresses
  // (it has memslots), never through guest-virtual PTEs. Refuse it so ring-3
  // code cannot fault it in and rewrite the tables that enforce the arenas.
  prot_add(G->pt_base, G->pt_base + G->pt_bytes, GK_PROT_REFUSE);
#if defined(__aarch64__)
#if !GK_HVF
  vdso_setup();  // before the first region: its windows must avoid [vvar]
#endif
  // Table entries hold guest-physical addresses (PTE_TABLE_ENTRY), so the
  // page-table arena's must be known before the first table is linked in: it
  // is one region, and one guest-physical range.
  if (region_ensure(G->pt_base, G->pt_base + G->pt_bytes) < 0) { G->err = "memslot for PT arena"; return -1; }
  gk_region *ptr = region_find(G->pt_base);
  if (!ptr || ptr->end < G->pt_base + G->pt_bytes) { G->err = "PT arena memslot split"; return -1; }
  G->pt_gpa = ptr->gpa + (G->pt_base - ptr->start);
#endif
  G->pml4 = alloc_table();

#if defined(__x86_64__)
  uint8_t *tables = mmap(NULL, 0x2000, PROT_READ | PROT_WRITE,
                         MAP_SHARED | MAP_ANONYMOUS, -1, 0);
  if (tables == MAP_FAILED) { G->err = "mmap gdt/idt"; return -1; }
  G->gdt_va = (uintptr_t)tables;
  G->idt_va = (uintptr_t)tables + 0x1000;
  // The GDT and IDT are read by the CPU (a supervisor access) during exception
  // delivery and ring transitions; ring-3 code has no business touching them.
  prot_add(G->gdt_va, G->gdt_va + 0x2000, GK_PROT_SUPER);
  // GDT layout, in the order SYSCALL/SYSRET and the exception frames need:
  //   0x08 ring-0 code64   (SYSCALL loads CS from STAR[47:32] = 0x08)
  //   0x10 ring-0 data     (SYSCALL loads SS = 0x08 + 8)
  //   0x18 unused          (TR's nominal selector; TR uses a cached descriptor)
  //   0x20 ring-3 data     (SYSRET loads SS = STAR[63:48] + 8 = 0x1b + 8 = 0x23)
  //   0x28 ring-3 code64   (SYSRET loads CS = STAR[63:48] + 16 = 0x1b + 16 = 0x2b)
  // STAR[63:48] is 0x1b (RPL 3): SYSRET adds 8/16 and uses the RPL as-is, so
  // ring 3 runs with SS 0x23 and CS 0x2b. The CPU reloads both from the GDT on
  // the IRETQ that returns from a ring-3 fault, so the +8/+16 order, RPL and
  // DPL=3 must all be exact.
  uint64_t *gdt = (uint64_t *)(uintptr_t)G->gdt_va;
  gdt[0] = 0;
  gdt[1] = 0x00AF9A000000FFFFULL;  // ring-0 code64 (DPL 0, L=1)
  gdt[2] = 0x00CF92000000FFFFULL;  // ring-0 data   (DPL 0)
  gdt[3] = 0;
  gdt[4] = 0x00CFF2000000FFFFULL;  // ring-3 data   (DPL 3)
  gdt[5] = 0x00AFFA000000FFFFULL;  // ring-3 code64 (DPL 3, L=1)
  uint8_t *idt = (uint8_t *)(uintptr_t)G->idt_va;
  memset(idt, 0, 0x1000);
  set_idt_gate(idt, 0, (uintptr_t)&gk_exc_de, 1);
  // The NMI a halted resident vCPU is kicked with (see the runner section),
  // on its own stack: it can arrive while an exception handler is on IST1.
  set_idt_gate(idt, 2, (uintptr_t)&gk_exc_nmi, 2);
  set_idt_gate(idt, 6, (uintptr_t)&gk_exc_ud, 1);
  set_idt_gate(idt, 8, (uintptr_t)&gk_exc_df, 1);
  set_idt_gate(idt, 13, (uintptr_t)&gk_exc_gp, 1);
  set_idt_gate(idt, 14, (uintptr_t)&gk_exc_pf, 1);

  // The page-table arena is read by the CPU page walker via guest-physical
  // addresses, so it needs memslots (but no PTEs of its own).
  if (region_ensure(G->pt_base, G->pt_base + G->pt_bytes) < 0) { G->err = "memslot for PT arena"; return -1; }
  // GDT/IDT and the handler text must be present before the first fault.
  if (mmu_map_range(G->gdt_va, G->gdt_va + 0x2000, 1 | 2) < 0) { G->err = "map gdt/idt"; return -1; }
  if (map_handler_text() < 0) { G->err = "map handler text"; return -1; }
  // The resolve table is read by the trampoline at ring 0, on the caller's
  // (ring-3) stack; mapping it now, supervisor r/w, keeps that path free of
  // demand faults in every root.
  if (mmu_map_range(GK_RESOLVE_START, GK_RESOLVE_END, 1 | 2) < 0) { G->err = "map resolve table"; return -1; }

  GK_CPUID->nent = GK_CPUID_ENTRIES;
  if (ioctl(G->kvm, KVM_GET_SUPPORTED_CPUID, GK_CPUID) < 0) { G->err = "GET_SUPPORTED_CPUID"; return -1; }
  flush_stub_select();
  G->park_flush_kind = G->flush_stub == gk_flush_stub_invpcid ? 1 : 2;
  xcr0_select();
  G->run_size = ioctl(G->kvm, KVM_GET_VCPU_MMAP_SIZE, 0);
  // Register exchange through kvm_run (see regs_get) when KVM offers it for
  // both register sets; GK_NO_SYNC_REGS in the environment forces the ioctls.
  if (!getenv("GK_NO_SYNC_REGS")) {
    long caps = ioctl(G->kvm, KVM_CHECK_EXTENSION, KVM_CAP_SYNC_REGS);
    G->sync_regs = caps > 0 && (caps & GK_SYNC_REGS) == GK_SYNC_REGS;
  }
  if (G->dbg) fprintf(stderr, "[gk] registers via %s\n", G->sync_regs ? "kvm_run" : "ioctl");
#else
  if (map_handler_text() < 0) { G->err = "map handler text"; return -1; }
#if !GK_HVF
  G->run_size = ioctl(G->kvm, KVM_GET_VCPU_MMAP_SIZE, 0);
#endif
#endif
  if (pthread_key_create(&G->thread_key, thread_key_dtor) != 0) { G->err = "pthread_key_create"; return -1; }
  if (image_record() < 0) return -1;  // image_record says which failure it was
  // The hostcall exchange regions (see the hostcalls section): one
  // reservation, committed a region at a time as thread records are taken.
  // Reserved before the first record below needs its region.
  void *area = mmap(NULL, GK_HOSTCALL_AREA_BYTES, PROT_NONE,
                    MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
  if (area == MAP_FAILED) { G->err = "mmap hostcall regions"; return -1; }
  G->hostcall_area = (uintptr_t)area;

  // Bring up the calling thread's record and vCPU.
  gk_thread *t = thread_get();
  if (!t) { G->err = "thread record"; return -1; }
  int r = vcpu_init(t, GK_VCPU_STACK);
  if (r == 0) G->initialized = 1;
  return r;
}

int gk_vcpu_count(void) { return atomic_load(&G->vcpus_created); }

const char *gk_last_error(void) { return G->err; }
unsigned long gk_fault_addr(void) { return G->fault_addr; }
void gk_set_syscall_filter(gk_syscall_filter f) { G->filter = f; }

// Test/diagnostic hook: hand out one supervisor page (the GDT) and one refused
// page (the page-table arena), so a test can verify that ring-3 code faults on
// gk's own control memory while its own user pages work.
void gk_debug_control_addrs(unsigned long *super, unsigned long *refuse) {
#if defined(__x86_64__)
  if (super) *super = (unsigned long)G->gdt_va;
#else
  if (super) *super = (unsigned long)G->vbar;  // the vector table
#endif
  if (refuse) *refuse = (unsigned long)G->pt_base;
}

// Test/diagnostic hook: the address of one of gk's control structures (see
// gk.h for the indices), all of which live in the supervisor control block. A
// test writes there from ring 3 and expects a fault; from ring 0 the scratch
// word (index 0) can be written freely.
#ifdef GK_NEG_THREAD_WORDS
static __thread uint64_t neg_words[GK_THREAD_WORDS];  // tentative def; see the words functions
#endif
unsigned long gk_debug_ctl_addr(int which) {
  switch (which) {
    case GK_CTL_SCRATCH: return (unsigned long)&G->debug_scratch;
    case GK_CTL_ARENAS: return (unsigned long)&G->arena_n;
    case GK_CTL_ARENA_POOL: return (unsigned long)&G->arena_pool[0];
    case GK_CTL_PROT: return (unsigned long)&G->prot[0];
    case GK_CTL_REGIONS: return (unsigned long)&G->regions;
    case GK_CTL_REGION_POOL: return (unsigned long)&G->region_pool[0];
    case GK_CTL_PT_ALLOC: return (unsigned long)&G->pt_free;
    case GK_CTL_THREADS: return (unsigned long)&G->threads[0];
    case GK_CTL_VCPU_POOL: return (unsigned long)&G->parked[0];
    case GK_CTL_PKEYS: return (unsigned long)&G->pkeys[0];
    case GK_CTL_ROOT: return (unsigned long)&G->pml4;
    case GK_CTL_FILTER: return (unsigned long)&G->filter;
    case GK_CTL_HOST_SLOTS: return (unsigned long)&G->host_slots[0];
    case GK_CTL_HOSTCALLS: return (unsigned long)&G->hostcalls[0];
    case GK_CTL_SIGNAL_FIELDS: return (unsigned long)&G->signal_fields[0];
    case GK_CTL_RESOLVE: return (unsigned long)&gk_resolve_table.e[0];
    case GK_CTL_THREAD_WORDS: {
#ifdef GK_NEG_THREAD_WORDS
      return (unsigned long)&neg_words[0];
#else
      gk_thread *t = thread_cur();
      return t ? (unsigned long)&t->words[0] : 0;
#endif
    }
    default: return 0;
  }
}


// Refresh an arena root's shared entries from the base root so it sees every
// mapping added since it was last synced, and put the arena's private PDPT
// entries back in its own slots. Entry by entry, with 8-byte stores, never
// touching the arena's slots: another vCPU may be running under this root at
// the same time (two threads in one arena), and its page walker must never
// see a torn entry or a transiently absent slot. An entry that differs only
// in its accessed bit (bit 5, which the page walker sets in whichever root it
// walked) is the same entry and is left alone. Returns the number of entries
// that changed. Caller holds G->lock.
#define PML4E_DIFFERS(have, want) (((have) ^ (want)) & ~(1ULL << 5))
static int arena_sync_root(gk_arena *a) {
  int changed = 0;
  for (int i = 0; i < 512; i++) {
    if (i >= a->slot0 && i < a->slot0 + a->nslots) continue;
    if (PML4E_DIFFERS(a->pml4[i], G->pml4[i])) {
      __atomic_store_n(&a->pml4[i], G->pml4[i], __ATOMIC_RELAXED);
      changed++;
    }
  }
  for (int i = 0; i < a->nslots; i++)
    if (PML4E_DIFFERS(a->pml4[a->slot0 + i], a->slot_entry[i])) {
      __atomic_store_n(&a->pml4[a->slot0 + i], a->slot_entry[i], __ATOMIC_RELAXED);
      changed++;
    }
  return changed;
}

// Bring an arena root up to date with the base root if anything can have
// changed since it last was: a base-root top-level entry added since
// (G->pml4_gen moved past the root's synced_gen). The generation is read
// before the scan, so an entry added during the scan, whether the scan saw it
// or not, leaves the root marked as of a generation the next entry finds
// stale and scans again. Skipping the scan on an unchanged generation is
// what makes a warm turn's entry cheap; under GK_DEBUG the scan always runs
// and a change it finds at an unchanged generation is reported, since that
// would mean a base-root change the generation does not cover.
static void arena_sync_if_stale(gk_arena *a) {
  unsigned gen = __atomic_load_n(&G->pml4_gen, __ATOMIC_ACQUIRE);
  unsigned have = __atomic_load_n(&a->synced_gen, __ATOMIC_RELAXED);
  if (have == gen && !G->dbg) return;
  pthread_mutex_lock(&G->lock);
  int first = -1;
  uint64_t was = 0, now = 0;
  if (G->dbg && have == gen) {  // for the report below: the first entry that differs
    for (int i = 0; i < 512 && first < 0; i++) {
      int own = i >= a->slot0 && i < a->slot0 + a->nslots;
      uint64_t want = own ? a->slot_entry[i - a->slot0] : G->pml4[i];
      if (PML4E_DIFFERS(a->pml4[i], want)) { first = i; was = a->pml4[i]; now = want; }
    }
  }
  int changed = arena_sync_root(a);
  if (changed && have == gen)
    fprintf(stderr, "[gk] BUG: arena root [%#lx,%#lx) changed %d entr%s at an unchanged "
            "top-level generation %u (first: index %d, %#lx -> %#lx)\n", (unsigned long)a->base,
            (unsigned long)a->end, changed, changed == 1 ? "y" : "ies", gen, first,
            (unsigned long)was, (unsigned long)now);
  __atomic_store_n(&a->synced_gen, gen, __ATOMIC_RELAXED);
  pthread_mutex_unlock(&G->lock);
}

// Ready this thread's vCPU for a guest invocation: its root (*load, see
// sync_cr3), its PKRU on first entry, and the per-invocation fault-repeat
// tracking.
static int prepare_entry(gk_thread *t, uint64_t *load) {
  // A resident vCPU whose run returned while it was parked (a signal) is
  // out of KVM_RUN: it is entered afresh, like one that faulted (see
  // enter_guest), and its root is read from kvm_run below, not shadowed.
  if (t->parked && resident_run_returned(t)) t->parked = 0;
  if (sync_cr3(t, load) < 0) return -1;
#if GK_HVF
  commpage_refresh();
#endif
  uint32_t hp = host_pkru();
  host_pkru_allow_all();
  if (!t->pkru_ready) {
    // The thread enters the guest with the PKRU the kernel gave it (the
    // process default, or its creator's), as its guest value.
    if (guest_pkru_update(t, 0, hp, NULL) < 0) return -1;
    t->pkru_ready = 1;
    if (G->dbg) fprintf(stderr, "[gk] vcpu %d: initial guest PKRU %#x\n", t->id, hp);
  }
  t->last_fault = 0;  // fault-repeat tracking is per guest invocation
  t->fault_repeat = 0;
  t->backing_retries = 0;
  t->turn_dir_set = 0;  // a turn starts with no directory (see gk_confine_paths)
  return 0;
}


long gk_run(long (*fn)(void *), void *arg) {
  gk_thread *t = thread_get();
  uint64_t cr3;
  if (!t || vcpu_init(t, GK_VCPU_STACK) < 0) return -1;
  if (prepare_entry(t, &cr3) < 0) return -1;
  return enter_guest(t, fn, arg, t->stack_top, 0, cr3);
}

// Like gk_run, but fn runs at guest ring 3 (see enter_guest). Same private
// guest stack, same fault/return reporting.
long gk_run_user(long (*fn)(void *), void *arg) {
  gk_thread *t = thread_get();
  uint64_t cr3;
  if (!t || vcpu_init(t, GK_VCPU_STACK) < 0) return -1;
  if (prepare_entry(t, &cr3) < 0) return -1;
  return enter_guest(t, fn, arg, t->stack_top, 1, cr3);
}

// ---- gk_run_here: the guest on the caller's stack ---------------------------
// The guest starts with rsp just below the point where gk_host_call_on_stack
// switched this thread away from its stack, so it grows down into the thread's
// stack exactly as a native call from gk_run_here would. Meanwhile the host
// side (run_vcpu, forward_syscall, demand_map) runs on the vCPU's side stack,
// so its frames can never land on top of the guest's. The two stacks are host
// memory either way: the guest demand-pages the thread stack as it descends,
// and demand_map grows the main thread's stack when the kernel would have.
//
// GK_HERE_SLACK separates the guest's first frame from the switch point. No
// host frame is ever built below caller_sp while the guest runs (a signal
// arriving then is handled on the side stack, where the thread is), so a
// red-zone's worth of gap guards the switch itself. The rest of the slack is
// a whole guest page, so that the host frames above caller_sp and the guest's
// own stack never share one: the host-frame wall below is PTE-granular.
#define GK_HERE_SLACK (GK_GUEST_PAGE + 128)
typedef struct { gk_thread *t; long (*fn)(void *); void *arg; int user; uint64_t cr3; } gk_here_ctx;

// ---- the host-frame wall --------------------------------------------------
// A ring-3 fn on the caller's stack sits directly below the host frames it
// will eventually return into: gk_host_call_on_stack's saved rbp and return
// address at caller_sp, and above them run_here_common, gk_run_here_user and
// every caller up to the thread's start. Those frames are ordinary user pages,
// so arbitrary ring-3 code could overwrite a return address there and, once
// the turn ends cleanly and the host frames unwind, run at host privilege.
//
// For the length of a ring-3 turn, the caller's stack from the first page
// boundary above the guest's entry rsp up to the top of the stack (ro_hi) is
// therefore read-only to the guest: demand_map maps pages in the window
// without write permission, and a write fault inside it is genuine (see
// run_vcpu). Reads keep working, which a conservative GC scan that walks the
// whole thread stack needs. Legitimate turn code writes only below its entry
// frame; a store above it is a wall violation and ends the turn with
// GK_EFAULT.
//
// The wall is read-only, not no-read: the host frames stay readable, so a
// ring-3 escape can still read the return addresses there (an ASLR infoleak,
// though no longer a write/control primitive). Making them unreadable too
// (mapping the window supervisor) would fault the conservative GC scan, which
// walks the whole thread stack, unless that scan were first bounded to the
// guest's entry frame. V8 has the hook for that -- a per-turn
// v8::StackStartMarker (jsg.h, guarded by V8_HAS_STACK_START_MARKER) that sets
// the isolate's stack-scan start -- but it depends on an internal Cloudflare
// V8 patch that is not in the OSS tree. The low-level setter it needs
// (base::Stack::SetStackStart) does exist here, so the patch itself is small;
// the real work is confirming nothing else (V8's main-heap scan, the unwinder,
// stack-trace capture, the profiler, glibc) reads above the bound during a
// live turn. We could upgrade to a no-read wall once that patch is available;
// until then the wall stays read-only.
//
// The window is per vCPU: it walls the turn's own writes. Another thread's
// vCPU maps the same shared base subtree with its own (empty) window, so its
// writes to this thread's frames are not walled; that is the cross-thread
// residual of the shared-runtime model, not this wall's job.

// The thread pointer (the fs base): a pthread's TCB, which glibc places at the
// top of the thread's stack mapping, with its static TLS block just below.
// On arm64 Linux it is TPIDR_EL0, with glibc's struct pthread just below it
// and the static TLS block just above. On macOS it is the thread descriptor
// itself (pthread_self), which holds the TLS and starts exactly at the top of
// a pthread's stack, in the stack's own mapping.
static uintptr_t thread_pointer(void) {
  uintptr_t tp;
#if GK_HVF
  tp = (uintptr_t)pthread_self();
#elif defined(__x86_64__)
  __asm__ __volatile__("mov %%fs:0, %0" : "=r"(tp));
#else
  __asm__ __volatile__("mrs %0, tpidr_el0" : "=r"(tp));
#endif
  return tp;
}

// Compute the wall for a turn entered at rsp `top` from a host switch point
// at caller_sp: [*lo, *hi) is page-aligned and empty if nothing can be walled.
// *hi is the end of the host mapping holding the stack, clamped below the
// TCB and static TLS when they sit on that mapping (a pthread; the main
// thread's live in ld.so's own mapping): the turn writes TLS and glibc writes
// the TCB, so those must stay writable. Runs on the host before the turn.
//
// The mapping lookup reads /proc/self/maps, which is long in a process with
// many mappings and lists the stack last, so its result is cached on the
// thread record and reused while the switch point stays inside the same
// mapping (a thread's stack top and TLS never move; a switch point elsewhere,
// say on a fiber stack, is looked up afresh).
static int stack_wall_bounds(gk_thread *t, uintptr_t caller_sp, uintptr_t top,
                             uintptr_t *lo, uintptr_t *hi) {
  *lo = *hi = 0;
  if (caller_sp < t->wall_map_lo || caller_sp >= t->wall_map_hi) {
    uintptr_t rs, re; int perms;
    if (!host_region(GUEST_FLOOR(caller_sp), &rs, &re, &perms)) return -1;
    uintptr_t end = re;
    uintptr_t tp = thread_pointer();
    if (tp > caller_sp && tp < re) {
#if defined(__x86_64__)
      if (!G->tls_static_info) {
        end = 0;  // cannot place the TLS: no wall (warned at init)
      } else {
        size_t size = 0, align = 0;
        G->tls_static_info(&size, &align);
        uintptr_t tls_lo = GUEST_FLOOR(tp - size);
        if (tls_lo < end) end = tls_lo;
      }
#elif GK_HVF
      // The descriptor and its TLS start at tp and extend upward.
      uintptr_t pd_lo = GUEST_FLOOR(tp);
      if (pd_lo < end) end = pd_lo;
#else
      // The TCB and static TLS sit above tp; struct pthread (glibc's
      // TLS_PRE_TCB_SIZE, plus slack for its alignment) below it.
      if (!G->pthread_size) {
        end = 0;  // cannot place the thread descriptor: no wall (warned at init)
      } else {
        uintptr_t pd_lo = GUEST_FLOOR(tp - G->pthread_size - 64);
        if (pd_lo < end) end = pd_lo;
      }
#endif
    }
    t->wall_map_lo = rs;
    t->wall_map_hi = re;
    t->wall_end = end;
    if (G->dbg)
      fprintf(stderr, "[gk] vcpu %d: stack mapping [%#lx,%#lx) tp=%#lx caller_sp=%#lx -> wall end %#lx\n",
              t->id, (unsigned long)rs, (unsigned long)re, (unsigned long)tp,
              (unsigned long)caller_sp, (unsigned long)end);
  }
  uintptr_t first = GUEST_CEIL(top);
  if (first >= t->wall_end) return 0;
  *lo = first;
  *hi = t->wall_end;
  return 0;
}

// Raise the wall for this thread's turn: record the window and withhold write
// permission on every writable PTE any root holds for it, tagging each one
// PTE_WALL (pte_range_all: the base root and any arena root sharing the range,
// the same set a host decommit is reflected into). The PTEs stay present, so
// the few caller-stack pages a turn touches keep their translations from turn
// to turn; dropping them instead would make each one re-fault every turn, and
// every demand fault reads /proc/self/maps, which is long in this process.
// Pages in the window that are not mapped yet fault in without W through
// demand_map. This vCPU's TLB may still hold writable translations of the
// window from an earlier turn, so it is flushed too, unless that is provably
// unnecessary (below); no other vCPU's translations are touched, as the wall
// is this vCPU's alone (see above).
//
// The flush (a KVM_RUN of the flush stub, and the cold-TLB page walks the next
// turn then starts with) is a large share of a turn's fixed cost, and on the
// steady-state path it flushes nothing that matters: the turns of an event
// loop enter at the same call depth, one after another, with nothing else
// running the guest in between. Only a writable translation of a page in the
// new window needs flushing, and the TLB can hold one only if the vCPU ran
// while the page's PTE carried W. So the flush is skipped when both hold:
//
//  1. the vCPU has not run since the previous wall came down (tlb_touched is
//     clear: no KVM_RUN and no host-side CR3 write since wall_lower). The
//     window's PTEs have carried W again since then, but nothing could have
//     loaded a translation through them: the TLB holds exactly what the
//     previous turn left, and that turn ran under its own wall, under which
//     every page of [prev_ro_lo, prev_ro_hi) was read-only or absent in the
//     TLB (its raise flushed, or was itself skipped on the same argument);
//  2. the new window lies within the previous one: prev_ro_hi == hi (the same
//     stack end) and lo >= prev_ro_lo. Every page of [lo, hi) was then a
//     read-only-or-absent page of the previous turn, and a translation the
//     next run loads for it comes from the PTE, which withholds W again.
//
// Either condition failing means a writable translation of a window page may
// exist, and the flush is done as before: the vCPU ran (a ring-0 turn on this
// stack, a syscall retry, anything that entered the guest) and could have
// written through the lowered PTEs; or the new window starts lower than the
// previous one and takes in pages that were the previous turn's writable
// working stack. A vCPU with no previous wall to reason from (fresh or
// reused, see vcpu_init; a turn that had no wall; a failed flush) flushes.
// The counts are in gk_stats (wall_flushed, wall_skipped).
static int wall_raise(gk_thread *t, uintptr_t lo, uintptr_t hi) {
  if (lo >= hi) {
    // No wall this turn: the guest may write through the whole caller stack,
    // so the next wall cannot treat this turn's window as its predecessor.
    t->prev_ro_lo = t->prev_ro_hi = 0;
    return 0;
  }
  int skip = !t->tlb_touched && t->prev_ro_hi != 0 && hi == t->prev_ro_hi && lo >= t->prev_ro_lo;
  pthread_mutex_lock(&G->lock);
  t->ro_lo = lo;
  t->ro_hi = hi;
  pte_range_all(lo, hi, PTE_OP_WALL_RAISE, NULL);
  if (skip) G->wall_skipped++; else G->wall_flushed++;
  pthread_mutex_unlock(&G->lock);
  if (G->dbg)
    fprintf(stderr, "[gk] vcpu %d: wall [%#lx,%#lx) raised; previous [%#lx,%#lx), TLB %s since "
            "lowered -> %s\n", t->id, (unsigned long)lo, (unsigned long)hi,
            (unsigned long)t->prev_ro_lo, (unsigned long)t->prev_ro_hi,
            t->tlb_touched ? "touched" : "untouched", skip ? "flush skipped" : "flush");
  t->prev_ro_lo = lo;
  t->prev_ro_hi = hi;
  if (skip) return 0;
  if (flush_run(t) < 0) {
    t->prev_ro_lo = t->prev_ro_hi = 0;  // TLB state unknown: the next wall flushes
    return -1;
  }
  return 0;
}

// Lower the wall after the turn: give write permission back to every PTE the
// wall withheld it from (those tagged PTE_WALL, by wall_raise or by demand_map
// during the turn) and clear the window. A page the host allows no write on
// was never tagged and stays as it is, so the window's PTEs again mirror the
// host protection. This vCPU's TLB is not flushed: its read-only translations
// of the window are what the next ring-3 turn wants anyway (wall_raise
// flushes before the window can differ, or proves that it cannot), and a
// ring-0 run in between that writes through one takes a spurious #PF, which
// the fault path resolves by re-mapping against the now-writable PTE. From
// here until the vCPU next runs, its TLB is known (tlb_touched clear), unless
// the walk finds a window page writable without the wall's tag: the wall
// withheld W from every writable page when it went up, so such a page was
// mapped or reprotected writable during the turn by another vCPU (the
// cross-thread residual, see run_here) and this vCPU may hold a writable
// translation of it. The TLB then counts as touched, so the next wall_raise
// flushes rather than reasons from this turn.
static void wall_lower(gk_thread *t) {
  if (t->ro_lo >= t->ro_hi) return;
  pthread_mutex_lock(&G->lock);
  long foreign = pte_range_all(t->ro_lo, t->ro_hi, PTE_OP_WALL_LOWER, NULL);
  t->ro_lo = t->ro_hi = 0;
  pthread_mutex_unlock(&G->lock);
  if (foreign == 0) {
    t->tlb_touched = 0;
  } else if (G->dbg) {
    fprintf(stderr, "[gk] vcpu %d: wall lowered over %ld page(s) another vCPU mapped writable; "
            "the next wall flushes\n", t->id, foreign);
  }
}

static long run_here(void *ctx, unsigned long caller_sp) {
  gk_here_ctx *c = ctx;
  uint64_t top = (caller_sp - GK_HERE_SLACK) & ~0xfULL;
  if (!c->user) return enter_guest(c->t, c->fn, c->arg, top, 0, c->cr3);
  uintptr_t lo, hi;
  if (stack_wall_bounds(c->t, caller_sp, top, &lo, &hi) < 0) {
    G->err = "gk_run_here_user: caller stack not in /proc/self/maps";
    return -1;
  }
  if (wall_raise(c->t, lo, hi) < 0) {
    wall_lower(c->t);
    return -1;
  }
  long r = enter_guest(c->t, c->fn, c->arg, top, 1, c->cr3);
  wall_lower(c->t);
  return r;
}

static long run_here_common(long (*fn)(void *), void *arg, int user) {
  gk_thread *t = thread_get();
  if (!t || vcpu_init(t, 0) < 0) return -1;
  if (!t->side_stack) {
    // The gk-loop side stack is host-only: refuse it so ring-3 code cannot
    // reach the host frames running underneath it. It is mapped PROT_NONE
    // until registered, so no vCPU can fault it in as user memory in between.
    void *ss = mmap(NULL, GK_SIDE_STACK, PROT_NONE,
                    MAP_PRIVATE | MAP_ANONYMOUS | MAP_STACK, -1, 0);
    if (ss == MAP_FAILED) { G->err = "mmap side stack"; return -1; }
    pthread_mutex_lock(&G->lock);
    prot_add((uintptr_t)ss, (uintptr_t)ss + GK_SIDE_STACK, GK_PROT_REFUSE);
    pthread_mutex_unlock(&G->lock);
    if (mprotect(ss, GK_SIDE_STACK, PROT_READ | PROT_WRITE) != 0) {
      G->err = "mprotect side stack";
      return -1;
    }
    t->side_stack = ss;
  }
  uint64_t cr3;
  if (prepare_entry(t, &cr3) < 0) return -1;
  gk_here_ctx c = {t, fn, arg, user, cr3};  // lives above caller_sp, out of the guest's way
  return gk_host_call_on_stack((char *)t->side_stack + GK_SIDE_STACK, run_here, &c);
}

long gk_run_here(long (*fn)(void *), void *arg) {
  return run_here_common(fn, arg, 0);
}

// Like gk_run_here, but fn runs at guest ring 3 (see enter_guest).
long gk_run_here_user(long (*fn)(void *), void *arg) {
  return run_here_common(fn, arg, 1);
}

void gk_get_stats(gk_stats *s) {
  pthread_mutex_lock(&G->lock);
  s->memslots = G->region_live;
  s->memslot_ids = G->next_slot;
  s->arenas = G->arena_n;
  s->pt_pages_used = G->pt_used;
  s->pt_pages_free = G->pt_free_n;
  s->pt_pages_total = (long)((G->pt_next - (uint8_t *)G->pt_base) >> 12);
  s->prot_ranges = G->prot_n;
  s->demand_faults = G->demand_ok;
  s->wall_flushed = G->wall_flushed;
  s->wall_skipped = G->wall_skipped;
  s->global_pages = G->global_pages;
  s->neighbor_pages = G->neighbor_pages;
  s->root_switches = atomic_load_explicit(&G->root_switches, memory_order_relaxed);
  s->tlb_flushes = atomic_load_explicit(&G->tlb_flushes, memory_order_relaxed);
#if defined(__x86_64__)
  s->flush_invpcid = G->flush_stub == gk_flush_stub_invpcid;
#else
  s->flush_invpcid = 0;
#endif
  s->host_regions = G->host_region_n;
  s->host_blocks = G->host_blocks;
  s->host_slots = G->host_slot_live;
  s->hostcall_handlers = G->hostcall_n;
  s->hostcalls = atomic_load_explicit(&G->hostcall_calls, memory_order_relaxed);
  s->signal_fields = G->signal_field_live;
  s->resolve_entries = G->resolve_live;
  s->resolve_orphans = G->resolve_orphans;
  s->path_refusals = atomic_load_explicit(&G->path_refusals, memory_order_relaxed);
  s->runners = G->runner_n;
  s->runner_runs = atomic_load_explicit(&G->runner_runs, memory_order_relaxed);
  s->resident_vcpus = G->resident_n;
  s->park_turns = atomic_load_explicit(&G->park_turns, memory_order_relaxed);
  s->park_kicks = atomic_load_explicit(&G->park_kicks, memory_order_relaxed);
  pthread_mutex_unlock(&G->lock);
}
int gk_turn_cpu_clockid(clockid_t *id) {
  if (!G->initialized) return -GK_ESRCH;
  gk_thread *t = thread_cur();
  if (!t || !t->inited || !t->runner) {
    *id = CLOCK_THREAD_CPUTIME_ID;
    return 0;
  }
  // The kernel's encoding of a thread's CPU clock (CPUCLOCK_SCHED, per
  // thread), as glibc's pthread_getcpuclockid builds it from a tid.
  long tid = atomic_load_explicit(&t->runner->tid, memory_order_seq_cst);
  *id = (clockid_t)(((~tid) << 3) | 6);
  return 0;
}

// Test/diagnostic hook (see gk.h): drop the PTE of the page holding `addr`
// from every root, without a flush. Equivalent to the reflection of a munmap of
// that page minus the calling vCPU's flush, so any translation a vCPU holds
// keeps working until something flushes it.
void gk_debug_drop_pte(unsigned long addr) {
  uintptr_t page = GUEST_FLOOR((uintptr_t)addr);
  pthread_mutex_lock(&G->lock);
  unmap_range_all(page, page + GK_GUEST_PAGE);
  pthread_mutex_unlock(&G->lock);
}

// Test/diagnostic hook (see gk.h): register or unregister a page-aligned range
// of the caller's own memory in the supervisor/refuse registry, so a test can
// place a SUPER or REFUSE page wherever it wants one. Registering drops any
// PTE a root holds for the range, as gk's own registrations do.
void gk_debug_protect_range(unsigned long addr, size_t len, int kind) {
  uintptr_t s = GUEST_FLOOR((uintptr_t)addr), e = GUEST_CEIL((uintptr_t)addr + len);
  if (s >= e) return;
  pthread_mutex_lock(&G->lock);
  if (kind == GK_DEBUG_PROT_SUPER) prot_add(s, e, GK_PROT_SUPER);
  else if (kind == GK_DEBUG_PROT_REFUSE) prot_add(s, e, GK_PROT_REFUSE);
  else prot_remove(s, e);
  pthread_mutex_unlock(&G->lock);
}
