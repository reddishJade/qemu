/*
 * host-signal.h: signal info dependent on the host architecture
 *
 * Copyright (c) 2003-2005 Fabrice Bellard
 * Copyright (c) 2021 Linaro Limited
 *
 * This work is licensed under the terms of the GNU LGPL, version 2.1 or later.
 * See the COPYING file in the top-level directory.
 */

#ifndef SW_64_HOST_SIGNAL_H
#define SW_64_HOST_SIGNAL_H

/* The third argument to a SA_SIGINFO handler is ucontext_t. */
typedef ucontext_t host_sigcontext;

static inline uintptr_t host_signal_pc(host_sigcontext *uc)
{
    return uc->uc_mcontext.sc_pc;
}

static inline void host_signal_set_pc(host_sigcontext *uc, uintptr_t pc)
{
    uc->uc_mcontext.sc_pc = pc;
}

static inline void *host_signal_mask(host_sigcontext *uc)
{
    return &uc->uc_sigmask;
}

static inline bool host_signal_write(siginfo_t *info, host_sigcontext *uc)
{
    uint32_t *pc = (uint32_t *)host_signal_pc(uc);
    uint32_t insn = *pc;

    /* XXX: need kernel patch to get write flag faster */
    switch (insn >> 26) {
    case 0x28: /* stb */
    case 0x29: /* sth */
    case 0x2A: /* stw */
    case 0x2B: /* stl */
    case 0x2C: /* stl_u */
    case 0x2E: /* fsts */
    case 0x2F: /* fstd */
    case 0x08: /* lstl */
        return true;
    }
    return false;
}

#endif
