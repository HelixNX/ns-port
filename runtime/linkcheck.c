/* Link check for the lifted code: the runtime entry points as stubs, and the entry tables that reference every
 * lifted function. If this links, every call between lifted functions (and across modules) resolves. */
#include <stdio.h>
#include <stdlib.h>
#include "a64rt.h"

extern const ModInfo lifted_modules[];

void call_ind(Ctx *c, u64 addr) { guest_trap(c, "call_ind", addr); }
void jump_ind(Ctx *c) { guest_trap(c, "jump_ind", c->pc); }
void guest_trap(Ctx *c, const char *what, u64 pc) { printf("trap %s at %llx\n", what, (unsigned long long)pc); exit(1); }
void guest_unimpl(Ctx *c, u64 pc, u32 insn, const char *what) { guest_trap(c, what, pc); }
void guest_svc(Ctx *c, u32 imm) { guest_trap(c, "svc", imm); }
void host_yield(void) {}
u64 host_cntpct(void) { return 0; }
void host_set_fpcr(Ctx *c) {}
void u_nnosGetDefaultUserExceptionStackForApplication(Ctx *c) { guest_trap(c, "unresolved import", 0); }

int main(void) {
    for (const ModInfo *m = lifted_modules; m->name; m++) {
        size_t n = 0;
        while (m->entries[n].fn) n++;
        printf("%-8s base %llx  text end %llx  %zu entries\n", m->name, (unsigned long long)m->base,
               (unsigned long long)m->text_end, n);
    }
    return 0;
}
