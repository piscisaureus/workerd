// Guest-side trampolines and exception handlers for gk. These run inside the
// guest in ring 0 and live in this binary's text, which gk maps into the guest.
// The code for each architecture is its own file, included here.
//
// The arm64 code assembles for ELF (Linux) and Mach-O (macOS). Mach-O prefixes
// C-visible symbols with an underscore, names a section by segment and section,
// and has no .type directive; the macros below spell each form.
#if defined(__APPLE__)
#define SYM(name) _##name
#define FUNC_TYPE(name)
#define SECTION_GK_SUPER .section __TEXT,__gk_super,regular,pure_instructions
#define SECTION_GK_USER_EXIT .section __TEXT,__gk_user_exit,regular,pure_instructions
#else
#define SYM(name) name
#define FUNC_TYPE(name) .type name, @function
#define SECTION_GK_SUPER .section .text.gk_super,"ax",@progbits
#define SECTION_GK_USER_EXIT .section .text.gk_user_exit,"ax",@progbits
#endif
#if defined(__x86_64__)
#include "gk_asm_x86_64.S"
#elif defined(__aarch64__)
#include "gk_asm_arm64.S"
#else
#error "gk supports x86-64 and arm64 only"
#endif

#if !defined(__APPLE__)
    .section .note.GNU-stack,"",@progbits
#endif
