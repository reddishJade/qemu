```c
static void tcg_reg_alloc_op(TCGContext *s, const TCGOp *op)
{
    const TCGLifeData arg_life = op->life;
    const TCGOpDef * const def = &tcg_op_defs[op->opc];
    TCGRegSet i_allocated_regs;
    TCGRegSet o_allocated_regs;
    int i, k, nb_iargs, nb_oargs;
    TCGReg reg;
    TCGArg arg;
    const TCGArgConstraint *arg_ct;
    TCGTemp *ts;
    TCGArg new_args[TCG_MAX_OP_ARGS];
    int const_args[TCG_MAX_OP_ARGS];

    nb_oargs = def->nb_oargs;
    nb_iargs = def->nb_iargs;

    /* copy constants */
    memcpy(new_args + nb_oargs + nb_iargs, 
           op->args + nb_oargs + nb_iargs,
           sizeof(TCGArg) * def->nb_cargs);

    i_allocated_regs = s->reserved_regs;
    o_allocated_regs = s->reserved_regs;

    /* satisfy input constraints */ 
    for (k = 0; k < nb_iargs; k++) {
        TCGRegSet i_preferred_regs, o_preferred_regs;

        i = def->args_ct[nb_oargs + k].sort_index;
        arg = op->args[i];
        arg_ct = &def->args_ct[i];
        ts = arg_temp(arg);

        if (ts->val_type == TEMP_VAL_CONST
            && tcg_target_const_match(ts->val, ts->type, arg_ct->ct)) {
            /* constant is OK for instruction */
            const_args[i] = 1;
            new_args[i] = ts->val;
            continue;
        }

        i_preferred_regs = o_preferred_regs = 0;
        if (arg_ct->ialias) {
            o_preferred_regs = op->output_pref[arg_ct->alias_index];

            /*
             * If the input is readonly, then it cannot also be an
             * output and aliased to itself.  If the input is not
             * dead after the instruction, we must allocate a new
             * register and move it.
             */
            if (temp_readonly(ts) || !IS_DEAD_ARG(i)) {
                goto allocate_in_reg;
            }

            /*
             * Check if the current register has already been allocated
             * for another input aliased to an output.
             */
            if (ts->val_type == TEMP_VAL_REG) {
                reg = ts->reg;
                for (int k2 = 0; k2 < k; k2++) {
                    int i2 = def->args_ct[nb_oargs + k2].sort_index;
                    if (def->args_ct[i2].ialias && reg == new_args[i2]) {
                        goto allocate_in_reg;
                    }
                }
            }
            i_preferred_regs = o_preferred_regs;
        }

        temp_load(s, ts, arg_ct->regs, i_allocated_regs, i_preferred_regs);
        reg = ts->reg;

        if (!tcg_regset_test_reg(arg_ct->regs, reg)) {
 allocate_in_reg:
            /*
             * Allocate a new register matching the constraint
             * and move the temporary register into it.
             */
            temp_load(s, ts, tcg_target_available_regs[ts->type],
                      i_allocated_regs, 0);
            reg = tcg_reg_alloc(s, arg_ct->regs, i_allocated_regs,
                                o_preferred_regs, ts->indirect_base);
            if (!tcg_out_mov(s, ts->type, reg, ts->reg)) {
                /*
                 * Cross register class move not supported.  Sync the
                 * temp back to its slot and load from there.
                 */
                temp_sync(s, ts, i_allocated_regs, 0, 0);
                tcg_out_ld(s, ts->type, reg,
                           ts->mem_base->reg, ts->mem_offset);
            }
        }
        new_args[i] = reg;
        const_args[i] = 0;
        tcg_regset_set_reg(i_allocated_regs, reg);
    }
    
    /* mark dead temporaries and free the associated registers */
    for (i = nb_oargs; i < nb_oargs + nb_iargs; i++) {
        if (IS_DEAD_ARG(i)) {
            temp_dead(s, arg_temp(op->args[i]));
        }
    }

    if (def->flags & TCG_OPF_COND_BRANCH) {
        tcg_reg_alloc_cbranch(s, i_allocated_regs);
    } else if (def->flags & TCG_OPF_BB_END) {
        tcg_reg_alloc_bb_end(s, i_allocated_regs);
    } else {
        if (def->flags & TCG_OPF_CALL_CLOBBER) {
            /* XXX: permit generic clobber register list ? */ 
            for (i = 0; i < TCG_TARGET_NB_REGS; i++) {
                if (tcg_regset_test_reg(tcg_target_call_clobber_regs, i)) {
                    tcg_reg_free(s, i, i_allocated_regs);
                }
            }
        }
        if (def->flags & TCG_OPF_SIDE_EFFECTS) {
            /* sync globals if the op has side effects and might trigger
               an exception. */
            sync_globals(s, i_allocated_regs);
        }
        
        /* satisfy the output constraints */
        for(k = 0; k < nb_oargs; k++) {
            i = def->args_ct[k].sort_index;
            arg = op->args[i];
            arg_ct = &def->args_ct[i];
            ts = arg_temp(arg);

            /* ENV should not be modified.  */
            tcg_debug_assert(!temp_readonly(ts));

            if (arg_ct->oalias && !const_args[arg_ct->alias_index]) {
                reg = new_args[arg_ct->alias_index];
            } else if (arg_ct->newreg) {
                reg = tcg_reg_alloc(s, arg_ct->regs,
                                    i_allocated_regs | o_allocated_regs,
                                    op->output_pref[k], ts->indirect_base);
            } else {
                reg = tcg_reg_alloc(s, arg_ct->regs, o_allocated_regs,
                                    op->output_pref[k], ts->indirect_base);
            }
            tcg_regset_set_reg(o_allocated_regs, reg);
            if (ts->val_type == TEMP_VAL_REG) {
                s->reg_to_temp[ts->reg] = NULL;
            }
            ts->val_type = TEMP_VAL_REG;
            ts->reg = reg;
            /*
             * Temp value is modified, so the value kept in memory is
             * potentially not the same.
             */
            ts->mem_coherent = 0;
            s->reg_to_temp[reg] = ts;
            new_args[i] = reg;
        }
    }

    /* emit instruction */
    if (def->flags & TCG_OPF_VECTOR) {
        tcg_out_vec_op(s, op->opc, TCGOP_VECL(op), TCGOP_VECE(op),
                       new_args, const_args);
    } else {
        tcg_out_op(s, op->opc, new_args, const_args);
    }

    /* move the outputs in the correct register if needed */
    for(i = 0; i < nb_oargs; i++) {
        ts = arg_temp(op->args[i]);

        /* ENV should not be modified.  */
        tcg_debug_assert(!temp_readonly(ts));

        if (NEED_SYNC_ARG(i)) {
            temp_sync(s, ts, o_allocated_regs, 0, IS_DEAD_ARG(i));
        } else if (IS_DEAD_ARG(i)) {
            temp_dead(s, ts);
        }
    }
}
```