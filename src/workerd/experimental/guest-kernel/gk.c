// gk implementation. See gk.h, and gk_internal.h for how the parts fit.
//
// gk is compiled as this one translation unit: every part is included here,
// and each part compiles to nothing on a platform it is not for. The
// backend, the architecture and the host OS are fixed at compile time, so the
// parts call each other directly and the per-turn paths inline across them.
#include "gk_internal.h"

#include "gk_util.c"
#include "gk_hvf.c"
#include "gk_linux.c"
#include "gk_kvm.c"
#include "gk_pt.c"
#include "gk_region.c"
#include "gk_pkey.c"
#include "gk_thread.c"
#include "gk_x86_64.c"
#include "gk_arm64.c"
#include "gk_kvm_arm64.c"
#include "gk_core.c"
#include "gk_arena.c"
