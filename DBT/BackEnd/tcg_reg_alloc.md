```c
static TCGReg tcg_reg_alloc(TCGContext *s, TCGRegSet required_regs,
                            TCGRegSet allocated_regs,
                            TCGRegSet preferred_regs, bool rev)
{
    int i, j, f, n = ARRAY_SIZE(tcg_target_reg_alloc_order);
    TCGRegSet reg_ct[2];
    const int *order;

    reg_ct[1] = required_regs & ~allocated_regs;
    tcg_debug_assert(reg_ct[1] != 0);
    reg_ct[0] = reg_ct[1] & preferred_regs;

    /* Skip the preferred_regs option if it cannot be satisfied,
       or if the preference made no difference.  */
    f = reg_ct[0] == 0 || reg_ct[0] == reg_ct[1];

    order = rev ? indirect_reg_alloc_order : tcg_target_reg_alloc_order;

    /* Try free registers, preferences first.  */
    for (j = f; j < 2; j++) {
        TCGRegSet set = reg_ct[j];

        if (tcg_regset_single(set)) {
            /* One register in the set.  */
            TCGReg reg = tcg_regset_first(set);
            if (s->reg_to_temp[reg] == NULL) {
                return reg;
            }
        } else {
            for (i = 0; i < n; i++) {
                TCGReg reg = order[i];
                if (s->reg_to_temp[reg] == NULL &&
                    tcg_regset_test_reg(set, reg)) {
                    return reg;
                }
            }
        }
    }

    /* We must spill something.  */
    for (j = f; j < 2; j++) {
        TCGRegSet set = reg_ct[j];

        if (tcg_regset_single(set)) {
            /* One register in the set.  */
            TCGReg reg = tcg_regset_first(set);
            tcg_reg_free(s, reg, allocated_regs);
            return reg;
        } else {
            for (i = 0; i < n; i++) {
                TCGReg reg = order[i];
                if (tcg_regset_test_reg(set, reg)) {
                    tcg_reg_free(s, reg, allocated_regs);
                    return reg;
                }
            }
        }
    }

    tcg_abort();
}
```