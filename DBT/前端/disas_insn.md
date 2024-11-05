```c
/* convert one instruction. s->base.is_jmp is set if the translation must
   be stopped. Return the next pc value */
static bool disas_insn(DisasContext *s, CPUState *cpu)
{
    CPUX86State *env = cpu->env_ptr;
    int b, prefixes;
    int shift;
    MemOp ot, aflag, dflag;
    int modrm, reg, rm, mod, op, opreg, val;
    bool orig_cc_op_dirty = s->cc_op_dirty;
    CCOp orig_cc_op = s->cc_op;
    target_ulong orig_pc_save = s->pc_save;

    s->pc = s->base.pc_next;
    s->override = -1;
#ifdef TARGET_X86_64
    s->rex_r = 0;
    s->rex_x = 0;
    s->rex_b = 0;
#endif
    s->rip_offset = 0; /* for relative ip address */
    s->vex_l = 0;
    s->vex_v = 0;
    s->vex_w = false;
    switch (sigsetjmp(s->jmpbuf, 0)) {
    case 0:
        break;
    case 1:
        gen_exception_gpf(s);
        return true;
    case 2:
        /* Restore state that may affect the next instruction. */
        s->pc = s->base.pc_next;
        /*
         * TODO: These save/restore can be removed after the table-based
         * decoder is complete; we will be decoding the insn completely
         * before any code generation that might affect these variables.
         */
        s->cc_op_dirty = orig_cc_op_dirty;
        s->cc_op = orig_cc_op;
        s->pc_save = orig_pc_save;
        /* END TODO */
        s->base.num_insns--;
        tcg_remove_ops_after(s->prev_insn_end);
        s->base.is_jmp = DISAS_TOO_MANY;
        return false;
    default:
        g_assert_not_reached();
    }

    prefixes = 0;

 next_byte:
    s->prefix = prefixes;
    b = x86_ldub_code(env, s);
    /* Collect prefixes.  */
    switch (b) {
    default:
        break;
    case 0x0f:
        b = x86_ldub_code(env, s) + 0x100;
        break;
    case 0xf3:
        prefixes |= PREFIX_REPZ;
        prefixes &= ~PREFIX_REPNZ;
        goto next_byte;
    case 0xf2:
        prefixes |= PREFIX_REPNZ;
        prefixes &= ~PREFIX_REPZ;
        goto next_byte;
    case 0xf0:
        prefixes |= PREFIX_LOCK;
        goto next_byte;
    case 0x2e:
        s->override = R_CS;
        goto next_byte;
    case 0x36:
        s->override = R_SS;
        goto next_byte;
    case 0x3e:
        s->override = R_DS;
        goto next_byte;
    case 0x26:
        s->override = R_ES;
        goto next_byte;
    case 0x64:
        s->override = R_FS;
        goto next_byte;
    case 0x65:
        s->override = R_GS;
        goto next_byte;
    case 0x66:
        prefixes |= PREFIX_DATA;
        goto next_byte;
    case 0x67:
        prefixes |= PREFIX_ADR;
        goto next_byte;
#ifdef TARGET_X86_64
    case 0x40 ... 0x4f:
        if (CODE64(s)) {
            /* REX prefix */
            prefixes |= PREFIX_REX;
            s->vex_w = (b >> 3) & 1;
            s->rex_r = (b & 0x4) << 1;
            s->rex_x = (b & 0x2) << 2;
            s->rex_b = (b & 0x1) << 3;
            goto next_byte;
        }
        break;
#endif
    case 0xc5: /* 2-byte VEX */
    case 0xc4: /* 3-byte VEX */
        if (CODE32(s) && !VM86(s)) {
            int vex2 = x86_ldub_code(env, s);
            s->pc--; /* rewind the advance_pc() x86_ldub_code() did */

            if (!CODE64(s) && (vex2 & 0xc0) != 0xc0) {
                /* 4.1.4.6: In 32-bit mode, bits [7:6] must be 11b,
                   otherwise the instruction is LES or LDS.  */
                break;
            }
            disas_insn_new(s, cpu, b);
            return s->pc;
        }
        break;
    }

    /* Post-process prefixes.  */
    if (CODE64(s)) {
        /* In 64-bit mode, the default data size is 32-bit.  Select 64-bit
           data with rex_w, and 16-bit data with 0x66; rex_w takes precedence
           over 0x66 if both are present.  */
        dflag = (REX_W(s) ? MO_64 : prefixes & PREFIX_DATA ? MO_16 : MO_32);
        /* In 64-bit mode, 0x67 selects 32-bit addressing.  */
        aflag = (prefixes & PREFIX_ADR ? MO_32 : MO_64);
    } else {
        /* In 16/32-bit mode, 0x66 selects the opposite data size.  */
        if (CODE32(s) ^ ((prefixes & PREFIX_DATA) != 0)) {
            dflag = MO_32;
        } else {
            dflag = MO_16;
        }
        /* In 16/32-bit mode, 0x67 selects the opposite addressing.  */
        if (CODE32(s) ^ ((prefixes & PREFIX_ADR) != 0)) {
            aflag = MO_32;
        }  else {
            aflag = MO_16;
        }
    }

    s->prefix = prefixes;
    s->aflag = aflag;
    s->dflag = dflag;

    /* now check op code */
    switch (b) {
        /**************************/
        /* arith & logic */
    case 0x00 ... 0x05:
    case 0x08 ... 0x0d:
    case 0x10 ... 0x15:
    case 0x18 ... 0x1d:
    case 0x20 ... 0x25:
    case 0x28 ... 0x2d:
    case 0x30 ... 0x35:
    case 0x38 ... 0x3d:
        {
            int op, f, val;
            op = (b >> 3) & 7;
            f = (b >> 1) & 3;

            ot = mo_b_d(b, dflag);

            switch(f) {
            case 0: /* OP Ev, Gv */
                modrm = x86_ldub_code(env, s);
                reg = ((modrm >> 3) & 7) | REX_R(s);
                mod = (modrm >> 6) & 3;
                rm = (modrm & 7) | REX_B(s);
                if (mod != 3) {
                    gen_lea_modrm(env, s, modrm);
                    opreg = OR_TMP0;
                } else if (op == OP_XORL && rm == reg) {
                xor_zero:
                    /* xor reg, reg optimisation */
                    set_cc_op(s, CC_OP_CLR);
                    tcg_gen_movi_tl(s->T0, 0);
                    gen_op_mov_reg_v(s, ot, reg, s->T0);
                    break;
                } else {
                    opreg = rm;
                }
                gen_op_mov_v_reg(s, ot, s->T1, reg);
                gen_op(s, op, ot, opreg);
                break;
            case 1: /* OP Gv, Ev */
                modrm = x86_ldub_code(env, s);
                mod = (modrm >> 6) & 3;
                reg = ((modrm >> 3) & 7) | REX_R(s);
                rm = (modrm & 7) | REX_B(s);
                if (mod != 3) {
                    gen_lea_modrm(env, s, modrm);
                    gen_op_ld_v(s, ot, s->T1, s->A0);
                } else if (op == OP_XORL && rm == reg) {
                    goto xor_zero;
                } else {
                    gen_op_mov_v_reg(s, ot, s->T1, rm);
                }
                gen_op(s, op, ot, reg);
                break;
            case 2: /* OP A, Iv */
                val = insn_get(env, s, ot);
                tcg_gen_movi_tl(s->T1, val);
                gen_op(s, op, ot, OR_EAX);
                break;
            }
        }
        break;

    case 0x82:
        if (CODE64(s))
            goto illegal_op;
        /* fall through */
    case 0x80: /* GRP1 */
    case 0x81:
    case 0x83:
        {
            int val;

            ot = mo_b_d(b, dflag);

            modrm = x86_ldub_code(env, s);
            mod = (modrm >> 6) & 3;
            rm = (modrm & 7) | REX_B(s);
            op = (modrm >> 3) & 7;

            if (mod != 3) {
                if (b == 0x83)
                    s->rip_offset = 1;
                else
                    s->rip_offset = insn_const_size(ot);
                gen_lea_modrm(env, s, modrm);
                opreg = OR_TMP0;
            } else {
                opreg = rm;
            }

            switch(b) {
            default:
            case 0x80:
            case 0x81:
            case 0x82:
                val = insn_get(env, s, ot);
                break;
            case 0x83:
                val = (int8_t)insn_get(env, s, MO_8);
                break;
            }
            tcg_gen_movi_tl(s->T1, val);
            gen_op(s, op, ot, opreg);
        }
        break;

        /**************************/
        /* inc, dec, and other misc arith */
    case 0x40 ... 0x47: /* inc Gv */
        ot = dflag;
        gen_inc(s, ot, OR_EAX + (b & 7), 1);
        break;
    case 0x48 ... 0x4f: /* dec Gv */
        ot = dflag;
        gen_inc(s, ot, OR_EAX + (b & 7), -1);
        break;
    case 0xf6: /* GRP3 */
    case 0xf7:
        ot = mo_b_d(b, dflag);

        modrm = x86_ldub_code(env, s);
        mod = (modrm >> 6) & 3;
        rm = (modrm & 7) | REX_B(s);
        op = (modrm >> 3) & 7;
        if (mod != 3) {
            if (op == 0) {
                s->rip_offset = insn_const_size(ot);
            }
            gen_lea_modrm(env, s, modrm);
            /* For those below that handle locked memory, don't load here.  */
            if (!(s->prefix & PREFIX_LOCK)
                || op != 2) {
                gen_op_ld_v(s, ot, s->T0, s->A0);
            }
        } else {
            gen_op_mov_v_reg(s, ot, s->T0, rm);
        }

        switch(op) {
        case 0: /* test */
            val = insn_get(env, s, ot);
            tcg_gen_movi_tl(s->T1, val);
            gen_op_testl_T0_T1_cc(s);
            set_cc_op(s, CC_OP_LOGICB + ot);
            break;
        case 2: /* not */
            if (s->prefix & PREFIX_LOCK) {
                if (mod == 3) {
                    goto illegal_op;
                }
                tcg_gen_movi_tl(s->T0, ~0);
                tcg_gen_atomic_xor_fetch_tl(s->T0, s->A0, s->T0,
                                            s->mem_index, ot | MO_LE);
            } else {
                tcg_gen_not_tl(s->T0, s->T0);
                if (mod != 3) {
                    gen_op_st_v(s, ot, s->T0, s->A0);
                } else {
                    gen_op_mov_reg_v(s, ot, rm, s->T0);
                }
            }
            break;
        case 3: /* neg */
            if (s->prefix & PREFIX_LOCK) {
                TCGLabel *label1;
                TCGv a0, t0, t1, t2;

                if (mod == 3) {
                    goto illegal_op;
                }
                a0 = tcg_temp_local_new();
                t0 = tcg_temp_local_new();
                label1 = gen_new_label();

                tcg_gen_mov_tl(a0, s->A0);
                tcg_gen_mov_tl(t0, s->T0);

                gen_set_label(label1);
                t1 = tcg_temp_new();
                t2 = tcg_temp_new();
                tcg_gen_mov_tl(t2, t0);
                tcg_gen_neg_tl(t1, t0);
                tcg_gen_atomic_cmpxchg_tl(t0, a0, t0, t1,
                                          s->mem_index, ot | MO_LE);
                tcg_temp_free(t1);
                tcg_gen_brcond_tl(TCG_COND_NE, t0, t2, label1);

                tcg_temp_free(t2);
                tcg_temp_free(a0);
                tcg_gen_neg_tl(s->T0, t0);
                tcg_temp_free(t0);
            } else {
                tcg_gen_neg_tl(s->T0, s->T0);
                if (mod != 3) {
                    gen_op_st_v(s, ot, s->T0, s->A0);
                } else {
                    gen_op_mov_reg_v(s, ot, rm, s->T0);
                }
            }
            gen_op_update_neg_cc(s);
            set_cc_op(s, CC_OP_SUBB + ot);
            break;
        case 4: /* mul */
            switch(ot) {
            case MO_8:
                gen_op_mov_v_reg(s, MO_8, s->T1, R_EAX);
                tcg_gen_ext8u_tl(s->T0, s->T0);
                tcg_gen_ext8u_tl(s->T1, s->T1);
                /* XXX: use 32 bit mul which could be faster */
                tcg_gen_mul_tl(s->T0, s->T0, s->T1);
                gen_op_mov_reg_v(s, MO_16, R_EAX, s->T0);
                tcg_gen_mov_tl(cpu_cc_dst, s->T0);
                tcg_gen_andi_tl(cpu_cc_src, s->T0, 0xff00);
                set_cc_op(s, CC_OP_MULB);
                break;
            case MO_16:
                gen_op_mov_v_reg(s, MO_16, s->T1, R_EAX);
                tcg_gen_ext16u_tl(s->T0, s->T0);
                tcg_gen_ext16u_tl(s->T1, s->T1);
                /* XXX: use 32 bit mul which could be faster */
                tcg_gen_mul_tl(s->T0, s->T0, s->T1);
                gen_op_mov_reg_v(s, MO_16, R_EAX, s->T0);
                tcg_gen_mov_tl(cpu_cc_dst, s->T0);
                tcg_gen_shri_tl(s->T0, s->T0, 16);
                gen_op_mov_reg_v(s, MO_16, R_EDX, s->T0);
                tcg_gen_mov_tl(cpu_cc_src, s->T0);
                set_cc_op(s, CC_OP_MULW);
                break;
            default:
            case MO_32:
                tcg_gen_trunc_tl_i32(s->tmp2_i32, s->T0);
                tcg_gen_trunc_tl_i32(s->tmp3_i32, cpu_regs[R_EAX]);
                tcg_gen_mulu2_i32(s->tmp2_i32, s->tmp3_i32,
                                  s->tmp2_i32, s->tmp3_i32);
                tcg_gen_extu_i32_tl(cpu_regs[R_EAX], s->tmp2_i32);
                tcg_gen_extu_i32_tl(cpu_regs[R_EDX], s->tmp3_i32);
                tcg_gen_mov_tl(cpu_cc_dst, cpu_regs[R_EAX]);
                tcg_gen_mov_tl(cpu_cc_src, cpu_regs[R_EDX]);
                set_cc_op(s, CC_OP_MULL);
                break;
#ifdef TARGET_X86_64
            case MO_64:
                tcg_gen_mulu2_i64(cpu_regs[R_EAX], cpu_regs[R_EDX],
                                  s->T0, cpu_regs[R_EAX]);
                tcg_gen_mov_tl(cpu_cc_dst, cpu_regs[R_EAX]);
                tcg_gen_mov_tl(cpu_cc_src, cpu_regs[R_EDX]);
                set_cc_op(s, CC_OP_MULQ);
                break;
#endif
            }
            break;
        case 5: /* imul */
            switch(ot) {
            case MO_8:
                gen_op_mov_v_reg(s, MO_8, s->T1, R_EAX);
                tcg_gen_ext8s_tl(s->T0, s->T0);
                tcg_gen_ext8s_tl(s->T1, s->T1);
                /* XXX: use 32 bit mul which could be faster */
                tcg_gen_mul_tl(s->T0, s->T0, s->T1);
                gen_op_mov_reg_v(s, MO_16, R_EAX, s->T0);
                tcg_gen_mov_tl(cpu_cc_dst, s->T0);
                tcg_gen_ext8s_tl(s->tmp0, s->T0);
                tcg_gen_sub_tl(cpu_cc_src, s->T0, s->tmp0);
                set_cc_op(s, CC_OP_MULB);
                break;
            case MO_16:
                gen_op_mov_v_reg(s, MO_16, s->T1, R_EAX);
                tcg_gen_ext16s_tl(s->T0, s->T0);
                tcg_gen_ext16s_tl(s->T1, s->T1);
                /* XXX: use 32 bit mul which could be faster */
                tcg_gen_mul_tl(s->T0, s->T0, s->T1);
                gen_op_mov_reg_v(s, MO_16, R_EAX, s->T0);
                tcg_gen_mov_tl(cpu_cc_dst, s->T0);
                tcg_gen_ext16s_tl(s->tmp0, s->T0);
                tcg_gen_sub_tl(cpu_cc_src, s->T0, s->tmp0);
                tcg_gen_shri_tl(s->T0, s->T0, 16);
                gen_op_mov_reg_v(s, MO_16, R_EDX, s->T0);
                set_cc_op(s, CC_OP_MULW);
                break;
            default:
            case MO_32:
                tcg_gen_trunc_tl_i32(s->tmp2_i32, s->T0);
                tcg_gen_trunc_tl_i32(s->tmp3_i32, cpu_regs[R_EAX]);
                tcg_gen_muls2_i32(s->tmp2_i32, s->tmp3_i32,
                                  s->tmp2_i32, s->tmp3_i32);
                tcg_gen_extu_i32_tl(cpu_regs[R_EAX], s->tmp2_i32);
                tcg_gen_extu_i32_tl(cpu_regs[R_EDX], s->tmp3_i32);
                tcg_gen_sari_i32(s->tmp2_i32, s->tmp2_i32, 31);
                tcg_gen_mov_tl(cpu_cc_dst, cpu_regs[R_EAX]);
                tcg_gen_sub_i32(s->tmp2_i32, s->tmp2_i32, s->tmp3_i32);
                tcg_gen_extu_i32_tl(cpu_cc_src, s->tmp2_i32);
                set_cc_op(s, CC_OP_MULL);
                break;
#ifdef TARGET_X86_64
            case MO_64:
                tcg_gen_muls2_i64(cpu_regs[R_EAX], cpu_regs[R_EDX],
                                  s->T0, cpu_regs[R_EAX]);
                tcg_gen_mov_tl(cpu_cc_dst, cpu_regs[R_EAX]);
                tcg_gen_sari_tl(cpu_cc_src, cpu_regs[R_EAX], 63);
                tcg_gen_sub_tl(cpu_cc_src, cpu_cc_src, cpu_regs[R_EDX]);
                set_cc_op(s, CC_OP_MULQ);
                break;
#endif
            }
            break;
        case 6: /* div */
            switch(ot) {
            case MO_8:
                gen_helper_divb_AL(cpu_env, s->T0);
                break;
            case MO_16:
                gen_helper_divw_AX(cpu_env, s->T0);
                break;
            default:
            case MO_32:
                gen_helper_divl_EAX(cpu_env, s->T0);
                break;
#ifdef TARGET_X86_64
            case MO_64:
                gen_helper_divq_EAX(cpu_env, s->T0);
                break;
#endif
            }
            break;
        case 7: /* idiv */
            switch(ot) {
            case MO_8:
                gen_helper_idivb_AL(cpu_env, s->T0);
                break;
            case MO_16:
                gen_helper_idivw_AX(cpu_env, s->T0);
                break;
            default:
            case MO_32:
                gen_helper_idivl_EAX(cpu_env, s->T0);
                break;
#ifdef TARGET_X86_64
            case MO_64:
                gen_helper_idivq_EAX(cpu_env, s->T0);
                break;
#endif
            }
            break;
        default:
            goto unknown_op;
        }
        break;

    case 0xfe: /* GRP4 */
    case 0xff: /* GRP5 */
        ot = mo_b_d(b, dflag);

        modrm = x86_ldub_code(env, s);
        mod = (modrm >> 6) & 3;
        rm = (modrm & 7) | REX_B(s);
        op = (modrm >> 3) & 7;
        if (op >= 2 && b == 0xfe) {
            goto unknown_op;
        }
        if (CODE64(s)) {
            if (op == 2 || op == 4) {
                /* operand size for jumps is 64 bit */
                ot = MO_64;
            } else if (op == 3 || op == 5) {
                ot = dflag != MO_16 ? MO_32 + REX_W(s) : MO_16;
            } else if (op == 6) {
                /* default push size is 64 bit */
                ot = mo_pushpop(s, dflag);
            }
        }
        if (mod != 3) {
            gen_lea_modrm(env, s, modrm);
            if (op >= 2 && op != 3 && op != 5)
                gen_op_ld_v(s, ot, s->T0, s->A0);
        } else {
            gen_op_mov_v_reg(s, ot, s->T0, rm);
        }

        switch(op) {
        case 0: /* inc Ev */
            if (mod != 3)
                opreg = OR_TMP0;
            else
                opreg = rm;
            gen_inc(s, ot, opreg, 1);
            break;
        case 1: /* dec Ev */
            if (mod != 3)
                opreg = OR_TMP0;
            else
                opreg = rm;
            gen_inc(s, ot, opreg, -1);
            break;
        case 2: /* call Ev */
            /* XXX: optimize if memory (no 'and' is necessary) */
            if (dflag == MO_16) {
                tcg_gen_ext16u_tl(s->T0, s->T0);
            }
            gen_push_v(s, eip_next_tl(s));
            gen_op_jmp_v(s, s->T0);
            gen_bnd_jmp(s);
            s->base.is_jmp = DISAS_JUMP;
            break;
        case 3: /* lcall Ev */
            if (mod == 3) {
                goto illegal_op;
            }
            gen_op_ld_v(s, ot, s->T1, s->A0);
            gen_add_A0_im(s, 1 << ot);
            gen_op_ld_v(s, MO_16, s->T0, s->A0);
        do_lcall:
            if (PE(s) && !VM86(s)) {
                tcg_gen_trunc_tl_i32(s->tmp2_i32, s->T0);
                gen_helper_lcall_protected(cpu_env, s->tmp2_i32, s->T1,
                                           tcg_constant_i32(dflag - 1),
                                           eip_next_tl(s));
            } else {
                tcg_gen_trunc_tl_i32(s->tmp2_i32, s->T0);
                tcg_gen_trunc_tl_i32(s->tmp3_i32, s->T1);
                gen_helper_lcall_real(cpu_env, s->tmp2_i32, s->tmp3_i32,
                                      tcg_constant_i32(dflag - 1),
                                      eip_next_i32(s));
            }
            s->base.is_jmp = DISAS_JUMP;
            break;
        case 4: /* jmp Ev */
            if (dflag == MO_16) {
                tcg_gen_ext16u_tl(s->T0, s->T0);
            }
            gen_op_jmp_v(s, s->T0);
            gen_bnd_jmp(s);
            s->base.is_jmp = DISAS_JUMP;
            break;
        case 5: /* ljmp Ev */
            if (mod == 3) {
                goto illegal_op;
            }
            gen_op_ld_v(s, ot, s->T1, s->A0);
            gen_add_A0_im(s, 1 << ot);
            gen_op_ld_v(s, MO_16, s->T0, s->A0);
        do_ljmp:
            if (PE(s) && !VM86(s)) {
                tcg_gen_trunc_tl_i32(s->tmp2_i32, s->T0);
                gen_helper_ljmp_protected(cpu_env, s->tmp2_i32, s->T1,
                                          eip_next_tl(s));
            } else {
                gen_op_movl_seg_T0_vm(s, R_CS);
                gen_op_jmp_v(s, s->T1);
            }
            s->base.is_jmp = DISAS_JUMP;
            break;
        case 6: /* push Ev */
            gen_push_v(s, s->T0);
            break;
        default:
            goto unknown_op;
        }
        break;

    case 0x84: /* test Ev, Gv */
    case 0x85:
        ot = mo_b_d(b, dflag);

        modrm = x86_ldub_code(env, s);
        reg = ((modrm >> 3) & 7) | REX_R(s);

        gen_ldst_modrm(env, s, modrm, ot, OR_TMP0, 0);
        gen_op_mov_v_reg(s, ot, s->T1, reg);
        gen_op_testl_T0_T1_cc(s);
        set_cc_op(s, CC_OP_LOGICB + ot);
        break;

    case 0xa8: /* test eAX, Iv */
    case 0xa9:
        ot = mo_b_d(b, dflag);
        val = insn_get(env, s, ot);

        gen_op_mov_v_reg(s, ot, s->T0, OR_EAX);
        tcg_gen_movi_tl(s->T1, val);
        gen_op_testl_T0_T1_cc(s);
        set_cc_op(s, CC_OP_LOGICB + ot);
        break;

    case 0x98: /* CWDE/CBW */
        switch (dflag) {
#ifdef TARGET_X86_64
        case MO_64:
            gen_op_mov_v_reg(s, MO_32, s->T0, R_EAX);
            tcg_gen_ext32s_tl(s->T0, s->T0);
            gen_op_mov_reg_v(s, MO_64, R_EAX, s->T0);
            break;
#endif
        case MO_32:
            gen_op_mov_v_reg(s, MO_16, s->T0, R_EAX);
            tcg_gen_ext16s_tl(s->T0, s->T0);
            gen_op_mov_reg_v(s, MO_32, R_EAX, s->T0);
            break;
        case MO_16:
            gen_op_mov_v_reg(s, MO_8, s->T0, R_EAX);
            tcg_gen_ext8s_tl(s->T0, s->T0);
            gen_op_mov_reg_v(s, MO_16, R_EAX, s->T0);
            break;
        default:
            tcg_abort();
        }
        break;
    case 0x99: /* CDQ/CWD */
        switch (dflag) {
#ifdef TARGET_X86_64
        case MO_64:
            gen_op_mov_v_reg(s, MO_64, s->T0, R_EAX);
            tcg_gen_sari_tl(s->T0, s->T0, 63);
            gen_op_mov_reg_v(s, MO_64, R_EDX, s->T0);
            break;
#endif
        case MO_32:
            gen_op_mov_v_reg(s, MO_32, s->T0, R_EAX);
            tcg_gen_ext32s_tl(s->T0, s->T0);
            tcg_gen_sari_tl(s->T0, s->T0, 31);
            gen_op_mov_reg_v(s, MO_32, R_EDX, s->T0);
            break;
        case MO_16:
            gen_op_mov_v_reg(s, MO_16, s->T0, R_EAX);
            tcg_gen_ext16s_tl(s->T0, s->T0);
            tcg_gen_sari_tl(s->T0, s->T0, 15);
            gen_op_mov_reg_v(s, MO_16, R_EDX, s->T0);
            break;
        default:
            tcg_abort();
        }
        break;
    case 0x1af: /* imul Gv, Ev */
    case 0x69: /* imul Gv, Ev, I */
    case 0x6b:
        ot = dflag;
        modrm = x86_ldub_code(env, s);
        reg = ((modrm >> 3) & 7) | REX_R(s);
        if (b == 0x69)
            s->rip_offset = insn_const_size(ot);
        else if (b == 0x6b)
            s->rip_offset = 1;
        gen_ldst_modrm(env, s, modrm, ot, OR_TMP0, 0);
        if (b == 0x69) {
            val = insn_get(env, s, ot);
            tcg_gen_movi_tl(s->T1, val);
        } else if (b == 0x6b) {
            val = (int8_t)insn_get(env, s, MO_8);
            tcg_gen_movi_tl(s->T1, val);
        } else {
            gen_op_mov_v_reg(s, ot, s->T1, reg);
        }
        switch (ot) {
#ifdef TARGET_X86_64
        case MO_64:
            tcg_gen_muls2_i64(cpu_regs[reg], s->T1, s->T0, s->T1);
            tcg_gen_mov_tl(cpu_cc_dst, cpu_regs[reg]);
            tcg_gen_sari_tl(cpu_cc_src, cpu_cc_dst, 63);
            tcg_gen_sub_tl(cpu_cc_src, cpu_cc_src, s->T1);
            break;
#endif
        case MO_32:
            tcg_gen_trunc_tl_i32(s->tmp2_i32, s->T0);
            tcg_gen_trunc_tl_i32(s->tmp3_i32, s->T1);
            tcg_gen_muls2_i32(s->tmp2_i32, s->tmp3_i32,
                              s->tmp2_i32, s->tmp3_i32);
            tcg_gen_extu_i32_tl(cpu_regs[reg], s->tmp2_i32);
            tcg_gen_sari_i32(s->tmp2_i32, s->tmp2_i32, 31);
            tcg_gen_mov_tl(cpu_cc_dst, cpu_regs[reg]);
            tcg_gen_sub_i32(s->tmp2_i32, s->tmp2_i32, s->tmp3_i32);
            tcg_gen_extu_i32_tl(cpu_cc_src, s->tmp2_i32);
            break;
        default:
            tcg_gen_ext16s_tl(s->T0, s->T0);
            tcg_gen_ext16s_tl(s->T1, s->T1);
            /* XXX: use 32 bit mul which could be faster */
            tcg_gen_mul_tl(s->T0, s->T0, s->T1);
            tcg_gen_mov_tl(cpu_cc_dst, s->T0);
            tcg_gen_ext16s_tl(s->tmp0, s->T0);
            tcg_gen_sub_tl(cpu_cc_src, s->T0, s->tmp0);
            gen_op_mov_reg_v(s, ot, reg, s->T0);
            break;
        }
        set_cc_op(s, CC_OP_MULB + ot);
        break;
    case 0x1c0:
    case 0x1c1: /* xadd Ev, Gv */
        ot = mo_b_d(b, dflag);
        modrm = x86_ldub_code(env, s);
        reg = ((modrm >> 3) & 7) | REX_R(s);
        mod = (modrm >> 6) & 3;
        gen_op_mov_v_reg(s, ot, s->T0, reg);
        if (mod == 3) {
            rm = (modrm & 7) | REX_B(s);
            gen_op_mov_v_reg(s, ot, s->T1, rm);
            tcg_gen_add_tl(s->T0, s->T0, s->T1);
            gen_op_mov_reg_v(s, ot, reg, s->T1);
            gen_op_mov_reg_v(s, ot, rm, s->T0);
        } else {
            gen_lea_modrm(env, s, modrm);
            if (s->prefix & PREFIX_LOCK) {
                tcg_gen_atomic_fetch_add_tl(s->T1, s->A0, s->T0,
                                            s->mem_index, ot | MO_LE);
                tcg_gen_add_tl(s->T0, s->T0, s->T1);
            } else {
                gen_op_ld_v(s, ot, s->T1, s->A0);
                tcg_gen_add_tl(s->T0, s->T0, s->T1);
                gen_op_st_v(s, ot, s->T0, s->A0);
            }
            gen_op_mov_reg_v(s, ot, reg, s->T1);
        }
        gen_op_update2_cc(s);
        set_cc_op(s, CC_OP_ADDB + ot);
        break;
    case 0x1b0:
    case 0x1b1: /* cmpxchg Ev, Gv */
        {
            TCGv oldv, newv, cmpv, dest;

            ot = mo_b_d(b, dflag);
            modrm = x86_ldub_code(env, s);
            reg = ((modrm >> 3) & 7) | REX_R(s);
            mod = (modrm >> 6) & 3;
            oldv = tcg_temp_new();
            newv = tcg_temp_new();
            cmpv = tcg_temp_new();
            gen_op_mov_v_reg(s, ot, newv, reg);
            tcg_gen_mov_tl(cmpv, cpu_regs[R_EAX]);
            gen_extu(ot, cmpv);
            if (s->prefix & PREFIX_LOCK) {
                if (mod == 3) {
                    goto illegal_op;
                }
                gen_lea_modrm(env, s, modrm);
                tcg_gen_atomic_cmpxchg_tl(oldv, s->A0, cmpv, newv,
                                          s->mem_index, ot | MO_LE);
            } else {
                if (mod == 3) {
                    rm = (modrm & 7) | REX_B(s);
                    gen_op_mov_v_reg(s, ot, oldv, rm);
                    gen_extu(ot, oldv);

                    /*
                     * Unlike the memory case, where "the destination operand receives
                     * a write cycle without regard to the result of the comparison",
                     * rm must not be touched altogether if the write fails, including
                     * not zero-extending it on 64-bit processors.  So, precompute
                     * the result of a successful writeback and perform the movcond
                     * directly on cpu_regs.  Also need to write accumulator first, in
                     * case rm is part of RAX too.
                     */
                    dest = gen_op_deposit_reg_v(s, ot, rm, newv, newv);
                    tcg_gen_movcond_tl(TCG_COND_EQ, dest, oldv, cmpv, newv, dest);
                } else {
                    gen_lea_modrm(env, s, modrm);
                    gen_op_ld_v(s, ot, oldv, s->A0);

                    /*
                     * Perform an unconditional store cycle like physical cpu;
                     * must be before changing accumulator to ensure
                     * idempotency if the store faults and the instruction
                     * is restarted
                     */
                    tcg_gen_movcond_tl(TCG_COND_EQ, newv, oldv, cmpv, newv, oldv);
                    gen_op_st_v(s, ot, newv, s->A0);
                }
            }
	    /*
	     * Write EAX only if the cmpxchg fails; reuse newv as the destination,
	     * since it's dead here.
	     */
            dest = gen_op_deposit_reg_v(s, ot, R_EAX, newv, oldv);
            tcg_gen_movcond_tl(TCG_COND_EQ, dest, oldv, cmpv, dest, newv);
            tcg_gen_mov_tl(cpu_cc_src, oldv);
            tcg_gen_mov_tl(s->cc_srcT, cmpv);
            tcg_gen_sub_tl(cpu_cc_dst, cmpv, oldv);
            set_cc_op(s, CC_OP_SUBB + ot);
            tcg_temp_free(oldv);
            tcg_temp_free(newv);
            tcg_temp_free(cmpv);
        }
        break;
    case 0x1c7: /* cmpxchg8b */
        modrm = x86_ldub_code(env, s);
        mod = (modrm >> 6) & 3;
        switch ((modrm >> 3) & 7) {
        case 1: /* CMPXCHG8, CMPXCHG16 */
            if (mod == 3) {
                goto illegal_op;
            }
#ifdef TARGET_X86_64
            if (dflag == MO_64) {
                if (!(s->cpuid_ext_features & CPUID_EXT_CX16)) {
                    goto illegal_op;
                }
                gen_lea_modrm(env, s, modrm);
                if ((s->prefix & PREFIX_LOCK) &&
                    (tb_cflags(s->base.tb) & CF_PARALLEL)) {
                    gen_helper_cmpxchg16b(cpu_env, s->A0);
                } else {
                    gen_helper_cmpxchg16b_unlocked(cpu_env, s->A0);
                }
                set_cc_op(s, CC_OP_EFLAGS);
                break;
            }
#endif        
            if (!(s->cpuid_features & CPUID_CX8)) {
                goto illegal_op;
            }
            gen_lea_modrm(env, s, modrm);
            if ((s->prefix & PREFIX_LOCK) &&
                (tb_cflags(s->base.tb) & CF_PARALLEL)) {
                gen_helper_cmpxchg8b(cpu_env, s->A0);
            } else {
                gen_helper_cmpxchg8b_unlocked(cpu_env, s->A0);
            }
            set_cc_op(s, CC_OP_EFLAGS);
            break;

        case 7: /* RDSEED */
        case 6: /* RDRAND */
            if (mod != 3 ||
                (s->prefix & (PREFIX_LOCK | PREFIX_REPZ | PREFIX_REPNZ)) ||
                !(s->cpuid_ext_features & CPUID_EXT_RDRAND)) {
                goto illegal_op;
            }
            if (tb_cflags(s->base.tb) & CF_USE_ICOUNT) {
                gen_io_start();
                s->base.is_jmp = DISAS_TOO_MANY;
            }
            gen_helper_rdrand(s->T0, cpu_env);
            rm = (modrm & 7) | REX_B(s);
            gen_op_mov_reg_v(s, dflag, rm, s->T0);
            set_cc_op(s, CC_OP_EFLAGS);
            break;

        default:
            goto illegal_op;
        }
        break;

        /**************************/
        /* push/pop */
    case 0x50 ... 0x57: /* push */
        gen_op_mov_v_reg(s, MO_32, s->T0, (b & 7) | REX_B(s));
        gen_push_v(s, s->T0);
        break;
    case 0x58 ... 0x5f: /* pop */
        ot = gen_pop_T0(s);
        /* NOTE: order is important for pop %sp */
        gen_pop_update(s, ot);
        gen_op_mov_reg_v(s, ot, (b & 7) | REX_B(s), s->T0);
        break;
    case 0x60: /* pusha */
        if (CODE64(s))
            goto illegal_op;
        gen_pusha(s);
        break;
    case 0x61: /* popa */
        if (CODE64(s))
            goto illegal_op;
        gen_popa(s);
        break;
    case 0x68: /* push Iv */
    case 0x6a:
        ot = mo_pushpop(s, dflag);
        if (b == 0x68)
            val = insn_get(env, s, ot);
        else
            val = (int8_t)insn_get(env, s, MO_8);
        tcg_gen_movi_tl(s->T0, val);
        gen_push_v(s, s->T0);
        break;
    case 0x8f: /* pop Ev */
        modrm = x86_ldub_code(env, s);
        mod = (modrm >> 6) & 3;
        ot = gen_pop_T0(s);
        if (mod == 3) {
            /* NOTE: order is important for pop %sp */
            gen_pop_update(s, ot);
            rm = (modrm & 7) | REX_B(s);
            gen_op_mov_reg_v(s, ot, rm, s->T0);
        } else {
            /* NOTE: order is important too for MMU exceptions */
            s->popl_esp_hack = 1 << ot;
            gen_ldst_modrm(env, s, modrm, ot, OR_TMP0, 1);
            s->popl_esp_hack = 0;
            gen_pop_update(s, ot);
        }
        break;
    case 0xc8: /* enter */
        {
            int level;
            val = x86_lduw_code(env, s);
            level = x86_ldub_code(env, s);
            gen_enter(s, val, level);
        }
        break;
    case 0xc9: /* leave */
        gen_leave(s);
        break;
    case 0x06: /* push es */
    case 0x0e: /* push cs */
    case 0x16: /* push ss */
    case 0x1e: /* push ds */
        if (CODE64(s))
            goto illegal_op;
        gen_op_movl_T0_seg(s, b >> 3);
        gen_push_v(s, s->T0);
        break;
    case 0x1a0: /* push fs */
    case 0x1a8: /* push gs */
        gen_op_movl_T0_seg(s, (b >> 3) & 7);
        gen_push_v(s, s->T0);
        break;
    case 0x07: /* pop es */
    case 0x17: /* pop ss */
    case 0x1f: /* pop ds */
        if (CODE64(s))
            goto illegal_op;
        reg = b >> 3;
        ot = gen_pop_T0(s);
        gen_movl_seg_T0(s, reg);
        gen_pop_update(s, ot);
        break;
    case 0x1a1: /* pop fs */
    case 0x1a9: /* pop gs */
        ot = gen_pop_T0(s);
        gen_movl_seg_T0(s, (b >> 3) & 7);
        gen_pop_update(s, ot);
        break;

        /**************************/
        /* mov */
    case 0x88:
    case 0x89: /* mov Gv, Ev */
        ot = mo_b_d(b, dflag);
        modrm = x86_ldub_code(env, s);
        reg = ((modrm >> 3) & 7) | REX_R(s);

        /* generate a generic store */
        gen_ldst_modrm(env, s, modrm, ot, reg, 1);
        break;
    case 0xc6:
    case 0xc7: /* mov Ev, Iv */
        ot = mo_b_d(b, dflag);
        modrm = x86_ldub_code(env, s);
        mod = (modrm >> 6) & 3;
        if (mod != 3) {
            s->rip_offset = insn_const_size(ot);
            gen_lea_modrm(env, s, modrm);
        }
        val = insn_get(env, s, ot);
        tcg_gen_movi_tl(s->T0, val);
        if (mod != 3) {
            gen_op_st_v(s, ot, s->T0, s->A0);
        } else {
            gen_op_mov_reg_v(s, ot, (modrm & 7) | REX_B(s), s->T0);
        }
        break;
    case 0x8a:
    case 0x8b: /* mov Ev, Gv */
        ot = mo_b_d(b, dflag);
        modrm = x86_ldub_code(env, s);
        reg = ((modrm >> 3) & 7) | REX_R(s);

        gen_ldst_modrm(env, s, modrm, ot, OR_TMP0, 0);
        gen_op_mov_reg_v(s, ot, reg, s->T0);
        break;
    case 0x8e: /* mov seg, Gv */
        modrm = x86_ldub_code(env, s);
        reg = (modrm >> 3) & 7;
        if (reg >= 6 || reg == R_CS)
            goto illegal_op;
        gen_ldst_modrm(env, s, modrm, MO_16, OR_TMP0, 0);
        gen_movl_seg_T0(s, reg);
        break;
    case 0x8c: /* mov Gv, seg */
        modrm = x86_ldub_code(env, s);
        reg = (modrm >> 3) & 7;
        mod = (modrm >> 6) & 3;
        if (reg >= 6)
            goto illegal_op;
        gen_op_movl_T0_seg(s, reg);
        ot = mod == 3 ? dflag : MO_16;
        gen_ldst_modrm(env, s, modrm, ot, OR_TMP0, 1);
        break;

    case 0x1b6: /* movzbS Gv, Eb */
    case 0x1b7: /* movzwS Gv, Eb */
    case 0x1be: /* movsbS Gv, Eb */
    case 0x1bf: /* movswS Gv, Eb */
        {
            MemOp d_ot;
            MemOp s_ot;

            /* d_ot is the size of destination */
            d_ot = dflag;
            /* ot is the size of source */
            ot = (b & 1) + MO_8;
            /* s_ot is the sign+size of source */
            s_ot = b & 8 ? MO_SIGN | ot : ot;

            modrm = x86_ldub_code(env, s);
            reg = ((modrm >> 3) & 7) | REX_R(s);
            mod = (modrm >> 6) & 3;
            rm = (modrm & 7) | REX_B(s);

            if (mod == 3) {
                if (s_ot == MO_SB && byte_reg_is_xH(s, rm)) {
                    tcg_gen_sextract_tl(s->T0, cpu_regs[rm - 4], 8, 8);
                } else {
                    gen_op_mov_v_reg(s, ot, s->T0, rm);
                    switch (s_ot) {
                    case MO_UB:
                        tcg_gen_ext8u_tl(s->T0, s->T0);
                        break;
                    case MO_SB:
                        tcg_gen_ext8s_tl(s->T0, s->T0);
                        break;
                    case MO_UW:
                        tcg_gen_ext16u_tl(s->T0, s->T0);
                        break;
                    default:
                    case MO_SW:
                        tcg_gen_ext16s_tl(s->T0, s->T0);
                        break;
                    }
                }
                gen_op_mov_reg_v(s, d_ot, reg, s->T0);
            } else {
                gen_lea_modrm(env, s, modrm);
                gen_op_ld_v(s, s_ot, s->T0, s->A0);
                gen_op_mov_reg_v(s, d_ot, reg, s->T0);
            }
        }
        break;

    case 0x8d: /* lea */
        modrm = x86_ldub_code(env, s);
        mod = (modrm >> 6) & 3;
        if (mod == 3)
            goto illegal_op;
        reg = ((modrm >> 3) & 7) | REX_R(s);
        {
            AddressParts a = gen_lea_modrm_0(env, s, modrm);
            TCGv ea = gen_lea_modrm_1(s, a, false);
            gen_lea_v_seg(s, s->aflag, ea, -1, -1);
            gen_op_mov_reg_v(s, dflag, reg, s->A0);
        }
        break;

    case 0xa0: /* mov EAX, Ov */
    case 0xa1:
    case 0xa2: /* mov Ov, EAX */
    case 0xa3:
        {
            target_ulong offset_addr;

            ot = mo_b_d(b, dflag);
            offset_addr = insn_get_addr(env, s, s->aflag);
            tcg_gen_movi_tl(s->A0, offset_addr);
            gen_add_A0_ds_seg(s);
            if ((b & 2) == 0) {
                gen_op_ld_v(s, ot, s->T0, s->A0);
                gen_op_mov_reg_v(s, ot, R_EAX, s->T0);
            } else {
                gen_op_mov_v_reg(s, ot, s->T0, R_EAX);
                gen_op_st_v(s, ot, s->T0, s->A0);
            }
        }
        break;
    case 0xd7: /* xlat */
        tcg_gen_mov_tl(s->A0, cpu_regs[R_EBX]);
        tcg_gen_ext8u_tl(s->T0, cpu_regs[R_EAX]);
        tcg_gen_add_tl(s->A0, s->A0, s->T0);
        gen_extu(s->aflag, s->A0);
        gen_add_A0_ds_seg(s);
        gen_op_ld_v(s, MO_8, s->T0, s->A0);
        gen_op_mov_reg_v(s, MO_8, R_EAX, s->T0);
        break;
    case 0xb0 ... 0xb7: /* mov R, Ib */
        val = insn_get(env, s, MO_8);
        tcg_gen_movi_tl(s->T0, val);
        gen_op_mov_reg_v(s, MO_8, (b & 7) | REX_B(s), s->T0);
        break;
    case 0xb8 ... 0xbf: /* mov R, Iv */
#ifdef TARGET_X86_64
        if (dflag == MO_64) {
            uint64_t tmp;
            /* 64 bit case */
            tmp = x86_ldq_code(env, s);
            reg = (b & 7) | REX_B(s);
            tcg_gen_movi_tl(s->T0, tmp);
            gen_op_mov_reg_v(s, MO_64, reg, s->T0);
        } else
#endif
        {
            ot = dflag;
            val = insn_get(env, s, ot);
            reg = (b & 7) | REX_B(s);
            tcg_gen_movi_tl(s->T0, val);
            gen_op_mov_reg_v(s, ot, reg, s->T0);
        }
        break;

    case 0x91 ... 0x97: /* xchg R, EAX */
    do_xchg_reg_eax:
        ot = dflag;
        reg = (b & 7) | REX_B(s);
        rm = R_EAX;
        goto do_xchg_reg;
    case 0x86:
    case 0x87: /* xchg Ev, Gv */
        ot = mo_b_d(b, dflag);
        modrm = x86_ldub_code(env, s);
        reg = ((modrm >> 3) & 7) | REX_R(s);
        mod = (modrm >> 6) & 3;
        if (mod == 3) {
            rm = (modrm & 7) | REX_B(s);
        do_xchg_reg:
            gen_op_mov_v_reg(s, ot, s->T0, reg);
            gen_op_mov_v_reg(s, ot, s->T1, rm);
            gen_op_mov_reg_v(s, ot, rm, s->T0);
            gen_op_mov_reg_v(s, ot, reg, s->T1);
        } else {
            gen_lea_modrm(env, s, modrm);
            gen_op_mov_v_reg(s, ot, s->T0, reg);
            /* for xchg, lock is implicit */
            tcg_gen_atomic_xchg_tl(s->T1, s->A0, s->T0,
                                   s->mem_index, ot | MO_LE);
            gen_op_mov_reg_v(s, ot, reg, s->T1);
        }
        break;
    case 0xc4: /* les Gv */
        /* In CODE64 this is VEX3; see above.  */
        op = R_ES;
        goto do_lxx;
    case 0xc5: /* lds Gv */
        /* In CODE64 this is VEX2; see above.  */
        op = R_DS;
        goto do_lxx;
    case 0x1b2: /* lss Gv */
        op = R_SS;
        goto do_lxx;
    case 0x1b4: /* lfs Gv */
        op = R_FS;
        goto do_lxx;
    case 0x1b5: /* lgs Gv */
        op = R_GS;
    do_lxx:
        ot = dflag != MO_16 ? MO_32 : MO_16;
        modrm = x86_ldub_code(env, s);
        reg = ((modrm >> 3) & 7) | REX_R(s);
        mod = (modrm >> 6) & 3;
        if (mod == 3)
            goto illegal_op;
        gen_lea_modrm(env, s, modrm);
        gen_op_ld_v(s, ot, s->T1, s->A0);
        gen_add_A0_im(s, 1 << ot);
        /* load the segment first to handle exceptions properly */
        gen_op_ld_v(s, MO_16, s->T0, s->A0);
        gen_movl_seg_T0(s, op);
        /* then put the data */
        gen_op_mov_reg_v(s, ot, reg, s->T1);
        break;

        /************************/
        /* shifts */
    case 0xc0:
    case 0xc1:
        /* shift Ev,Ib */
        shift = 2;
    grp2:
        {
            ot = mo_b_d(b, dflag);
            modrm = x86_ldub_code(env, s);
            mod = (modrm >> 6) & 3;
            op = (modrm >> 3) & 7;

            if (mod != 3) {
                if (shift == 2) {
                    s->rip_offset = 1;
                }
                gen_lea_modrm(env, s, modrm);
                opreg = OR_TMP0;
            } else {
                opreg = (modrm & 7) | REX_B(s);
            }

            /* simpler op */
            if (shift == 0) {
                gen_shift(s, op, ot, opreg, OR_ECX);
            } else {
                if (shift == 2) {
                    shift = x86_ldub_code(env, s);
                }
                gen_shifti(s, op, ot, opreg, shift);
            }
        }
        break;
    case 0xd0:
    case 0xd1:
        /* shift Ev,1 */
        shift = 1;
        goto grp2;
    case 0xd2:
    case 0xd3:
        /* shift Ev,cl */
        shift = 0;
        goto grp2;

    case 0x1a4: /* shld imm */
        op = 0;
        shift = 1;
        goto do_shiftd;
    case 0x1a5: /* shld cl */
        op = 0;
        shift = 0;
        goto do_shiftd;
    case 0x1ac: /* shrd imm */
        op = 1;
        shift = 1;
        goto do_shiftd;
    case 0x1ad: /* shrd cl */
        op = 1;
        shift = 0;
    do_shiftd:
        ot = dflag;
        modrm = x86_ldub_code(env, s);
        mod = (modrm >> 6) & 3;
        rm = (modrm & 7) | REX_B(s);
        reg = ((modrm >> 3) & 7) | REX_R(s);
        if (mod != 3) {
            gen_lea_modrm(env, s, modrm);
            opreg = OR_TMP0;
        } else {
            opreg = rm;
        }
        gen_op_mov_v_reg(s, ot, s->T1, reg);

        if (shift) {
            TCGv imm = tcg_const_tl(x86_ldub_code(env, s));
            gen_shiftd_rm_T1(s, ot, opreg, op, imm);
            tcg_temp_free(imm);
        } else {
            gen_shiftd_rm_T1(s, ot, opreg, op, cpu_regs[R_ECX]);
        }
        break;

        /************************/
        /* floats */
    case 0xd8 ... 0xdf:
        {
            bool update_fip = true;

            if (s->flags & (HF_EM_MASK | HF_TS_MASK)) {
                /* if CR0.EM or CR0.TS are set, generate an FPU exception */
                /* XXX: what to do if illegal op ? */
                gen_exception(s, EXCP07_PREX);
                break;
            }
            modrm = x86_ldub_code(env, s);
            mod = (modrm >> 6) & 3;
            rm = modrm & 7;
            op = ((b & 7) << 3) | ((modrm >> 3) & 7);
            if (mod != 3) {
                /* memory op */
                AddressParts a = gen_lea_modrm_0(env, s, modrm);
                TCGv ea = gen_lea_modrm_1(s, a, false);
                TCGv last_addr = tcg_temp_new();
                bool update_fdp = true;

                tcg_gen_mov_tl(last_addr, ea);
                gen_lea_v_seg(s, s->aflag, ea, a.def_seg, s->override);

                switch (op) {
                case 0x00 ... 0x07: /* fxxxs */
                case 0x10 ... 0x17: /* fixxxl */
                case 0x20 ... 0x27: /* fxxxl */
                case 0x30 ... 0x37: /* fixxx */
                    {
                        int op1;
                        op1 = op & 7;

                        switch (op >> 4) {
                        case 0:
                            tcg_gen_qemu_ld_i32(s->tmp2_i32, s->A0,
                                                s->mem_index, MO_LEUL);
                            gen_helper_flds_FT0(cpu_env, s->tmp2_i32);
                            break;
                        case 1:
                            tcg_gen_qemu_ld_i32(s->tmp2_i32, s->A0,
                                                s->mem_index, MO_LEUL);
                            gen_helper_fildl_FT0(cpu_env, s->tmp2_i32);
                            break;
                        case 2:
                            tcg_gen_qemu_ld_i64(s->tmp1_i64, s->A0,
                                                s->mem_index, MO_LEUQ);
                            gen_helper_fldl_FT0(cpu_env, s->tmp1_i64);
                            break;
                        case 3:
                        default:
                            tcg_gen_qemu_ld_i32(s->tmp2_i32, s->A0,
                                                s->mem_index, MO_LESW);
                            gen_helper_fildl_FT0(cpu_env, s->tmp2_i32);
                            break;
                        }

                        gen_helper_fp_arith_ST0_FT0(op1);
                        if (op1 == 3) {
                            /* fcomp needs pop */
                            gen_helper_fpop(cpu_env);
                        }
                    }
                    break;
                case 0x08: /* flds */
                case 0x0a: /* fsts */
                case 0x0b: /* fstps */
                case 0x18 ... 0x1b: /* fildl, fisttpl, fistl, fistpl */
                case 0x28 ... 0x2b: /* fldl, fisttpll, fstl, fstpl */
                case 0x38 ... 0x3b: /* filds, fisttps, fists, fistps */
                    switch (op & 7) {
                    case 0:
                        switch (op >> 4) {
                        case 0:
                            tcg_gen_qemu_ld_i32(s->tmp2_i32, s->A0,
                                                s->mem_index, MO_LEUL);
                            gen_helper_flds_ST0(cpu_env, s->tmp2_i32);
                            break;
                        case 1:
                            tcg_gen_qemu_ld_i32(s->tmp2_i32, s->A0,
                                                s->mem_index, MO_LEUL);
                            gen_helper_fildl_ST0(cpu_env, s->tmp2_i32);
                            break;
                        case 2:
                            tcg_gen_qemu_ld_i64(s->tmp1_i64, s->A0,
                                                s->mem_index, MO_LEUQ);
                            gen_helper_fldl_ST0(cpu_env, s->tmp1_i64);
                            break;
                        case 3:
                        default:
                            tcg_gen_qemu_ld_i32(s->tmp2_i32, s->A0,
                                                s->mem_index, MO_LESW);
                            gen_helper_fildl_ST0(cpu_env, s->tmp2_i32);
                            break;
                        }
                        break;
                    case 1:
                        /* XXX: the corresponding CPUID bit must be tested ! */
                        switch (op >> 4) {
                        case 1:
                            gen_helper_fisttl_ST0(s->tmp2_i32, cpu_env);
                            tcg_gen_qemu_st_i32(s->tmp2_i32, s->A0,
                                                s->mem_index, MO_LEUL);
                            break;
                        case 2:
                            gen_helper_fisttll_ST0(s->tmp1_i64, cpu_env);
                            tcg_gen_qemu_st_i64(s->tmp1_i64, s->A0,
                                                s->mem_index, MO_LEUQ);
                            break;
                        case 3:
                        default:
                            gen_helper_fistt_ST0(s->tmp2_i32, cpu_env);
                            tcg_gen_qemu_st_i32(s->tmp2_i32, s->A0,
                                                s->mem_index, MO_LEUW);
                            break;
                        }
                        gen_helper_fpop(cpu_env);
                        break;
                    default:
                        switch (op >> 4) {
                        case 0:
                            gen_helper_fsts_ST0(s->tmp2_i32, cpu_env);
                            tcg_gen_qemu_st_i32(s->tmp2_i32, s->A0,
                                                s->mem_index, MO_LEUL);
                            break;
                        case 1:
                            gen_helper_fistl_ST0(s->tmp2_i32, cpu_env);
                            tcg_gen_qemu_st_i32(s->tmp2_i32, s->A0,
                                                s->mem_index, MO_LEUL);
                            break;
                        case 2:
                            gen_helper_fstl_ST0(s->tmp1_i64, cpu_env);
                            tcg_gen_qemu_st_i64(s->tmp1_i64, s->A0,
                                                s->mem_index, MO_LEUQ);
                            break;
                        case 3:
                        default:
                            gen_helper_fist_ST0(s->tmp2_i32, cpu_env);
                            tcg_gen_qemu_st_i32(s->tmp2_i32, s->A0,
                                                s->mem_index, MO_LEUW);
                            break;
                        }
                        if ((op & 7) == 3) {
                            gen_helper_fpop(cpu_env);
                        }
                        break;
                    }
                    break;
                case 0x0c: /* fldenv mem */
                    gen_helper_fldenv(cpu_env, s->A0,
                                      tcg_const_i32(dflag - 1));
                    update_fip = update_fdp = false;
                    break;
                case 0x0d: /* fldcw mem */
                    tcg_gen_qemu_ld_i32(s->tmp2_i32, s->A0,
                                        s->mem_index, MO_LEUW);
                    gen_helper_fldcw(cpu_env, s->tmp2_i32);
                    update_fip = update_fdp = false;
                    break;
                case 0x0e: /* fnstenv mem */
                    gen_helper_fstenv(cpu_env, s->A0,
                                      tcg_const_i32(dflag - 1));
                    update_fip = update_fdp = false;
                    break;
                case 0x0f: /* fnstcw mem */
                    gen_helper_fnstcw(s->tmp2_i32, cpu_env);
                    tcg_gen_qemu_st_i32(s->tmp2_i32, s->A0,
                                        s->mem_index, MO_LEUW);
                    update_fip = update_fdp = false;
                    break;
                case 0x1d: /* fldt mem */
                    gen_helper_fldt_ST0(cpu_env, s->A0);
                    break;
                case 0x1f: /* fstpt mem */
                    gen_helper_fstt_ST0(cpu_env, s->A0);
                    gen_helper_fpop(cpu_env);
                    break;
                case 0x2c: /* frstor mem */
                    gen_helper_frstor(cpu_env, s->A0,
                                      tcg_const_i32(dflag - 1));
                    update_fip = update_fdp = false;
                    break;
                case 0x2e: /* fnsave mem */
                    gen_helper_fsave(cpu_env, s->A0,
                                     tcg_const_i32(dflag - 1));
                    update_fip = update_fdp = false;
                    break;
                case 0x2f: /* fnstsw mem */
                    gen_helper_fnstsw(s->tmp2_i32, cpu_env);
                    tcg_gen_qemu_st_i32(s->tmp2_i32, s->A0,
                                        s->mem_index, MO_LEUW);
                    update_fip = update_fdp = false;
                    break;
                case 0x3c: /* fbld */
                    gen_helper_fbld_ST0(cpu_env, s->A0);
                    break;
                case 0x3e: /* fbstp */
                    gen_helper_fbst_ST0(cpu_env, s->A0);
                    gen_helper_fpop(cpu_env);
                    break;
                case 0x3d: /* fildll */
                    tcg_gen_qemu_ld_i64(s->tmp1_i64, s->A0,
                                        s->mem_index, MO_LEUQ);
                    gen_helper_fildll_ST0(cpu_env, s->tmp1_i64);
                    break;
                case 0x3f: /* fistpll */
                    gen_helper_fistll_ST0(s->tmp1_i64, cpu_env);
                    tcg_gen_qemu_st_i64(s->tmp1_i64, s->A0,
                                        s->mem_index, MO_LEUQ);
                    gen_helper_fpop(cpu_env);
                    break;
                default:
                    goto unknown_op;
                }

                if (update_fdp) {
                    int last_seg = s->override >= 0 ? s->override : a.def_seg;

                    tcg_gen_ld_i32(s->tmp2_i32, cpu_env,
                                   offsetof(CPUX86State,
                                            segs[last_seg].selector));
                    tcg_gen_st16_i32(s->tmp2_i32, cpu_env,
                                     offsetof(CPUX86State, fpds));
                    tcg_gen_st_tl(last_addr, cpu_env,
                                  offsetof(CPUX86State, fpdp));
                }
                tcg_temp_free(last_addr);
            } else {
                /* register float ops */
                opreg = rm;

                switch (op) {
                case 0x08: /* fld sti */
                    gen_helper_fpush(cpu_env);
                    gen_helper_fmov_ST0_STN(cpu_env,
                                            tcg_const_i32((opreg + 1) & 7));
                    break;
                case 0x09: /* fxchg sti */
                case 0x29: /* fxchg4 sti, undocumented op */
                case 0x39: /* fxchg7 sti, undocumented op */
                    gen_helper_fxchg_ST0_STN(cpu_env, tcg_const_i32(opreg));
                    break;
                case 0x0a: /* grp d9/2 */
                    switch (rm) {
                    case 0: /* fnop */
                        /* check exceptions (FreeBSD FPU probe) */
                        gen_helper_fwait(cpu_env);
                        update_fip = false;
                        break;
                    default:
                        goto unknown_op;
                    }
                    break;
                case 0x0c: /* grp d9/4 */
                    switch (rm) {
                    case 0: /* fchs */
                        gen_helper_fchs_ST0(cpu_env);
                        break;
                    case 1: /* fabs */
                        gen_helper_fabs_ST0(cpu_env);
                        break;
                    case 4: /* ftst */
                        gen_helper_fldz_FT0(cpu_env);
                        gen_helper_fcom_ST0_FT0(cpu_env);
                        break;
                    case 5: /* fxam */
                        gen_helper_fxam_ST0(cpu_env);
                        break;
                    default:
                        goto unknown_op;
                    }
                    break;
                case 0x0d: /* grp d9/5 */
                    {
                        switch (rm) {
                        case 0:
                            gen_helper_fpush(cpu_env);
                            gen_helper_fld1_ST0(cpu_env);
                            break;
                        case 1:
                            gen_helper_fpush(cpu_env);
                            gen_helper_fldl2t_ST0(cpu_env);
                            break;
                        case 2:
                            gen_helper_fpush(cpu_env);
                            gen_helper_fldl2e_ST0(cpu_env);
                            break;
                        case 3:
                            gen_helper_fpush(cpu_env);
                            gen_helper_fldpi_ST0(cpu_env);
                            break;
                        case 4:
                            gen_helper_fpush(cpu_env);
                            gen_helper_fldlg2_ST0(cpu_env);
                            break;
                        case 5:
                            gen_helper_fpush(cpu_env);
                            gen_helper_fldln2_ST0(cpu_env);
                            break;
                        case 6:
                            gen_helper_fpush(cpu_env);
                            gen_helper_fldz_ST0(cpu_env);
                            break;
                        default:
                            goto unknown_op;
                        }
                    }
                    break;
                case 0x0e: /* grp d9/6 */
                    switch (rm) {
                    case 0: /* f2xm1 */
                        gen_helper_f2xm1(cpu_env);
                        break;
                    case 1: /* fyl2x */
                        gen_helper_fyl2x(cpu_env);
                        break;
                    case 2: /* fptan */
                        gen_helper_fptan(cpu_env);
                        break;
                    case 3: /* fpatan */
                        gen_helper_fpatan(cpu_env);
                        break;
                    case 4: /* fxtract */
                        gen_helper_fxtract(cpu_env);
                        break;
                    case 5: /* fprem1 */
                        gen_helper_fprem1(cpu_env);
                        break;
                    case 6: /* fdecstp */
                        gen_helper_fdecstp(cpu_env);
                        break;
                    default:
                    case 7: /* fincstp */
                        gen_helper_fincstp(cpu_env);
                        break;
                    }
                    break;
                case 0x0f: /* grp d9/7 */
                    switch (rm) {
                    case 0: /* fprem */
                        gen_helper_fprem(cpu_env);
                        break;
                    case 1: /* fyl2xp1 */
                        gen_helper_fyl2xp1(cpu_env);
                        break;
                    case 2: /* fsqrt */
                        gen_helper_fsqrt(cpu_env);
                        break;
                    case 3: /* fsincos */
                        gen_helper_fsincos(cpu_env);
                        break;
                    case 5: /* fscale */
                        gen_helper_fscale(cpu_env);
                        break;
                    case 4: /* frndint */
                        gen_helper_frndint(cpu_env);
                        break;
                    case 6: /* fsin */
                        gen_helper_fsin(cpu_env);
                        break;
                    default:
                    case 7: /* fcos */
                        gen_helper_fcos(cpu_env);
                        break;
                    }
                    break;
                case 0x00: case 0x01: case 0x04 ... 0x07: /* fxxx st, sti */
                case 0x20: case 0x21: case 0x24 ... 0x27: /* fxxx sti, st */
                case 0x30: case 0x31: case 0x34 ... 0x37: /* fxxxp sti, st */
                    {
                        int op1;

                        op1 = op & 7;
                        if (op >= 0x20) {
                            gen_helper_fp_arith_STN_ST0(op1, opreg);
                            if (op >= 0x30) {
                                gen_helper_fpop(cpu_env);
                            }
                        } else {
                            gen_helper_fmov_FT0_STN(cpu_env,
                                                    tcg_const_i32(opreg));
                            gen_helper_fp_arith_ST0_FT0(op1);
                        }
                    }
                    break;
                case 0x02: /* fcom */
                case 0x22: /* fcom2, undocumented op */
                    gen_helper_fmov_FT0_STN(cpu_env, tcg_const_i32(opreg));
                    gen_helper_fcom_ST0_FT0(cpu_env);
                    break;
                case 0x03: /* fcomp */
                case 0x23: /* fcomp3, undocumented op */
                case 0x32: /* fcomp5, undocumented op */
                    gen_helper_fmov_FT0_STN(cpu_env, tcg_const_i32(opreg));
                    gen_helper_fcom_ST0_FT0(cpu_env);
                    gen_helper_fpop(cpu_env);
                    break;
                case 0x15: /* da/5 */
                    switch (rm) {
                    case 1: /* fucompp */
                        gen_helper_fmov_FT0_STN(cpu_env, tcg_const_i32(1));
                        gen_helper_fucom_ST0_FT0(cpu_env);
                        gen_helper_fpop(cpu_env);
                        gen_helper_fpop(cpu_env);
                        break;
                    default:
                        goto unknown_op;
                    }
                    break;
                case 0x1c:
                    switch (rm) {
                    case 0: /* feni (287 only, just do nop here) */
                        break;
                    case 1: /* fdisi (287 only, just do nop here) */
                        break;
                    case 2: /* fclex */
                        gen_helper_fclex(cpu_env);
                        update_fip = false;
                        break;
                    case 3: /* fninit */
                        gen_helper_fninit(cpu_env);
                        update_fip = false;
                        break;
                    case 4: /* fsetpm (287 only, just do nop here) */
                        break;
                    default:
                        goto unknown_op;
                    }
                    break;
                case 0x1d: /* fucomi */
                    if (!(s->cpuid_features & CPUID_CMOV)) {
                        goto illegal_op;
                    }
                    gen_update_cc_op(s);
                    gen_helper_fmov_FT0_STN(cpu_env, tcg_const_i32(opreg));
                    gen_helper_fucomi_ST0_FT0(cpu_env);
                    set_cc_op(s, CC_OP_EFLAGS);
                    break;
                case 0x1e: /* fcomi */
                    if (!(s->cpuid_features & CPUID_CMOV)) {
                        goto illegal_op;
                    }
                    gen_update_cc_op(s);
                    gen_helper_fmov_FT0_STN(cpu_env, tcg_const_i32(opreg));
                    gen_helper_fcomi_ST0_FT0(cpu_env);
                    set_cc_op(s, CC_OP_EFLAGS);
                    break;
                case 0x28: /* ffree sti */
                    gen_helper_ffree_STN(cpu_env, tcg_const_i32(opreg));
                    break;
                case 0x2a: /* fst sti */
                    gen_helper_fmov_STN_ST0(cpu_env, tcg_const_i32(opreg));
                    break;
                case 0x2b: /* fstp sti */
                case 0x0b: /* fstp1 sti, undocumented op */
                case 0x3a: /* fstp8 sti, undocumented op */
                case 0x3b: /* fstp9 sti, undocumented op */
                    gen_helper_fmov_STN_ST0(cpu_env, tcg_const_i32(opreg));
                    gen_helper_fpop(cpu_env);
                    break;
                case 0x2c: /* fucom st(i) */
                    gen_helper_fmov_FT0_STN(cpu_env, tcg_const_i32(opreg));
                    gen_helper_fucom_ST0_FT0(cpu_env);
                    break;
                case 0x2d: /* fucomp st(i) */
                    gen_helper_fmov_FT0_STN(cpu_env, tcg_const_i32(opreg));
                    gen_helper_fucom_ST0_FT0(cpu_env);
                    gen_helper_fpop(cpu_env);
                    break;
                case 0x33: /* de/3 */
                    switch (rm) {
                    case 1: /* fcompp */
                        gen_helper_fmov_FT0_STN(cpu_env, tcg_const_i32(1));
                        gen_helper_fcom_ST0_FT0(cpu_env);
                        gen_helper_fpop(cpu_env);
                        gen_helper_fpop(cpu_env);
                        break;
                    default:
                        goto unknown_op;
                    }
                    break;
                case 0x38: /* ffreep sti, undocumented op */
                    gen_helper_ffree_STN(cpu_env, tcg_const_i32(opreg));
                    gen_helper_fpop(cpu_env);
                    break;
                case 0x3c: /* df/4 */
                    switch (rm) {
                    case 0:
                        gen_helper_fnstsw(s->tmp2_i32, cpu_env);
                        tcg_gen_extu_i32_tl(s->T0, s->tmp2_i32);
                        gen_op_mov_reg_v(s, MO_16, R_EAX, s->T0);
                        break;
                    default:
                        goto unknown_op;
                    }
                    break;
                case 0x3d: /* fucomip */
                    if (!(s->cpuid_features & CPUID_CMOV)) {
                        goto illegal_op;
                    }
                    gen_update_cc_op(s);
                    gen_helper_fmov_FT0_STN(cpu_env, tcg_const_i32(opreg));
                    gen_helper_fucomi_ST0_FT0(cpu_env);
                    gen_helper_fpop(cpu_env);
                    set_cc_op(s, CC_OP_EFLAGS);
                    break;
                case 0x3e: /* fcomip */
                    if (!(s->cpuid_features & CPUID_CMOV)) {
                        goto illegal_op;
                    }
                    gen_update_cc_op(s);
                    gen_helper_fmov_FT0_STN(cpu_env, tcg_const_i32(opreg));
                    gen_helper_fcomi_ST0_FT0(cpu_env);
                    gen_helper_fpop(cpu_env);
                    set_cc_op(s, CC_OP_EFLAGS);
                    break;
                case 0x10 ... 0x13: /* fcmovxx */
                case 0x18 ... 0x1b:
                    {
                        int op1;
                        TCGLabel *l1;
                        static const uint8_t fcmov_cc[8] = {
                            (JCC_B << 1),
                            (JCC_Z << 1),
                            (JCC_BE << 1),
                            (JCC_P << 1),
                        };

                        if (!(s->cpuid_features & CPUID_CMOV)) {
                            goto illegal_op;
                        }
                        op1 = fcmov_cc[op & 3] | (((op >> 3) & 1) ^ 1);
                        l1 = gen_new_label();
                        gen_jcc1_noeob(s, op1, l1);
                        gen_helper_fmov_ST0_STN(cpu_env, tcg_const_i32(opreg));
                        gen_set_label(l1);
                    }
                    break;
                default:
                    goto unknown_op;
                }
            }

            if (update_fip) {
                tcg_gen_ld_i32(s->tmp2_i32, cpu_env,
                               offsetof(CPUX86State, segs[R_CS].selector));
                tcg_gen_st16_i32(s->tmp2_i32, cpu_env,
                                 offsetof(CPUX86State, fpcs));
                tcg_gen_st_tl(eip_cur_tl(s),
                              cpu_env, offsetof(CPUX86State, fpip));
            }
        }
        break;
        /************************/
        /* string ops */

    case 0xa4: /* movsS */
    case 0xa5:
        ot = mo_b_d(b, dflag);
        if (prefixes & (PREFIX_REPZ | PREFIX_REPNZ)) {
            gen_repz_movs(s, ot);
        } else {
            gen_movs(s, ot);
        }
        break;

    case 0xaa: /* stosS */
    case 0xab:
        ot = mo_b_d(b, dflag);
        if (prefixes & (PREFIX_REPZ | PREFIX_REPNZ)) {
            gen_repz_stos(s, ot);
        } else {
            gen_stos(s, ot);
        }
        break;
    case 0xac: /* lodsS */
    case 0xad:
        ot = mo_b_d(b, dflag);
        if (prefixes & (PREFIX_REPZ | PREFIX_REPNZ)) {
            gen_repz_lods(s, ot);
        } else {
            gen_lods(s, ot);
        }
        break;
    case 0xae: /* scasS */
    case 0xaf:
        ot = mo_b_d(b, dflag);
        if (prefixes & PREFIX_REPNZ) {
            gen_repz_scas(s, ot, 1);
        } else if (prefixes & PREFIX_REPZ) {
            gen_repz_scas(s, ot, 0);
        } else {
            gen_scas(s, ot);
        }
        break;

    case 0xa6: /* cmpsS */
    case 0xa7:
        ot = mo_b_d(b, dflag);
        if (prefixes & PREFIX_REPNZ) {
            gen_repz_cmps(s, ot, 1);
        } else if (prefixes & PREFIX_REPZ) {
            gen_repz_cmps(s, ot, 0);
        } else {
            gen_cmps(s, ot);
        }
        break;
    case 0x6c: /* insS */
    case 0x6d:
        ot = mo_b_d32(b, dflag);
        tcg_gen_trunc_tl_i32(s->tmp2_i32, cpu_regs[R_EDX]);
        tcg_gen_ext16u_i32(s->tmp2_i32, s->tmp2_i32);
        if (!gen_check_io(s, ot, s->tmp2_i32,
                          SVM_IOIO_TYPE_MASK | SVM_IOIO_STR_MASK)) {
            break;
        }
        if (tb_cflags(s->base.tb) & CF_USE_ICOUNT) {
            gen_io_start();
            s->base.is_jmp = DISAS_TOO_MANY;
        }
        if (prefixes & (PREFIX_REPZ | PREFIX_REPNZ)) {
            gen_repz_ins(s, ot);
        } else {
            gen_ins(s, ot);
        }
        break;
    case 0x6e: /* outsS */
    case 0x6f:
        ot = mo_b_d32(b, dflag);
        tcg_gen_trunc_tl_i32(s->tmp2_i32, cpu_regs[R_EDX]);
        tcg_gen_ext16u_i32(s->tmp2_i32, s->tmp2_i32);
        if (!gen_check_io(s, ot, s->tmp2_i32, SVM_IOIO_STR_MASK)) {
            break;
        }
        if (tb_cflags(s->base.tb) & CF_USE_ICOUNT) {
            gen_io_start();
            s->base.is_jmp = DISAS_TOO_MANY;
        }
        if (prefixes & (PREFIX_REPZ | PREFIX_REPNZ)) {
            gen_repz_outs(s, ot);
        } else {
            gen_outs(s, ot);
        }
        break;

        /************************/
        /* port I/O */

    case 0xe4:
    case 0xe5:
        ot = mo_b_d32(b, dflag);
        val = x86_ldub_code(env, s);
        tcg_gen_movi_i32(s->tmp2_i32, val);
        if (!gen_check_io(s, ot, s->tmp2_i32, SVM_IOIO_TYPE_MASK)) {
            break;
        }
        if (tb_cflags(s->base.tb) & CF_USE_ICOUNT) {
            gen_io_start();
            s->base.is_jmp = DISAS_TOO_MANY;
        }
        gen_helper_in_func(ot, s->T1, s->tmp2_i32);
        gen_op_mov_reg_v(s, ot, R_EAX, s->T1);
        gen_bpt_io(s, s->tmp2_i32, ot);
        break;
    case 0xe6:
    case 0xe7:
        ot = mo_b_d32(b, dflag);
        val = x86_ldub_code(env, s);
        tcg_gen_movi_i32(s->tmp2_i32, val);
        if (!gen_check_io(s, ot, s->tmp2_i32, 0)) {
            break;
        }
        if (tb_cflags(s->base.tb) & CF_USE_ICOUNT) {
            gen_io_start();
            s->base.is_jmp = DISAS_TOO_MANY;
        }
        gen_op_mov_v_reg(s, ot, s->T1, R_EAX);
        tcg_gen_trunc_tl_i32(s->tmp3_i32, s->T1);
        gen_helper_out_func(ot, s->tmp2_i32, s->tmp3_i32);
        gen_bpt_io(s, s->tmp2_i32, ot);
        break;
    case 0xec:
    case 0xed:
        ot = mo_b_d32(b, dflag);
        tcg_gen_trunc_tl_i32(s->tmp2_i32, cpu_regs[R_EDX]);
        tcg_gen_ext16u_i32(s->tmp2_i32, s->tmp2_i32);
        if (!gen_check_io(s, ot, s->tmp2_i32, SVM_IOIO_TYPE_MASK)) {
            break;
        }
        if (tb_cflags(s->base.tb) & CF_USE_ICOUNT) {
            gen_io_start();
            s->base.is_jmp = DISAS_TOO_MANY;
        }
        gen_helper_in_func(ot, s->T1, s->tmp2_i32);
        gen_op_mov_reg_v(s, ot, R_EAX, s->T1);
        gen_bpt_io(s, s->tmp2_i32, ot);
        break;
    case 0xee:
    case 0xef:
        ot = mo_b_d32(b, dflag);
        tcg_gen_trunc_tl_i32(s->tmp2_i32, cpu_regs[R_EDX]);
        tcg_gen_ext16u_i32(s->tmp2_i32, s->tmp2_i32);
        if (!gen_check_io(s, ot, s->tmp2_i32, 0)) {
            break;
        }
        if (tb_cflags(s->base.tb) & CF_USE_ICOUNT) {
            gen_io_start();
            s->base.is_jmp = DISAS_TOO_MANY;
        }
        gen_op_mov_v_reg(s, ot, s->T1, R_EAX);
        tcg_gen_trunc_tl_i32(s->tmp3_i32, s->T1);
        gen_helper_out_func(ot, s->tmp2_i32, s->tmp3_i32);
        gen_bpt_io(s, s->tmp2_i32, ot);
        break;

        /************************/
        /* control */
    case 0xc2: /* ret im */
        val = x86_ldsw_code(env, s);
        ot = gen_pop_T0(s);
        gen_stack_update(s, val + (1 << ot));
        /* Note that gen_pop_T0 uses a zero-extending load.  */
        gen_op_jmp_v(s, s->T0);
        gen_bnd_jmp(s);
        s->base.is_jmp = DISAS_JUMP;
        break;
    case 0xc3: /* ret */
        ot = gen_pop_T0(s);
        gen_pop_update(s, ot);
        /* Note that gen_pop_T0 uses a zero-extending load.  */
        gen_op_jmp_v(s, s->T0);
        gen_bnd_jmp(s);
        s->base.is_jmp = DISAS_JUMP;
        break;
    case 0xca: /* lret im */
        val = x86_ldsw_code(env, s);
    do_lret:
        if (PE(s) && !VM86(s)) {
            gen_update_cc_op(s);
            gen_update_eip_cur(s);
            gen_helper_lret_protected(cpu_env, tcg_const_i32(dflag - 1),
                                      tcg_const_i32(val));
        } else {
            gen_stack_A0(s);
            /* pop offset */
            gen_op_ld_v(s, dflag, s->T0, s->A0);
            /* NOTE: keeping EIP updated is not a problem in case of
               exception */
            gen_op_jmp_v(s, s->T0);
            /* pop selector */
            gen_add_A0_im(s, 1 << dflag);
            gen_op_ld_v(s, dflag, s->T0, s->A0);
            gen_op_movl_seg_T0_vm(s, R_CS);
            /* add stack offset */
            gen_stack_update(s, val + (2 << dflag));
        }
        s->base.is_jmp = DISAS_EOB_ONLY;
        break;
    case 0xcb: /* lret */
        val = 0;
        goto do_lret;
    case 0xcf: /* iret */
        gen_svm_check_intercept(s, SVM_EXIT_IRET);
        if (!PE(s) || VM86(s)) {
            /* real mode or vm86 mode */
            if (!check_vm86_iopl(s)) {
                break;
            }
            gen_helper_iret_real(cpu_env, tcg_const_i32(dflag - 1));
        } else {
            gen_helper_iret_protected(cpu_env, tcg_constant_i32(dflag - 1),
                                      eip_next_i32(s));
        }
        set_cc_op(s, CC_OP_EFLAGS);
        s->base.is_jmp = DISAS_EOB_ONLY;
        break;
    case 0xe8: /* call im */
        {
            int diff = (dflag != MO_16
                        ? (int32_t)insn_get(env, s, MO_32)
                        : (int16_t)insn_get(env, s, MO_16));
            gen_push_v(s, eip_next_tl(s));
            gen_bnd_jmp(s);
            gen_jmp_rel(s, dflag, diff, 0);
        }
        break;
    case 0x9a: /* lcall im */
        {
            unsigned int selector, offset;

            if (CODE64(s))
                goto illegal_op;
            ot = dflag;
            offset = insn_get(env, s, ot);
            selector = insn_get(env, s, MO_16);

            tcg_gen_movi_tl(s->T0, selector);
            tcg_gen_movi_tl(s->T1, offset);
        }
        goto do_lcall;
    case 0xe9: /* jmp im */
        {
            int diff = (dflag != MO_16
                        ? (int32_t)insn_get(env, s, MO_32)
                        : (int16_t)insn_get(env, s, MO_16));
            gen_bnd_jmp(s);
            gen_jmp_rel(s, dflag, diff, 0);
        }
        break;
    case 0xea: /* ljmp im */
        {
            unsigned int selector, offset;

            if (CODE64(s))
                goto illegal_op;
            ot = dflag;
            offset = insn_get(env, s, ot);
            selector = insn_get(env, s, MO_16);

            tcg_gen_movi_tl(s->T0, selector);
            tcg_gen_movi_tl(s->T1, offset);
        }
        goto do_ljmp;
    case 0xeb: /* jmp Jb */
        {
            int diff = (int8_t)insn_get(env, s, MO_8);
            gen_jmp_rel(s, dflag, diff, 0);
        }
        break;
    case 0x70 ... 0x7f: /* jcc Jb */
        {
            int diff = (int8_t)insn_get(env, s, MO_8);
            gen_bnd_jmp(s);
            gen_jcc(s, b, diff);
        }
        break;
    case 0x180 ... 0x18f: /* jcc Jv */
        {
            int diff = (dflag != MO_16
                        ? (int32_t)insn_get(env, s, MO_32)
                        : (int16_t)insn_get(env, s, MO_16));
            gen_bnd_jmp(s);
            gen_jcc(s, b, diff);
        }
        break;

    case 0x190 ... 0x19f: /* setcc Gv */
        modrm = x86_ldub_code(env, s);
        gen_setcc1(s, b, s->T0);
        gen_ldst_modrm(env, s, modrm, MO_8, OR_TMP0, 1);
        break;
    case 0x140 ... 0x14f: /* cmov Gv, Ev */
        if (!(s->cpuid_features & CPUID_CMOV)) {
            goto illegal_op;
        }
        ot = dflag;
        modrm = x86_ldub_code(env, s);
        reg = ((modrm >> 3) & 7) | REX_R(s);
        gen_cmovcc1(env, s, ot, b, modrm, reg);
        break;

        /************************/
        /* flags */
    case 0x9c: /* pushf */
        gen_svm_check_intercept(s, SVM_EXIT_PUSHF);
        if (check_vm86_iopl(s)) {
            gen_update_cc_op(s);
            gen_helper_read_eflags(s->T0, cpu_env);
            gen_push_v(s, s->T0);
        }
        break;
    case 0x9d: /* popf */
        gen_svm_check_intercept(s, SVM_EXIT_POPF);
        if (check_vm86_iopl(s)) {
            ot = gen_pop_T0(s);
            if (CPL(s) == 0) {
                if (dflag != MO_16) {
                    gen_helper_write_eflags(cpu_env, s->T0,
                                            tcg_const_i32((TF_MASK | AC_MASK |
                                                           ID_MASK | NT_MASK |
                                                           IF_MASK |
                                                           IOPL_MASK)));
                } else {
                    gen_helper_write_eflags(cpu_env, s->T0,
                                            tcg_const_i32((TF_MASK | AC_MASK |
                                                           ID_MASK | NT_MASK |
                                                           IF_MASK | IOPL_MASK)
                                                          & 0xffff));
                }
            } else {
                if (CPL(s) <= IOPL(s)) {
                    if (dflag != MO_16) {
                        gen_helper_write_eflags(cpu_env, s->T0,
                                                tcg_const_i32((TF_MASK |
                                                               AC_MASK |
                                                               ID_MASK |
                                                               NT_MASK |
                                                               IF_MASK)));
                    } else {
                        gen_helper_write_eflags(cpu_env, s->T0,
                                                tcg_const_i32((TF_MASK |
                                                               AC_MASK |
                                                               ID_MASK |
                                                               NT_MASK |
                                                               IF_MASK)
                                                              & 0xffff));
                    }
                } else {
                    if (dflag != MO_16) {
                        gen_helper_write_eflags(cpu_env, s->T0,
                                           tcg_const_i32((TF_MASK | AC_MASK |
                                                          ID_MASK | NT_MASK)));
                    } else {
                        gen_helper_write_eflags(cpu_env, s->T0,
                                           tcg_const_i32((TF_MASK | AC_MASK |
                                                          ID_MASK | NT_MASK)
                                                         & 0xffff));
                    }
                }
            }
            gen_pop_update(s, ot);
            set_cc_op(s, CC_OP_EFLAGS);
            /* abort translation because TF/AC flag may change */
            s->base.is_jmp = DISAS_EOB_NEXT;
        }
        break;
    case 0x9e: /* sahf */
        if (CODE64(s) && !(s->cpuid_ext3_features & CPUID_EXT3_LAHF_LM))
            goto illegal_op;
        tcg_gen_shri_tl(s->T0, cpu_regs[R_EAX], 8);
        gen_compute_eflags(s);
        tcg_gen_andi_tl(cpu_cc_src, cpu_cc_src, CC_O);
        tcg_gen_andi_tl(s->T0, s->T0, CC_S | CC_Z | CC_A | CC_P | CC_C);
        tcg_gen_or_tl(cpu_cc_src, cpu_cc_src, s->T0);
        break;
    case 0x9f: /* lahf */
        if (CODE64(s) && !(s->cpuid_ext3_features & CPUID_EXT3_LAHF_LM))
            goto illegal_op;
        gen_compute_eflags(s);
        /* Note: gen_compute_eflags() only gives the condition codes */
        tcg_gen_ori_tl(s->T0, cpu_cc_src, 0x02);
        tcg_gen_deposit_tl(cpu_regs[R_EAX], cpu_regs[R_EAX], s->T0, 8, 8);
        break;
    case 0xf5: /* cmc */
        gen_compute_eflags(s);
        tcg_gen_xori_tl(cpu_cc_src, cpu_cc_src, CC_C);
        break;
    case 0xf8: /* clc */
        gen_compute_eflags(s);
        tcg_gen_andi_tl(cpu_cc_src, cpu_cc_src, ~CC_C);
        break;
    case 0xf9: /* stc */
        gen_compute_eflags(s);
        tcg_gen_ori_tl(cpu_cc_src, cpu_cc_src, CC_C);
        break;
    case 0xfc: /* cld */
        tcg_gen_movi_i32(s->tmp2_i32, 1);
        tcg_gen_st_i32(s->tmp2_i32, cpu_env, offsetof(CPUX86State, df));
        break;
    case 0xfd: /* std */
        tcg_gen_movi_i32(s->tmp2_i32, -1);
        tcg_gen_st_i32(s->tmp2_i32, cpu_env, offsetof(CPUX86State, df));
        break;

        /************************/
        /* bit operations */
    case 0x1ba: /* bt/bts/btr/btc Gv, im */
        ot = dflag;
        modrm = x86_ldub_code(env, s);
        op = (modrm >> 3) & 7;
        mod = (modrm >> 6) & 3;
        rm = (modrm & 7) | REX_B(s);
        if (mod != 3) {
            s->rip_offset = 1;
            gen_lea_modrm(env, s, modrm);
            if (!(s->prefix & PREFIX_LOCK)) {
                gen_op_ld_v(s, ot, s->T0, s->A0);
            }
        } else {
            gen_op_mov_v_reg(s, ot, s->T0, rm);
        }
        /* load shift */
        val = x86_ldub_code(env, s);
        tcg_gen_movi_tl(s->T1, val);
        if (op < 4)
            goto unknown_op;
        op -= 4;
        goto bt_op;
    case 0x1a3: /* bt Gv, Ev */
        op = 0;
        goto do_btx;
    case 0x1ab: /* bts */
        op = 1;
        goto do_btx;
    case 0x1b3: /* btr */
        op = 2;
        goto do_btx;
    case 0x1bb: /* btc */
        op = 3;
    do_btx:
        ot = dflag;
        modrm = x86_ldub_code(env, s);
        reg = ((modrm >> 3) & 7) | REX_R(s);
        mod = (modrm >> 6) & 3;
        rm = (modrm & 7) | REX_B(s);
        gen_op_mov_v_reg(s, MO_32, s->T1, reg);
        if (mod != 3) {
            AddressParts a = gen_lea_modrm_0(env, s, modrm);
            /* specific case: we need to add a displacement */
            gen_exts(ot, s->T1);
            tcg_gen_sari_tl(s->tmp0, s->T1, 3 + ot);
            tcg_gen_shli_tl(s->tmp0, s->tmp0, ot);
            tcg_gen_add_tl(s->A0, gen_lea_modrm_1(s, a, false), s->tmp0);
            gen_lea_v_seg(s, s->aflag, s->A0, a.def_seg, s->override);
            if (!(s->prefix & PREFIX_LOCK)) {
                gen_op_ld_v(s, ot, s->T0, s->A0);
            }
        } else {
            gen_op_mov_v_reg(s, ot, s->T0, rm);
        }
    bt_op:
        tcg_gen_andi_tl(s->T1, s->T1, (1 << (3 + ot)) - 1);
        tcg_gen_movi_tl(s->tmp0, 1);
        tcg_gen_shl_tl(s->tmp0, s->tmp0, s->T1);
        if (s->prefix & PREFIX_LOCK) {
            switch (op) {
            case 0: /* bt */
                /* Needs no atomic ops; we surpressed the normal
                   memory load for LOCK above so do it now.  */
                gen_op_ld_v(s, ot, s->T0, s->A0);
                break;
            case 1: /* bts */
                tcg_gen_atomic_fetch_or_tl(s->T0, s->A0, s->tmp0,
                                           s->mem_index, ot | MO_LE);
                break;
            case 2: /* btr */
                tcg_gen_not_tl(s->tmp0, s->tmp0);
                tcg_gen_atomic_fetch_and_tl(s->T0, s->A0, s->tmp0,
                                            s->mem_index, ot | MO_LE);
                break;
            default:
            case 3: /* btc */
                tcg_gen_atomic_fetch_xor_tl(s->T0, s->A0, s->tmp0,
                                            s->mem_index, ot | MO_LE);
                break;
            }
            tcg_gen_shr_tl(s->tmp4, s->T0, s->T1);
        } else {
            tcg_gen_shr_tl(s->tmp4, s->T0, s->T1);
            switch (op) {
            case 0: /* bt */
                /* Data already loaded; nothing to do.  */
                break;
            case 1: /* bts */
                tcg_gen_or_tl(s->T0, s->T0, s->tmp0);
                break;
            case 2: /* btr */
                tcg_gen_andc_tl(s->T0, s->T0, s->tmp0);
                break;
            default:
            case 3: /* btc */
                tcg_gen_xor_tl(s->T0, s->T0, s->tmp0);
                break;
            }
            if (op != 0) {
                if (mod != 3) {
                    gen_op_st_v(s, ot, s->T0, s->A0);
                } else {
                    gen_op_mov_reg_v(s, ot, rm, s->T0);
                }
            }
        }

        /* Delay all CC updates until after the store above.  Note that
           C is the result of the test, Z is unchanged, and the others
           are all undefined.  */
        switch (s->cc_op) {
        case CC_OP_MULB ... CC_OP_MULQ:
        case CC_OP_ADDB ... CC_OP_ADDQ:
        case CC_OP_ADCB ... CC_OP_ADCQ:
        case CC_OP_SUBB ... CC_OP_SUBQ:
        case CC_OP_SBBB ... CC_OP_SBBQ:
        case CC_OP_LOGICB ... CC_OP_LOGICQ:
        case CC_OP_INCB ... CC_OP_INCQ:
        case CC_OP_DECB ... CC_OP_DECQ:
        case CC_OP_SHLB ... CC_OP_SHLQ:
        case CC_OP_SARB ... CC_OP_SARQ:
        case CC_OP_BMILGB ... CC_OP_BMILGQ:
            /* Z was going to be computed from the non-zero status of CC_DST.
               We can get that same Z value (and the new C value) by leaving
               CC_DST alone, setting CC_SRC, and using a CC_OP_SAR of the
               same width.  */
            tcg_gen_mov_tl(cpu_cc_src, s->tmp4);
            set_cc_op(s, ((s->cc_op - CC_OP_MULB) & 3) + CC_OP_SARB);
            break;
        default:
            /* Otherwise, generate EFLAGS and replace the C bit.  */
            gen_compute_eflags(s);
            tcg_gen_deposit_tl(cpu_cc_src, cpu_cc_src, s->tmp4,
                               ctz32(CC_C), 1);
            break;
        }
        break;
    case 0x1bc: /* bsf / tzcnt */
    case 0x1bd: /* bsr / lzcnt */
        ot = dflag;
        modrm = x86_ldub_code(env, s);
        reg = ((modrm >> 3) & 7) | REX_R(s);
        gen_ldst_modrm(env, s, modrm, ot, OR_TMP0, 0);
        gen_extu(ot, s->T0);

        /* Note that lzcnt and tzcnt are in different extensions.  */
        if ((prefixes & PREFIX_REPZ)
            && (b & 1
                ? s->cpuid_ext3_features & CPUID_EXT3_ABM
                : s->cpuid_7_0_ebx_features & CPUID_7_0_EBX_BMI1)) {
            int size = 8 << ot;
            /* For lzcnt/tzcnt, C bit is defined related to the input. */
            tcg_gen_mov_tl(cpu_cc_src, s->T0);
            if (b & 1) {
                /* For lzcnt, reduce the target_ulong result by the
                   number of zeros that we expect to find at the top.  */
                tcg_gen_clzi_tl(s->T0, s->T0, TARGET_LONG_BITS);
                tcg_gen_subi_tl(s->T0, s->T0, TARGET_LONG_BITS - size);
            } else {
                /* For tzcnt, a zero input must return the operand size.  */
                tcg_gen_ctzi_tl(s->T0, s->T0, size);
            }
            /* For lzcnt/tzcnt, Z bit is defined related to the result.  */
            gen_op_update1_cc(s);
            set_cc_op(s, CC_OP_BMILGB + ot);
        } else {
            /* For bsr/bsf, only the Z bit is defined and it is related
               to the input and not the result.  */
            tcg_gen_mov_tl(cpu_cc_dst, s->T0);
            set_cc_op(s, CC_OP_LOGICB + ot);

            /* ??? The manual says that the output is undefined when the
               input is zero, but real hardware leaves it unchanged, and
               real programs appear to depend on that.  Accomplish this
               by passing the output as the value to return upon zero.  */
            if (b & 1) {
                /* For bsr, return the bit index of the first 1 bit,
                   not the count of leading zeros.  */
                tcg_gen_xori_tl(s->T1, cpu_regs[reg], TARGET_LONG_BITS - 1);
                tcg_gen_clz_tl(s->T0, s->T0, s->T1);
                tcg_gen_xori_tl(s->T0, s->T0, TARGET_LONG_BITS - 1);
            } else {
                tcg_gen_ctz_tl(s->T0, s->T0, cpu_regs[reg]);
            }
        }
        gen_op_mov_reg_v(s, ot, reg, s->T0);
        break;
        /************************/
        /* bcd */
    case 0x27: /* daa */
        if (CODE64(s))
            goto illegal_op;
        gen_update_cc_op(s);
        gen_helper_daa(cpu_env);
        set_cc_op(s, CC_OP_EFLAGS);
        break;
    case 0x2f: /* das */
        if (CODE64(s))
            goto illegal_op;
        gen_update_cc_op(s);
        gen_helper_das(cpu_env);
        set_cc_op(s, CC_OP_EFLAGS);
        break;
    case 0x37: /* aaa */
        if (CODE64(s))
            goto illegal_op;
        gen_update_cc_op(s);
        gen_helper_aaa(cpu_env);
        set_cc_op(s, CC_OP_EFLAGS);
        break;
    case 0x3f: /* aas */
        if (CODE64(s))
            goto illegal_op;
        gen_update_cc_op(s);
        gen_helper_aas(cpu_env);
        set_cc_op(s, CC_OP_EFLAGS);
        break;
    case 0xd4: /* aam */
        if (CODE64(s))
            goto illegal_op;
        val = x86_ldub_code(env, s);
        if (val == 0) {
            gen_exception(s, EXCP00_DIVZ);
        } else {
            gen_helper_aam(cpu_env, tcg_const_i32(val));
            set_cc_op(s, CC_OP_LOGICB);
        }
        break;
    case 0xd5: /* aad */
        if (CODE64(s))
            goto illegal_op;
        val = x86_ldub_code(env, s);
        gen_helper_aad(cpu_env, tcg_const_i32(val));
        set_cc_op(s, CC_OP_LOGICB);
        break;
        /************************/
        /* misc */
    case 0x90: /* nop */
        /* XXX: correct lock test for all insn */
        if (prefixes & PREFIX_LOCK) {
            goto illegal_op;
        }
        /* If REX_B is set, then this is xchg eax, r8d, not a nop.  */
        if (REX_B(s)) {
            goto do_xchg_reg_eax;
        }
        if (prefixes & PREFIX_REPZ) {
            gen_update_cc_op(s);
            gen_update_eip_cur(s);
            gen_helper_pause(cpu_env, cur_insn_len_i32(s));
            s->base.is_jmp = DISAS_NORETURN;
        }
        break;
    case 0x9b: /* fwait */
        if ((s->flags & (HF_MP_MASK | HF_TS_MASK)) ==
            (HF_MP_MASK | HF_TS_MASK)) {
            gen_exception(s, EXCP07_PREX);
        } else {
            gen_helper_fwait(cpu_env);
        }
        break;
    case 0xcc: /* int3 */
        gen_interrupt(s, EXCP03_INT3);
        break;
    case 0xcd: /* int N */
        val = x86_ldub_code(env, s);
        if (check_vm86_iopl(s)) {
            gen_interrupt(s, val);
        }
        break;
    case 0xce: /* into */
        if (CODE64(s))
            goto illegal_op;
        gen_update_cc_op(s);
        gen_update_eip_cur(s);
        gen_helper_into(cpu_env, cur_insn_len_i32(s));
        break;
#ifdef WANT_ICEBP
    case 0xf1: /* icebp (undocumented, exits to external debugger) */
        gen_svm_check_intercept(s, SVM_EXIT_ICEBP);
        gen_debug(s);
        break;
#endif
    case 0xfa: /* cli */
        if (check_iopl(s)) {
            gen_reset_eflags(s, IF_MASK);
        }
        break;
    case 0xfb: /* sti */
        if (check_iopl(s)) {
            gen_set_eflags(s, IF_MASK);
            /* interruptions are enabled only the first insn after sti */
            gen_update_eip_next(s);
            gen_eob_inhibit_irq(s, true);
        }
        break;
    case 0x62: /* bound */
        if (CODE64(s))
            goto illegal_op;
        ot = dflag;
        modrm = x86_ldub_code(env, s);
        reg = (modrm >> 3) & 7;
        mod = (modrm >> 6) & 3;
        if (mod == 3)
            goto illegal_op;
        gen_op_mov_v_reg(s, ot, s->T0, reg);
        gen_lea_modrm(env, s, modrm);
        tcg_gen_trunc_tl_i32(s->tmp2_i32, s->T0);
        if (ot == MO_16) {
            gen_helper_boundw(cpu_env, s->A0, s->tmp2_i32);
        } else {
            gen_helper_boundl(cpu_env, s->A0, s->tmp2_i32);
        }
        break;
    case 0x1c8 ... 0x1cf: /* bswap reg */
        reg = (b & 7) | REX_B(s);
#ifdef TARGET_X86_64
        if (dflag == MO_64) {
            tcg_gen_bswap64_i64(cpu_regs[reg], cpu_regs[reg]);
            break;
        }
#endif
        tcg_gen_bswap32_tl(cpu_regs[reg], cpu_regs[reg], TCG_BSWAP_OZ);
        break;
    case 0xd6: /* salc */
        if (CODE64(s))
            goto illegal_op;
        gen_compute_eflags_c(s, s->T0);
        tcg_gen_neg_tl(s->T0, s->T0);
        gen_op_mov_reg_v(s, MO_8, R_EAX, s->T0);
        break;
    case 0xe0: /* loopnz */
    case 0xe1: /* loopz */
    case 0xe2: /* loop */
    case 0xe3: /* jecxz */
        {
            TCGLabel *l1, *l2;
            int diff = (int8_t)insn_get(env, s, MO_8);

            l1 = gen_new_label();
            l2 = gen_new_label();
            gen_update_cc_op(s);
            b &= 3;
            switch(b) {
            case 0: /* loopnz */
            case 1: /* loopz */
                gen_op_add_reg_im(s, s->aflag, R_ECX, -1);
                gen_op_jz_ecx(s, l2);
                gen_jcc1(s, (JCC_Z << 1) | (b ^ 1), l1);
                break;
            case 2: /* loop */
                gen_op_add_reg_im(s, s->aflag, R_ECX, -1);
                gen_op_jnz_ecx(s, l1);
                break;
            default:
            case 3: /* jcxz */
                gen_op_jz_ecx(s, l1);
                break;
            }

            gen_set_label(l2);
            gen_jmp_rel_csize(s, 0, 1);

            gen_set_label(l1);
            gen_jmp_rel(s, dflag, diff, 0);
        }
        break;
    case 0x130: /* wrmsr */
    case 0x132: /* rdmsr */
        if (check_cpl0(s)) {
            gen_update_cc_op(s);
            gen_update_eip_cur(s);
            if (b & 2) {
                gen_helper_rdmsr(cpu_env);
            } else {
                gen_helper_wrmsr(cpu_env);
                s->base.is_jmp = DISAS_EOB_NEXT;
            }
        }
        break;
    case 0x131: /* rdtsc */
        gen_update_cc_op(s);
        gen_update_eip_cur(s);
        if (tb_cflags(s->base.tb) & CF_USE_ICOUNT) {
            gen_io_start();
            s->base.is_jmp = DISAS_TOO_MANY;
        }
        gen_helper_rdtsc(cpu_env);
        break;
    case 0x133: /* rdpmc */
        gen_update_cc_op(s);
        gen_update_eip_cur(s);
        gen_helper_rdpmc(cpu_env);
        s->base.is_jmp = DISAS_NORETURN;
        break;
    case 0x134: /* sysenter */
        /* For Intel SYSENTER is valid on 64-bit */
        if (CODE64(s) && env->cpuid_vendor1 != CPUID_VENDOR_INTEL_1)
            goto illegal_op;
        if (!PE(s)) {
            gen_exception_gpf(s);
        } else {
            gen_helper_sysenter(cpu_env);
            s->base.is_jmp = DISAS_EOB_ONLY;
        }
        break;
    case 0x135: /* sysexit */
        /* For Intel SYSEXIT is valid on 64-bit */
        if (CODE64(s) && env->cpuid_vendor1 != CPUID_VENDOR_INTEL_1)
            goto illegal_op;
        if (!PE(s)) {
            gen_exception_gpf(s);
        } else {
            gen_helper_sysexit(cpu_env, tcg_const_i32(dflag - 1));
            s->base.is_jmp = DISAS_EOB_ONLY;
        }
        break;
#ifdef TARGET_X86_64
    case 0x105: /* syscall */
        /* XXX: is it usable in real mode ? */
        gen_update_cc_op(s);
        gen_update_eip_cur(s);
        gen_helper_syscall(cpu_env, cur_insn_len_i32(s));
        /* TF handling for the syscall insn is different. The TF bit is  checked
           after the syscall insn completes. This allows #DB to not be
           generated after one has entered CPL0 if TF is set in FMASK.  */
        gen_eob_worker(s, false, true);
        break;
    case 0x107: /* sysret */
        if (!PE(s)) {
            gen_exception_gpf(s);
        } else {
            gen_helper_sysret(cpu_env, tcg_const_i32(dflag - 1));
            /* condition codes are modified only in long mode */
            if (LMA(s)) {
                set_cc_op(s, CC_OP_EFLAGS);
            }
            /* TF handling for the sysret insn is different. The TF bit is
               checked after the sysret insn completes. This allows #DB to be
               generated "as if" the syscall insn in userspace has just
               completed.  */
            gen_eob_worker(s, false, true);
        }
        break;
#endif
    case 0x1a2: /* cpuid */
        gen_update_cc_op(s);
        gen_update_eip_cur(s);
        gen_helper_cpuid(cpu_env);
        break;
    case 0xf4: /* hlt */
        if (check_cpl0(s)) {
            gen_update_cc_op(s);
            gen_update_eip_cur(s);
            gen_helper_hlt(cpu_env, cur_insn_len_i32(s));
            s->base.is_jmp = DISAS_NORETURN;
        }
        break;
    case 0x100:
        modrm = x86_ldub_code(env, s);
        mod = (modrm >> 6) & 3;
        op = (modrm >> 3) & 7;
        switch(op) {
        case 0: /* sldt */
            if (!PE(s) || VM86(s))
                goto illegal_op;
            if (s->flags & HF_UMIP_MASK && !check_cpl0(s)) {
                break;
            }
            gen_svm_check_intercept(s, SVM_EXIT_LDTR_READ);
            tcg_gen_ld32u_tl(s->T0, cpu_env,
                             offsetof(CPUX86State, ldt.selector));
            ot = mod == 3 ? dflag : MO_16;
            gen_ldst_modrm(env, s, modrm, ot, OR_TMP0, 1);
            break;
        case 2: /* lldt */
            if (!PE(s) || VM86(s))
                goto illegal_op;
            if (check_cpl0(s)) {
                gen_svm_check_intercept(s, SVM_EXIT_LDTR_WRITE);
                gen_ldst_modrm(env, s, modrm, MO_16, OR_TMP0, 0);
                tcg_gen_trunc_tl_i32(s->tmp2_i32, s->T0);
                gen_helper_lldt(cpu_env, s->tmp2_i32);
            }
            break;
        case 1: /* str */
            if (!PE(s) || VM86(s))
                goto illegal_op;
            if (s->flags & HF_UMIP_MASK && !check_cpl0(s)) {
                break;
            }
            gen_svm_check_intercept(s, SVM_EXIT_TR_READ);
            tcg_gen_ld32u_tl(s->T0, cpu_env,
                             offsetof(CPUX86State, tr.selector));
            ot = mod == 3 ? dflag : MO_16;
            gen_ldst_modrm(env, s, modrm, ot, OR_TMP0, 1);
            break;
        case 3: /* ltr */
            if (!PE(s) || VM86(s))
                goto illegal_op;
            if (check_cpl0(s)) {
                gen_svm_check_intercept(s, SVM_EXIT_TR_WRITE);
                gen_ldst_modrm(env, s, modrm, MO_16, OR_TMP0, 0);
                tcg_gen_trunc_tl_i32(s->tmp2_i32, s->T0);
                gen_helper_ltr(cpu_env, s->tmp2_i32);
            }
            break;
        case 4: /* verr */
        case 5: /* verw */
            if (!PE(s) || VM86(s))
                goto illegal_op;
            gen_ldst_modrm(env, s, modrm, MO_16, OR_TMP0, 0);
            gen_update_cc_op(s);
            if (op == 4) {
                gen_helper_verr(cpu_env, s->T0);
            } else {
                gen_helper_verw(cpu_env, s->T0);
            }
            set_cc_op(s, CC_OP_EFLAGS);
            break;
        default:
            goto unknown_op;
        }
        break;

    case 0x101:
        modrm = x86_ldub_code(env, s);
        switch (modrm) {
        CASE_MODRM_MEM_OP(0): /* sgdt */
            if (s->flags & HF_UMIP_MASK && !check_cpl0(s)) {
                break;
            }
            gen_svm_check_intercept(s, SVM_EXIT_GDTR_READ);
            gen_lea_modrm(env, s, modrm);
            tcg_gen_ld32u_tl(s->T0,
                             cpu_env, offsetof(CPUX86State, gdt.limit));
            gen_op_st_v(s, MO_16, s->T0, s->A0);
            gen_add_A0_im(s, 2);
            tcg_gen_ld_tl(s->T0, cpu_env, offsetof(CPUX86State, gdt.base));
            if (dflag == MO_16) {
                tcg_gen_andi_tl(s->T0, s->T0, 0xffffff);
            }
            gen_op_st_v(s, CODE64(s) + MO_32, s->T0, s->A0);
            break;

        case 0xc8: /* monitor */
            if (!(s->cpuid_ext_features & CPUID_EXT_MONITOR) || CPL(s) != 0) {
                goto illegal_op;
            }
            gen_update_cc_op(s);
            gen_update_eip_cur(s);
            tcg_gen_mov_tl(s->A0, cpu_regs[R_EAX]);
            gen_extu(s->aflag, s->A0);
            gen_add_A0_ds_seg(s);
            gen_helper_monitor(cpu_env, s->A0);
            break;

        case 0xc9: /* mwait */
            if (!(s->cpuid_ext_features & CPUID_EXT_MONITOR) || CPL(s) != 0) {
                goto illegal_op;
            }
            gen_update_cc_op(s);
            gen_update_eip_cur(s);
            gen_helper_mwait(cpu_env, cur_insn_len_i32(s));
            s->base.is_jmp = DISAS_NORETURN;
            break;

        case 0xca: /* clac */
            if (!(s->cpuid_7_0_ebx_features & CPUID_7_0_EBX_SMAP)
                || CPL(s) != 0) {
                goto illegal_op;
            }
            gen_reset_eflags(s, AC_MASK);
            s->base.is_jmp = DISAS_EOB_NEXT;
            break;

        case 0xcb: /* stac */
            if (!(s->cpuid_7_0_ebx_features & CPUID_7_0_EBX_SMAP)
                || CPL(s) != 0) {
                goto illegal_op;
            }
            gen_set_eflags(s, AC_MASK);
            s->base.is_jmp = DISAS_EOB_NEXT;
            break;

        CASE_MODRM_MEM_OP(1): /* sidt */
            if (s->flags & HF_UMIP_MASK && !check_cpl0(s)) {
                break;
            }
            gen_svm_check_intercept(s, SVM_EXIT_IDTR_READ);
            gen_lea_modrm(env, s, modrm);
            tcg_gen_ld32u_tl(s->T0, cpu_env, offsetof(CPUX86State, idt.limit));
            gen_op_st_v(s, MO_16, s->T0, s->A0);
            gen_add_A0_im(s, 2);
            tcg_gen_ld_tl(s->T0, cpu_env, offsetof(CPUX86State, idt.base));
            if (dflag == MO_16) {
                tcg_gen_andi_tl(s->T0, s->T0, 0xffffff);
            }
            gen_op_st_v(s, CODE64(s) + MO_32, s->T0, s->A0);
            break;

        case 0xd0: /* xgetbv */
            if ((s->cpuid_ext_features & CPUID_EXT_XSAVE) == 0
                || (s->prefix & (PREFIX_LOCK | PREFIX_DATA
                                 | PREFIX_REPZ | PREFIX_REPNZ))) {
                goto illegal_op;
            }
            tcg_gen_trunc_tl_i32(s->tmp2_i32, cpu_regs[R_ECX]);
            gen_helper_xgetbv(s->tmp1_i64, cpu_env, s->tmp2_i32);
            tcg_gen_extr_i64_tl(cpu_regs[R_EAX], cpu_regs[R_EDX], s->tmp1_i64);
            break;

        case 0xd1: /* xsetbv */
            if ((s->cpuid_ext_features & CPUID_EXT_XSAVE) == 0
                || (s->prefix & (PREFIX_LOCK | PREFIX_DATA
                                 | PREFIX_REPZ | PREFIX_REPNZ))) {
                goto illegal_op;
            }
            if (!check_cpl0(s)) {
                break;
            }
            tcg_gen_concat_tl_i64(s->tmp1_i64, cpu_regs[R_EAX],
                                  cpu_regs[R_EDX]);
            tcg_gen_trunc_tl_i32(s->tmp2_i32, cpu_regs[R_ECX]);
            gen_helper_xsetbv(cpu_env, s->tmp2_i32, s->tmp1_i64);
            /* End TB because translation flags may change.  */
            s->base.is_jmp = DISAS_EOB_NEXT;
            break;

        case 0xd8: /* VMRUN */
            if (!SVME(s) || !PE(s)) {
                goto illegal_op;
            }
            if (!check_cpl0(s)) {
                break;
            }
            gen_update_cc_op(s);
            gen_update_eip_cur(s);
            gen_helper_vmrun(cpu_env, tcg_const_i32(s->aflag - 1),
                             cur_insn_len_i32(s));
            tcg_gen_exit_tb(NULL, 0);
            s->base.is_jmp = DISAS_NORETURN;
            break;

        case 0xd9: /* VMMCALL */
            if (!SVME(s)) {
                goto illegal_op;
            }
            gen_update_cc_op(s);
            gen_update_eip_cur(s);
            gen_helper_vmmcall(cpu_env);
            break;

        case 0xda: /* VMLOAD */
            if (!SVME(s) || !PE(s)) {
                goto illegal_op;
            }
            if (!check_cpl0(s)) {
                break;
            }
            gen_update_cc_op(s);
            gen_update_eip_cur(s);
            gen_helper_vmload(cpu_env, tcg_const_i32(s->aflag - 1));
            break;

        case 0xdb: /* VMSAVE */
            if (!SVME(s) || !PE(s)) {
                goto illegal_op;
            }
            if (!check_cpl0(s)) {
                break;
            }
            gen_update_cc_op(s);
            gen_update_eip_cur(s);
            gen_helper_vmsave(cpu_env, tcg_const_i32(s->aflag - 1));
            break;

        case 0xdc: /* STGI */
            if ((!SVME(s) && !(s->cpuid_ext3_features & CPUID_EXT3_SKINIT))
                || !PE(s)) {
                goto illegal_op;
            }
            if (!check_cpl0(s)) {
                break;
            }
            gen_update_cc_op(s);
            gen_helper_stgi(cpu_env);
            s->base.is_jmp = DISAS_EOB_NEXT;
            break;

        case 0xdd: /* CLGI */
            if (!SVME(s) || !PE(s)) {
                goto illegal_op;
            }
            if (!check_cpl0(s)) {
                break;
            }
            gen_update_cc_op(s);
            gen_update_eip_cur(s);
            gen_helper_clgi(cpu_env);
            break;

        case 0xde: /* SKINIT */
            if ((!SVME(s) && !(s->cpuid_ext3_features & CPUID_EXT3_SKINIT))
                || !PE(s)) {
                goto illegal_op;
            }
            gen_svm_check_intercept(s, SVM_EXIT_SKINIT);
            /* If not intercepted, not implemented -- raise #UD. */
            goto illegal_op;

        case 0xdf: /* INVLPGA */
            if (!SVME(s) || !PE(s)) {
                goto illegal_op;
            }
            if (!check_cpl0(s)) {
                break;
            }
            gen_svm_check_intercept(s, SVM_EXIT_INVLPGA);
            if (s->aflag == MO_64) {
                tcg_gen_mov_tl(s->A0, cpu_regs[R_EAX]);
            } else {
                tcg_gen_ext32u_tl(s->A0, cpu_regs[R_EAX]);
            }
            gen_helper_flush_page(cpu_env, s->A0);
            s->base.is_jmp = DISAS_EOB_NEXT;
            break;

        CASE_MODRM_MEM_OP(2): /* lgdt */
            if (!check_cpl0(s)) {
                break;
            }
            gen_svm_check_intercept(s, SVM_EXIT_GDTR_WRITE);
            gen_lea_modrm(env, s, modrm);
            gen_op_ld_v(s, MO_16, s->T1, s->A0);
            gen_add_A0_im(s, 2);
            gen_op_ld_v(s, CODE64(s) + MO_32, s->T0, s->A0);
            if (dflag == MO_16) {
                tcg_gen_andi_tl(s->T0, s->T0, 0xffffff);
            }
            tcg_gen_st_tl(s->T0, cpu_env, offsetof(CPUX86State, gdt.base));
            tcg_gen_st32_tl(s->T1, cpu_env, offsetof(CPUX86State, gdt.limit));
            break;

        CASE_MODRM_MEM_OP(3): /* lidt */
            if (!check_cpl0(s)) {
                break;
            }
            gen_svm_check_intercept(s, SVM_EXIT_IDTR_WRITE);
            gen_lea_modrm(env, s, modrm);
            gen_op_ld_v(s, MO_16, s->T1, s->A0);
            gen_add_A0_im(s, 2);
            gen_op_ld_v(s, CODE64(s) + MO_32, s->T0, s->A0);
            if (dflag == MO_16) {
                tcg_gen_andi_tl(s->T0, s->T0, 0xffffff);
            }
            tcg_gen_st_tl(s->T0, cpu_env, offsetof(CPUX86State, idt.base));
            tcg_gen_st32_tl(s->T1, cpu_env, offsetof(CPUX86State, idt.limit));
            break;

        CASE_MODRM_OP(4): /* smsw */
            if (s->flags & HF_UMIP_MASK && !check_cpl0(s)) {
                break;
            }
            gen_svm_check_intercept(s, SVM_EXIT_READ_CR0);
            tcg_gen_ld_tl(s->T0, cpu_env, offsetof(CPUX86State, cr[0]));
            /*
             * In 32-bit mode, the higher 16 bits of the destination
             * register are undefined.  In practice CR0[31:0] is stored
             * just like in 64-bit mode.
             */
            mod = (modrm >> 6) & 3;
            ot = (mod != 3 ? MO_16 : s->dflag);
            gen_ldst_modrm(env, s, modrm, ot, OR_TMP0, 1);
            break;
        case 0xee: /* rdpkru */
            if (prefixes & PREFIX_LOCK) {
                goto illegal_op;
            }
            tcg_gen_trunc_tl_i32(s->tmp2_i32, cpu_regs[R_ECX]);
            gen_helper_rdpkru(s->tmp1_i64, cpu_env, s->tmp2_i32);
            tcg_gen_extr_i64_tl(cpu_regs[R_EAX], cpu_regs[R_EDX], s->tmp1_i64);
            break;
        case 0xef: /* wrpkru */
            if (prefixes & PREFIX_LOCK) {
                goto illegal_op;
            }
            tcg_gen_concat_tl_i64(s->tmp1_i64, cpu_regs[R_EAX],
                                  cpu_regs[R_EDX]);
            tcg_gen_trunc_tl_i32(s->tmp2_i32, cpu_regs[R_ECX]);
            gen_helper_wrpkru(cpu_env, s->tmp2_i32, s->tmp1_i64);
            break;

        CASE_MODRM_OP(6): /* lmsw */
            if (!check_cpl0(s)) {
                break;
            }
            gen_svm_check_intercept(s, SVM_EXIT_WRITE_CR0);
            gen_ldst_modrm(env, s, modrm, MO_16, OR_TMP0, 0);
            /*
             * Only the 4 lower bits of CR0 are modified.
             * PE cannot be set to zero if already set to one.
             */
            tcg_gen_ld_tl(s->T1, cpu_env, offsetof(CPUX86State, cr[0]));
            tcg_gen_andi_tl(s->T0, s->T0, 0xf);
            tcg_gen_andi_tl(s->T1, s->T1, ~0xe);
            tcg_gen_or_tl(s->T0, s->T0, s->T1);
            gen_helper_write_crN(cpu_env, tcg_constant_i32(0), s->T0);
            s->base.is_jmp = DISAS_EOB_NEXT;
            break;

        CASE_MODRM_MEM_OP(7): /* invlpg */
            if (!check_cpl0(s)) {
                break;
            }
            gen_svm_check_intercept(s, SVM_EXIT_INVLPG);
            gen_lea_modrm(env, s, modrm);
            gen_helper_flush_page(cpu_env, s->A0);
            s->base.is_jmp = DISAS_EOB_NEXT;
            break;

        case 0xf8: /* swapgs */
#ifdef TARGET_X86_64
            if (CODE64(s)) {
                if (check_cpl0(s)) {
                    tcg_gen_mov_tl(s->T0, cpu_seg_base[R_GS]);
                    tcg_gen_ld_tl(cpu_seg_base[R_GS], cpu_env,
                                  offsetof(CPUX86State, kernelgsbase));
                    tcg_gen_st_tl(s->T0, cpu_env,
                                  offsetof(CPUX86State, kernelgsbase));
                }
                break;
            }
#endif
            goto illegal_op;

        case 0xf9: /* rdtscp */
            if (!(s->cpuid_ext2_features & CPUID_EXT2_RDTSCP)) {
                goto illegal_op;
            }
            gen_update_cc_op(s);
            gen_update_eip_cur(s);
            if (tb_cflags(s->base.tb) & CF_USE_ICOUNT) {
                gen_io_start();
                s->base.is_jmp = DISAS_TOO_MANY;
            }
            gen_helper_rdtscp(cpu_env);
            break;

        default:
            goto unknown_op;
        }
        break;

    case 0x108: /* invd */
    case 0x109: /* wbinvd */
        if (check_cpl0(s)) {
            gen_svm_check_intercept(s, (b & 2) ? SVM_EXIT_INVD : SVM_EXIT_WBINVD);
            /* nothing to do */
        }
        break;
    case 0x63: /* arpl or movslS (x86_64) */
#ifdef TARGET_X86_64
        if (CODE64(s)) {
            int d_ot;
            /* d_ot is the size of destination */
            d_ot = dflag;

            modrm = x86_ldub_code(env, s);
            reg = ((modrm >> 3) & 7) | REX_R(s);
            mod = (modrm >> 6) & 3;
            rm = (modrm & 7) | REX_B(s);

            if (mod == 3) {
                gen_op_mov_v_reg(s, MO_32, s->T0, rm);
                /* sign extend */
                if (d_ot == MO_64) {
                    tcg_gen_ext32s_tl(s->T0, s->T0);
                }
                gen_op_mov_reg_v(s, d_ot, reg, s->T0);
            } else {
                gen_lea_modrm(env, s, modrm);
                gen_op_ld_v(s, MO_32 | MO_SIGN, s->T0, s->A0);
                gen_op_mov_reg_v(s, d_ot, reg, s->T0);
            }
        } else
#endif
        {
            TCGLabel *label1;
            TCGv t0, t1, t2, a0;

            if (!PE(s) || VM86(s))
                goto illegal_op;
            t0 = tcg_temp_local_new();
            t1 = tcg_temp_local_new();
            t2 = tcg_temp_local_new();
            ot = MO_16;
            modrm = x86_ldub_code(env, s);
            reg = (modrm >> 3) & 7;
            mod = (modrm >> 6) & 3;
            rm = modrm & 7;
            if (mod != 3) {
                gen_lea_modrm(env, s, modrm);
                gen_op_ld_v(s, ot, t0, s->A0);
                a0 = tcg_temp_local_new();
                tcg_gen_mov_tl(a0, s->A0);
            } else {
                gen_op_mov_v_reg(s, ot, t0, rm);
                a0 = NULL;
            }
            gen_op_mov_v_reg(s, ot, t1, reg);
            tcg_gen_andi_tl(s->tmp0, t0, 3);
            tcg_gen_andi_tl(t1, t1, 3);
            tcg_gen_movi_tl(t2, 0);
            label1 = gen_new_label();
            tcg_gen_brcond_tl(TCG_COND_GE, s->tmp0, t1, label1);
            tcg_gen_andi_tl(t0, t0, ~3);
            tcg_gen_or_tl(t0, t0, t1);
            tcg_gen_movi_tl(t2, CC_Z);
            gen_set_label(label1);
            if (mod != 3) {
                gen_op_st_v(s, ot, t0, a0);
                tcg_temp_free(a0);
           } else {
                gen_op_mov_reg_v(s, ot, rm, t0);
            }
            gen_compute_eflags(s);
            tcg_gen_andi_tl(cpu_cc_src, cpu_cc_src, ~CC_Z);
            tcg_gen_or_tl(cpu_cc_src, cpu_cc_src, t2);
            tcg_temp_free(t0);
            tcg_temp_free(t1);
            tcg_temp_free(t2);
        }
        break;
    case 0x102: /* lar */
    case 0x103: /* lsl */
        {
            TCGLabel *label1;
            TCGv t0;
            if (!PE(s) || VM86(s))
                goto illegal_op;
            ot = dflag != MO_16 ? MO_32 : MO_16;
            modrm = x86_ldub_code(env, s);
            reg = ((modrm >> 3) & 7) | REX_R(s);
            gen_ldst_modrm(env, s, modrm, MO_16, OR_TMP0, 0);
            t0 = tcg_temp_local_new();
            gen_update_cc_op(s);
            if (b == 0x102) {
                gen_helper_lar(t0, cpu_env, s->T0);
            } else {
                gen_helper_lsl(t0, cpu_env, s->T0);
            }
            tcg_gen_andi_tl(s->tmp0, cpu_cc_src, CC_Z);
            label1 = gen_new_label();
            tcg_gen_brcondi_tl(TCG_COND_EQ, s->tmp0, 0, label1);
            gen_op_mov_reg_v(s, ot, reg, t0);
            gen_set_label(label1);
            set_cc_op(s, CC_OP_EFLAGS);
            tcg_temp_free(t0);
        }
        break;
    case 0x118:
        modrm = x86_ldub_code(env, s);
        mod = (modrm >> 6) & 3;
        op = (modrm >> 3) & 7;
        switch(op) {
        case 0: /* prefetchnta */
        case 1: /* prefetchnt0 */
        case 2: /* prefetchnt0 */
        case 3: /* prefetchnt0 */
            if (mod == 3)
                goto illegal_op;
            gen_nop_modrm(env, s, modrm);
            /* nothing more to do */
            break;
        default: /* nop (multi byte) */
            gen_nop_modrm(env, s, modrm);
            break;
        }
        break;
    case 0x11a:
        modrm = x86_ldub_code(env, s);
        if (s->flags & HF_MPX_EN_MASK) {
            mod = (modrm >> 6) & 3;
            reg = ((modrm >> 3) & 7) | REX_R(s);
            if (prefixes & PREFIX_REPZ) {
                /* bndcl */
                if (reg >= 4
                    || (prefixes & PREFIX_LOCK)
                    || s->aflag == MO_16) {
                    goto illegal_op;
                }
                gen_bndck(env, s, modrm, TCG_COND_LTU, cpu_bndl[reg]);
            } else if (prefixes & PREFIX_REPNZ) {
                /* bndcu */
                if (reg >= 4
                    || (prefixes & PREFIX_LOCK)
                    || s->aflag == MO_16) {
                    goto illegal_op;
                }
                TCGv_i64 notu = tcg_temp_new_i64();
                tcg_gen_not_i64(notu, cpu_bndu[reg]);
                gen_bndck(env, s, modrm, TCG_COND_GTU, notu);
                tcg_temp_free_i64(notu);
            } else if (prefixes & PREFIX_DATA) {
                /* bndmov -- from reg/mem */
                if (reg >= 4 || s->aflag == MO_16) {
                    goto illegal_op;
                }
                if (mod == 3) {
                    int reg2 = (modrm & 7) | REX_B(s);
                    if (reg2 >= 4 || (prefixes & PREFIX_LOCK)) {
                        goto illegal_op;
                    }
                    if (s->flags & HF_MPX_IU_MASK) {
                        tcg_gen_mov_i64(cpu_bndl[reg], cpu_bndl[reg2]);
                        tcg_gen_mov_i64(cpu_bndu[reg], cpu_bndu[reg2]);
                    }
                } else {
                    gen_lea_modrm(env, s, modrm);
                    if (CODE64(s)) {
                        tcg_gen_qemu_ld_i64(cpu_bndl[reg], s->A0,
                                            s->mem_index, MO_LEUQ);
                        tcg_gen_addi_tl(s->A0, s->A0, 8);
                        tcg_gen_qemu_ld_i64(cpu_bndu[reg], s->A0,
                                            s->mem_index, MO_LEUQ);
                    } else {
                        tcg_gen_qemu_ld_i64(cpu_bndl[reg], s->A0,
                                            s->mem_index, MO_LEUL);
                        tcg_gen_addi_tl(s->A0, s->A0, 4);
                        tcg_gen_qemu_ld_i64(cpu_bndu[reg], s->A0,
                                            s->mem_index, MO_LEUL);
                    }
                    /* bnd registers are now in-use */
                    gen_set_hflag(s, HF_MPX_IU_MASK);
                }
            } else if (mod != 3) {
                /* bndldx */
                AddressParts a = gen_lea_modrm_0(env, s, modrm);
                if (reg >= 4
                    || (prefixes & PREFIX_LOCK)
                    || s->aflag == MO_16
                    || a.base < -1) {
                    goto illegal_op;
                }
                if (a.base >= 0) {
                    tcg_gen_addi_tl(s->A0, cpu_regs[a.base], a.disp);
                } else {
                    tcg_gen_movi_tl(s->A0, 0);
                }
                gen_lea_v_seg(s, s->aflag, s->A0, a.def_seg, s->override);
                if (a.index >= 0) {
                    tcg_gen_mov_tl(s->T0, cpu_regs[a.index]);
                } else {
                    tcg_gen_movi_tl(s->T0, 0);
                }
                if (CODE64(s)) {
                    gen_helper_bndldx64(cpu_bndl[reg], cpu_env, s->A0, s->T0);
                    tcg_gen_ld_i64(cpu_bndu[reg], cpu_env,
                                   offsetof(CPUX86State, mmx_t0.MMX_Q(0)));
                } else {
                    gen_helper_bndldx32(cpu_bndu[reg], cpu_env, s->A0, s->T0);
                    tcg_gen_ext32u_i64(cpu_bndl[reg], cpu_bndu[reg]);
                    tcg_gen_shri_i64(cpu_bndu[reg], cpu_bndu[reg], 32);
                }
                gen_set_hflag(s, HF_MPX_IU_MASK);
            }
        }
        gen_nop_modrm(env, s, modrm);
        break;
    case 0x11b:
        modrm = x86_ldub_code(env, s);
        if (s->flags & HF_MPX_EN_MASK) {
            mod = (modrm >> 6) & 3;
            reg = ((modrm >> 3) & 7) | REX_R(s);
            if (mod != 3 && (prefixes & PREFIX_REPZ)) {
                /* bndmk */
                if (reg >= 4
                    || (prefixes & PREFIX_LOCK)
                    || s->aflag == MO_16) {
                    goto illegal_op;
                }
                AddressParts a = gen_lea_modrm_0(env, s, modrm);
                if (a.base >= 0) {
                    tcg_gen_extu_tl_i64(cpu_bndl[reg], cpu_regs[a.base]);
                    if (!CODE64(s)) {
                        tcg_gen_ext32u_i64(cpu_bndl[reg], cpu_bndl[reg]);
                    }
                } else if (a.base == -1) {
                    /* no base register has lower bound of 0 */
                    tcg_gen_movi_i64(cpu_bndl[reg], 0);
                } else {
                    /* rip-relative generates #ud */
                    goto illegal_op;
                }
                tcg_gen_not_tl(s->A0, gen_lea_modrm_1(s, a, false));
                if (!CODE64(s)) {
                    tcg_gen_ext32u_tl(s->A0, s->A0);
                }
                tcg_gen_extu_tl_i64(cpu_bndu[reg], s->A0);
                /* bnd registers are now in-use */
                gen_set_hflag(s, HF_MPX_IU_MASK);
                break;
            } else if (prefixes & PREFIX_REPNZ) {
                /* bndcn */
                if (reg >= 4
                    || (prefixes & PREFIX_LOCK)
                    || s->aflag == MO_16) {
                    goto illegal_op;
                }
                gen_bndck(env, s, modrm, TCG_COND_GTU, cpu_bndu[reg]);
            } else if (prefixes & PREFIX_DATA) {
                /* bndmov -- to reg/mem */
                if (reg >= 4 || s->aflag == MO_16) {
                    goto illegal_op;
                }
                if (mod == 3) {
                    int reg2 = (modrm & 7) | REX_B(s);
                    if (reg2 >= 4 || (prefixes & PREFIX_LOCK)) {
                        goto illegal_op;
                    }
                    if (s->flags & HF_MPX_IU_MASK) {
                        tcg_gen_mov_i64(cpu_bndl[reg2], cpu_bndl[reg]);
                        tcg_gen_mov_i64(cpu_bndu[reg2], cpu_bndu[reg]);
                    }
                } else {
                    gen_lea_modrm(env, s, modrm);
                    if (CODE64(s)) {
                        tcg_gen_qemu_st_i64(cpu_bndl[reg], s->A0,
                                            s->mem_index, MO_LEUQ);
                        tcg_gen_addi_tl(s->A0, s->A0, 8);
                        tcg_gen_qemu_st_i64(cpu_bndu[reg], s->A0,
                                            s->mem_index, MO_LEUQ);
                    } else {
                        tcg_gen_qemu_st_i64(cpu_bndl[reg], s->A0,
                                            s->mem_index, MO_LEUL);
                        tcg_gen_addi_tl(s->A0, s->A0, 4);
                        tcg_gen_qemu_st_i64(cpu_bndu[reg], s->A0,
                                            s->mem_index, MO_LEUL);
                    }
                }
            } else if (mod != 3) {
                /* bndstx */
                AddressParts a = gen_lea_modrm_0(env, s, modrm);
                if (reg >= 4
                    || (prefixes & PREFIX_LOCK)
                    || s->aflag == MO_16
                    || a.base < -1) {
                    goto illegal_op;
                }
                if (a.base >= 0) {
                    tcg_gen_addi_tl(s->A0, cpu_regs[a.base], a.disp);
                } else {
                    tcg_gen_movi_tl(s->A0, 0);
                }
                gen_lea_v_seg(s, s->aflag, s->A0, a.def_seg, s->override);
                if (a.index >= 0) {
                    tcg_gen_mov_tl(s->T0, cpu_regs[a.index]);
                } else {
                    tcg_gen_movi_tl(s->T0, 0);
                }
                if (CODE64(s)) {
                    gen_helper_bndstx64(cpu_env, s->A0, s->T0,
                                        cpu_bndl[reg], cpu_bndu[reg]);
                } else {
                    gen_helper_bndstx32(cpu_env, s->A0, s->T0,
                                        cpu_bndl[reg], cpu_bndu[reg]);
                }
            }
        }
        gen_nop_modrm(env, s, modrm);
        break;
    case 0x119: case 0x11c ... 0x11f: /* nop (multi byte) */
        modrm = x86_ldub_code(env, s);
        gen_nop_modrm(env, s, modrm);
        break;

    case 0x120: /* mov reg, crN */
    case 0x122: /* mov crN, reg */
        if (!check_cpl0(s)) {
            break;
        }
        modrm = x86_ldub_code(env, s);
        /*
         * Ignore the mod bits (assume (modrm&0xc0)==0xc0).
         * AMD documentation (24594.pdf) and testing of Intel 386 and 486
         * processors all show that the mod bits are assumed to be 1's,
         * regardless of actual values.
         */
        rm = (modrm & 7) | REX_B(s);
        reg = ((modrm >> 3) & 7) | REX_R(s);
        switch (reg) {
        case 0:
            if ((prefixes & PREFIX_LOCK) &&
                (s->cpuid_ext3_features & CPUID_EXT3_CR8LEG)) {
                reg = 8;
            }
            break;
        case 2:
        case 3:
        case 4:
        case 8:
            break;
        default:
            goto unknown_op;
        }
        ot  = (CODE64(s) ? MO_64 : MO_32);

        if (tb_cflags(s->base.tb) & CF_USE_ICOUNT) {
            gen_io_start();
            s->base.is_jmp = DISAS_TOO_MANY;
        }
        if (b & 2) {
            gen_svm_check_intercept(s, SVM_EXIT_WRITE_CR0 + reg);
            gen_op_mov_v_reg(s, ot, s->T0, rm);
            gen_helper_write_crN(cpu_env, tcg_constant_i32(reg), s->T0);
            s->base.is_jmp = DISAS_EOB_NEXT;
        } else {
            gen_svm_check_intercept(s, SVM_EXIT_READ_CR0 + reg);
            gen_helper_read_crN(s->T0, cpu_env, tcg_constant_i32(reg));
            gen_op_mov_reg_v(s, ot, rm, s->T0);
        }
        break;

    case 0x121: /* mov reg, drN */
    case 0x123: /* mov drN, reg */
        if (check_cpl0(s)) {
            modrm = x86_ldub_code(env, s);
            /* Ignore the mod bits (assume (modrm&0xc0)==0xc0).
             * AMD documentation (24594.pdf) and testing of
             * intel 386 and 486 processors all show that the mod bits
             * are assumed to be 1's, regardless of actual values.
             */
            rm = (modrm & 7) | REX_B(s);
            reg = ((modrm >> 3) & 7) | REX_R(s);
            if (CODE64(s))
                ot = MO_64;
            else
                ot = MO_32;
            if (reg >= 8) {
                goto illegal_op;
            }
            if (b & 2) {
                gen_svm_check_intercept(s, SVM_EXIT_WRITE_DR0 + reg);
                gen_op_mov_v_reg(s, ot, s->T0, rm);
                tcg_gen_movi_i32(s->tmp2_i32, reg);
                gen_helper_set_dr(cpu_env, s->tmp2_i32, s->T0);
                s->base.is_jmp = DISAS_EOB_NEXT;
            } else {
                gen_svm_check_intercept(s, SVM_EXIT_READ_DR0 + reg);
                tcg_gen_movi_i32(s->tmp2_i32, reg);
                gen_helper_get_dr(s->T0, cpu_env, s->tmp2_i32);
                gen_op_mov_reg_v(s, ot, rm, s->T0);
            }
        }
        break;
    case 0x106: /* clts */
        if (check_cpl0(s)) {
            gen_svm_check_intercept(s, SVM_EXIT_WRITE_CR0);
            gen_helper_clts(cpu_env);
            /* abort block because static cpu state changed */
            s->base.is_jmp = DISAS_EOB_NEXT;
        }
        break;
    /* MMX/3DNow!/SSE/SSE2/SSE3/SSSE3/SSE4 support */
    case 0x1c3: /* MOVNTI reg, mem */
        if (!(s->cpuid_features & CPUID_SSE2))
            goto illegal_op;
        ot = mo_64_32(dflag);
        modrm = x86_ldub_code(env, s);
        mod = (modrm >> 6) & 3;
        if (mod == 3)
            goto illegal_op;
        reg = ((modrm >> 3) & 7) | REX_R(s);
        /* generate a generic store */
        gen_ldst_modrm(env, s, modrm, ot, reg, 1);
        break;
    case 0x1ae:
        modrm = x86_ldub_code(env, s);
        switch (modrm) {
        CASE_MODRM_MEM_OP(0): /* fxsave */
            if (!(s->cpuid_features & CPUID_FXSR)
                || (prefixes & PREFIX_LOCK)) {
                goto illegal_op;
            }
            if ((s->flags & HF_EM_MASK) || (s->flags & HF_TS_MASK)) {
                gen_exception(s, EXCP07_PREX);
                break;
            }
            gen_lea_modrm(env, s, modrm);
            gen_helper_fxsave(cpu_env, s->A0);
            break;

        CASE_MODRM_MEM_OP(1): /* fxrstor */
            if (!(s->cpuid_features & CPUID_FXSR)
                || (prefixes & PREFIX_LOCK)) {
                goto illegal_op;
            }
            if ((s->flags & HF_EM_MASK) || (s->flags & HF_TS_MASK)) {
                gen_exception(s, EXCP07_PREX);
                break;
            }
            gen_lea_modrm(env, s, modrm);
            gen_helper_fxrstor(cpu_env, s->A0);
            break;

        CASE_MODRM_MEM_OP(2): /* ldmxcsr */
            if ((s->flags & HF_EM_MASK) || !(s->flags & HF_OSFXSR_MASK)) {
                goto illegal_op;
            }
            if (s->flags & HF_TS_MASK) {
                gen_exception(s, EXCP07_PREX);
                break;
            }
            gen_lea_modrm(env, s, modrm);
            tcg_gen_qemu_ld_i32(s->tmp2_i32, s->A0, s->mem_index, MO_LEUL);
            gen_helper_ldmxcsr(cpu_env, s->tmp2_i32);
            break;

        CASE_MODRM_MEM_OP(3): /* stmxcsr */
            if ((s->flags & HF_EM_MASK) || !(s->flags & HF_OSFXSR_MASK)) {
                goto illegal_op;
            }
            if (s->flags & HF_TS_MASK) {
                gen_exception(s, EXCP07_PREX);
                break;
            }
            gen_helper_update_mxcsr(cpu_env);
            gen_lea_modrm(env, s, modrm);
            tcg_gen_ld32u_tl(s->T0, cpu_env, offsetof(CPUX86State, mxcsr));
            gen_op_st_v(s, MO_32, s->T0, s->A0);
            break;

        CASE_MODRM_MEM_OP(4): /* xsave */
            if ((s->cpuid_ext_features & CPUID_EXT_XSAVE) == 0
                || (prefixes & (PREFIX_LOCK | PREFIX_DATA
                                | PREFIX_REPZ | PREFIX_REPNZ))) {
                goto illegal_op;
            }
            gen_lea_modrm(env, s, modrm);
            tcg_gen_concat_tl_i64(s->tmp1_i64, cpu_regs[R_EAX],
                                  cpu_regs[R_EDX]);
            gen_helper_xsave(cpu_env, s->A0, s->tmp1_i64);
            break;

        CASE_MODRM_MEM_OP(5): /* xrstor */
            if ((s->cpuid_ext_features & CPUID_EXT_XSAVE) == 0
                || (prefixes & (PREFIX_LOCK | PREFIX_DATA
                                | PREFIX_REPZ | PREFIX_REPNZ))) {
                goto illegal_op;
            }
            gen_lea_modrm(env, s, modrm);
            tcg_gen_concat_tl_i64(s->tmp1_i64, cpu_regs[R_EAX],
                                  cpu_regs[R_EDX]);
            gen_helper_xrstor(cpu_env, s->A0, s->tmp1_i64);
            /* XRSTOR is how MPX is enabled, which changes how
               we translate.  Thus we need to end the TB.  */
            s->base.is_jmp = DISAS_EOB_NEXT;
            break;

        CASE_MODRM_MEM_OP(6): /* xsaveopt / clwb */
            if (prefixes & PREFIX_LOCK) {
                goto illegal_op;
            }
            if (prefixes & PREFIX_DATA) {
                /* clwb */
                if (!(s->cpuid_7_0_ebx_features & CPUID_7_0_EBX_CLWB)) {
                    goto illegal_op;
                }
                gen_nop_modrm(env, s, modrm);
            } else {
                /* xsaveopt */
                if ((s->cpuid_ext_features & CPUID_EXT_XSAVE) == 0
                    || (s->cpuid_xsave_features & CPUID_XSAVE_XSAVEOPT) == 0
                    || (prefixes & (PREFIX_REPZ | PREFIX_REPNZ))) {
                    goto illegal_op;
                }
                gen_lea_modrm(env, s, modrm);
                tcg_gen_concat_tl_i64(s->tmp1_i64, cpu_regs[R_EAX],
                                      cpu_regs[R_EDX]);
                gen_helper_xsaveopt(cpu_env, s->A0, s->tmp1_i64);
            }
            break;

        CASE_MODRM_MEM_OP(7): /* clflush / clflushopt */
            if (prefixes & PREFIX_LOCK) {
                goto illegal_op;
            }
            if (prefixes & PREFIX_DATA) {
                /* clflushopt */
                if (!(s->cpuid_7_0_ebx_features & CPUID_7_0_EBX_CLFLUSHOPT)) {
                    goto illegal_op;
                }
            } else {
                /* clflush */
                if ((s->prefix & (PREFIX_REPZ | PREFIX_REPNZ))
                    || !(s->cpuid_features & CPUID_CLFLUSH)) {
                    goto illegal_op;
                }
            }
            gen_nop_modrm(env, s, modrm);
            break;

        case 0xc0 ... 0xc7: /* rdfsbase (f3 0f ae /0) */
        case 0xc8 ... 0xcf: /* rdgsbase (f3 0f ae /1) */
        case 0xd0 ... 0xd7: /* wrfsbase (f3 0f ae /2) */
        case 0xd8 ... 0xdf: /* wrgsbase (f3 0f ae /3) */
            if (CODE64(s)
                && (prefixes & PREFIX_REPZ)
                && !(prefixes & PREFIX_LOCK)
                && (s->cpuid_7_0_ebx_features & CPUID_7_0_EBX_FSGSBASE)) {
                TCGv base, treg, src, dst;

                /* Preserve hflags bits by testing CR4 at runtime.  */
                tcg_gen_movi_i32(s->tmp2_i32, CR4_FSGSBASE_MASK);
                gen_helper_cr4_testbit(cpu_env, s->tmp2_i32);

                base = cpu_seg_base[modrm & 8 ? R_GS : R_FS];
                treg = cpu_regs[(modrm & 7) | REX_B(s)];

                if (modrm & 0x10) {
                    /* wr*base */
                    dst = base, src = treg;
                } else {
                    /* rd*base */
                    dst = treg, src = base;
                }

                if (s->dflag == MO_32) {
                    tcg_gen_ext32u_tl(dst, src);
                } else {
                    tcg_gen_mov_tl(dst, src);
                }
                break;
            }
            goto unknown_op;

        case 0xf8: /* sfence / pcommit */
            if (prefixes & PREFIX_DATA) {
                /* pcommit */
                if (!(s->cpuid_7_0_ebx_features & CPUID_7_0_EBX_PCOMMIT)
                    || (prefixes & PREFIX_LOCK)) {
                    goto illegal_op;
                }
                break;
            }
            /* fallthru */
        case 0xf9 ... 0xff: /* sfence */
            if (!(s->cpuid_features & CPUID_SSE)
                || (prefixes & PREFIX_LOCK)) {
                goto illegal_op;
            }
            tcg_gen_mb(TCG_MO_ST_ST | TCG_BAR_SC);
            break;
        case 0xe8 ... 0xef: /* lfence */
            if (!(s->cpuid_features & CPUID_SSE)
                || (prefixes & PREFIX_LOCK)) {
                goto illegal_op;
            }
            tcg_gen_mb(TCG_MO_LD_LD | TCG_BAR_SC);
            break;
        case 0xf0 ... 0xf7: /* mfence */
            if (!(s->cpuid_features & CPUID_SSE2)
                || (prefixes & PREFIX_LOCK)) {
                goto illegal_op;
            }
            tcg_gen_mb(TCG_MO_ALL | TCG_BAR_SC);
            break;

        default:
            goto unknown_op;
        }
        break;

    case 0x10d: /* 3DNow! prefetch(w) */
        modrm = x86_ldub_code(env, s);
        mod = (modrm >> 6) & 3;
        if (mod == 3)
            goto illegal_op;
        gen_nop_modrm(env, s, modrm);
        break;
    case 0x1aa: /* rsm */
        gen_svm_check_intercept(s, SVM_EXIT_RSM);
        if (!(s->flags & HF_SMM_MASK))
            goto illegal_op;
#ifdef CONFIG_USER_ONLY
        /* we should not be in SMM mode */
        g_assert_not_reached();
#else
        gen_update_cc_op(s);
        gen_update_eip_next(s);
        gen_helper_rsm(cpu_env);
#endif /* CONFIG_USER_ONLY */
        s->base.is_jmp = DISAS_EOB_ONLY;
        break;
    case 0x1b8: /* SSE4.2 popcnt */
        if ((prefixes & (PREFIX_REPZ | PREFIX_LOCK | PREFIX_REPNZ)) !=
             PREFIX_REPZ)
            goto illegal_op;
        if (!(s->cpuid_ext_features & CPUID_EXT_POPCNT))
            goto illegal_op;

        modrm = x86_ldub_code(env, s);
        reg = ((modrm >> 3) & 7) | REX_R(s);

        if (s->prefix & PREFIX_DATA) {
            ot = MO_16;
        } else {
            ot = mo_64_32(dflag);
        }

        gen_ldst_modrm(env, s, modrm, ot, OR_TMP0, 0);
        gen_extu(ot, s->T0);
        tcg_gen_mov_tl(cpu_cc_src, s->T0);
        tcg_gen_ctpop_tl(s->T0, s->T0);
        gen_op_mov_reg_v(s, ot, reg, s->T0);

        set_cc_op(s, CC_OP_POPCNT);
        break;
    case 0x10e ... 0x117:
    case 0x128 ... 0x12f:
    case 0x138 ... 0x13a:
    case 0x150 ... 0x179:
    case 0x17c ... 0x17f:
    case 0x1c2:
    case 0x1c4 ... 0x1c6:
    case 0x1d0 ... 0x1fe:
        disas_insn_new(s, cpu, b);
        break;
    default:
        goto unknown_op;
    }
    return true;
 illegal_op:
    gen_illegal_opcode(s);
    return true;
 unknown_op:
    gen_unknown_opcode(env, s);
    return true;
}
```