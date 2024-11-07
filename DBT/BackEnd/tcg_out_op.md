```c
static void tcg_out_op(TCGContext *s, TCGOpcode opc, const TCGArg args[TCG_MAX_OP_ARGS],const int const_args[TCG_MAX_OP_ARGS])

{

    /* 99% of the time, we can signal the use of extension registers

       by looking to see if the opcode handles 64-bit data.  */

    TCGType ext = (tcg_op_defs[opc].flags & TCG_OPF_64BIT) != 0;

    /* Hoist the loads of the most common arguments.  */

    TCGArg a0 = args[0];

    TCGArg a1 = args[1];

    TCGArg a2 = args[2];

    int c2 = const_args[2];

  

    /* Some operands are defined with "rZ" constraint, a register or

       the zero register.  These need not actually test args[I] == 0.  */

    #define REG0(I)  (const_args[I] ? TCG_REG_ZERO : (TCGReg)args[I])

  

    switch (opc) {

    case INDEX_op_exit_tb: /* sw */

        /* Reuse the zeroing that exists for goto_ptr.  */

        if (a0 == 0) {

            tcg_out_goto_long(s, tcg_code_gen_epilogue);

        } else {

            tcg_out_movi(s, TCG_TYPE_I64, TCG_REG_X0, a0);

            tcg_out_goto_long(s, tb_ret_addr);

        }

        break;

  

    case INDEX_op_goto_tb: /* sw */

        if (s->tb_jmp_insn_offset != NULL) {

            /* TCG_TARGET_HAS_direct_jump */

            /* Ensure that ADRP+ADD are 8-byte aligned so that an atomic

               write can be used to patch the target address. */

            if ((uintptr_t)s->code_ptr & 7) {

                tcg_out32(s, OPC_NOP);

            }

            s->tb_jmp_insn_offset[a0] = tcg_current_code_size(s);

            tcg_out32(s, OPC_NOP);

            tcg_out32(s, OPC_NOP);

            tcg_out32(s, OPC_NOP);

            tcg_out32(s, OPC_NOP);//total 2 nop for tb_target

        } else {

            /* !TCG_TARGET_HAS_direct_jump */

            tcg_debug_assert(s->tb_jmp_target_addr != NULL);

        tcg_out_ld(s, TCG_TYPE_PTR, TCG_REG_TMP, TCG_REG_ZERO, (uintptr_t)(s->tb_jmp_target_addr + a0));

        }

        tcg_out_insn_jump(s, OPC_JMP, TCG_REG_ZERO, TCG_REG_TMP, noPara);

        set_jmp_reset_offset(s, a0);

        break;

  

    case INDEX_op_goto_ptr:

        tcg_out_insn_jump(s, OPC_JMP, TCG_REG_ZERO, a0, noPara);

        break;

  

    case INDEX_op_br:

        tcg_out_goto_label(s, arg_label(a0));

        break;

  

    case INDEX_op_ld8u_i32:

    case INDEX_op_ld8u_i64:

        tcg_out_ldst(s, OPC_LDBU, a0, a1, a2, 0);

        break;

    case INDEX_op_ld8s_i32:

        tcg_out_ldst(s, OPC_LDBU, a0, a1, a2, 1);

        tcg_out_insn_simpleImm(s, OPC_ZAPNOT_I, a0, a0, 0xf);

        break;

    case INDEX_op_ld8s_i64:

        tcg_out_ldst(s, OPC_LDBU, a0, a1, a2, 1);

        break;

    case INDEX_op_ld16u_i32:

    case INDEX_op_ld16u_i64:

        tcg_out_ldst(s, OPC_LDHU, a0, a1, a2, 0);

        break;

    case INDEX_op_ld16s_i32:

        tcg_out_ldst(s, OPC_LDHU, a0, a1, a2, 1);

        tcg_out_insn_simpleImm(s, OPC_ZAPNOT_I, a0, a0, 0xf);

        break;

    case INDEX_op_ld16s_i64:

        tcg_out_ldst(s, OPC_LDHU, a0, a1, a2, 1);

        break;

    case INDEX_op_ld_i32:

        tcg_out_ldst(s, OPC_LDW, a0, a1, a2, 0);

    break;

    case INDEX_op_ld32u_i64:

        tcg_out_ldst(s, OPC_LDW, a0, a1, a2, 0);

        break;

    case INDEX_op_ld32s_i64:

        tcg_out_ldst(s, OPC_LDW, a0, a1, a2, 1);

        break;

    case INDEX_op_ld_i64:

        tcg_out_ldst(s, OPC_LDL, a0, a1, a2, 1);

        break;

    case INDEX_op_st8_i32:

    case INDEX_op_st8_i64:

        tcg_out_ldst(s, OPC_STB, REG0(0), a1, a2, 0);

        break;

    case INDEX_op_st16_i32:

    case INDEX_op_st16_i64:

        tcg_out_ldst(s, OPC_STH, REG0(0), a1, a2, 0);

        break;

    case INDEX_op_st_i32:

    case INDEX_op_st32_i64:

        tcg_out_ldst(s, OPC_STW, REG0(0), a1, a2, 0);

        break;

    case INDEX_op_st_i64:

        tcg_out_ldst(s, OPC_STL, REG0(0), a1, a2, 0);

        break;

  

    case INDEX_op_add_i32:

        a2 = (int32_t)a2;

        if (c2) {

            tcg_out_addsubi(s, ext, a0, a1, a2);

        } else {

            tcg_out_insn_simpleReg(s, OPC_ADDW, a0, a1, a2);

            tcg_out_insn_simpleImm(s, OPC_ZAPNOT_I, a0, a0, 0xf);

        }

        break;

    case INDEX_op_add_i64:

        if (c2) {

            tcg_out_addsubi(s, ext, a0, a1, a2);

        } else {

            tcg_out_insn_simpleReg(s, OPC_ADDL, a0, a1, a2);

        }

        break;

  

    case INDEX_op_sub_i32:

        a2 = (int32_t)a2;

        if (c2) {

            tcg_out_addsubi(s, ext, a0, a1, -a2);

        } else {

            tcg_out_insn_simpleReg(s, OPC_SUBW, a0, a1, a2);

            tcg_out_insn_simpleImm(s, OPC_ZAPNOT_I, a0, a0, 0xf);

        }

        break;

    case INDEX_op_sub_i64:

        if (c2) {

            tcg_out_addsubi(s, ext, a0, a1, -a2);

        } else {

            tcg_out_insn_simpleReg(s, OPC_SUBL, a0, a1, a2);

        }

        break;

  

    case INDEX_op_neg_i32:

        tcg_out_insn_bitReg(s, OPC_SUBW, a0, TCG_REG_ZERO, a1);

    tcg_out_insn_simpleImm(s, OPC_ZAPNOT_I, a0, a0, 0xf);

        break;

    case INDEX_op_neg_i64:

        tcg_out_insn_bitReg(s, OPC_SUBL, a0, TCG_REG_ZERO, a1);

        break;

  

    case INDEX_op_and_i32:

        if (c2) {

            a2 = (int32_t)a2;

            tcg_out_bit(s, OPC_AND_I, OPC_AND, a0, a1, a2);

        } else {

            tcg_out_insn_bitReg(s, OPC_AND, a0, a1, a2);

        }

    tcg_out_insn_simpleImm(s, OPC_ZAPNOT_I, a0, a0, 0xf);

        break;

    case INDEX_op_and_i64:

        if (c2) {

            tcg_out_bit(s, OPC_AND_I, OPC_AND, a0, a1, a2);

        } else {

            tcg_out_insn_bitReg(s, OPC_AND, a0, a1, a2);

        }

        break;

  

    case INDEX_op_andc_i32:

    case INDEX_op_andc_i64:

        tcg_debug_assert(0);

        break;

  

    case INDEX_op_or_i32:

        if (c2) {

            a2 = (int32_t)a2;

            tcg_out_bit(s, OPC_BIS_I, OPC_BIS, a0, a1, a2);

        } else {

            tcg_out_insn_bitReg(s, OPC_BIS, a0, a1, a2);

        }

    tcg_out_insn_simpleImm(s, OPC_ZAPNOT_I, a0, a0, 0xf);

        break;

    case INDEX_op_or_i64:

        if (c2) {

            tcg_out_bit(s, OPC_BIS_I, OPC_BIS, a0, a1, a2);

        } else {

            tcg_out_insn_bitReg(s, OPC_BIS, a0, a1, a2);

        }

        break;

  

    case INDEX_op_orc_i32:

        if (c2) {

            a2 = (int32_t)a2;

            tcg_out_bit(s, OPC_BIS_I, OPC_BIS, a0, a1, ~a2);

        } else {

            tcg_out_insn_bitReg(s, OPC_ORNOT, a0, a1, a2);

        }

    tcg_out_insn_simpleImm(s, OPC_ZAPNOT_I, a0, a0, 0xf);

        break;

    case INDEX_op_orc_i64:

        if (c2) {

            tcg_out_bit(s, OPC_BIS_I, OPC_BIS, a0, a1, ~a2);

        } else {

            tcg_out_insn_bitReg(s, OPC_ORNOT, a0, a1, a2);

        }

        break;

  

    case INDEX_op_xor_i32:

        if (c2) {

            a2 = (int32_t)a2;

            tcg_out_bit(s, OPC_XOR_I, OPC_XOR, a0, a1, a2);

        } else {

            tcg_out_insn_bitReg(s, OPC_XOR, a0, a1, a2);

        }

    tcg_out_insn_simpleImm(s, OPC_ZAPNOT_I, a0, a0, 0xf);

        break;

    case INDEX_op_xor_i64:

        if (c2) {

            tcg_out_bit(s, OPC_XOR_I, OPC_XOR, a0, a1, a2);

        } else {

            tcg_out_insn_bitReg(s, OPC_XOR, a0, a1, a2);

        }

        break;

  

    case INDEX_op_eqv_i32:

    case INDEX_op_eqv_i64:

        tcg_debug_assert(0);

        break;

  

    case INDEX_op_not_i32:

        tcg_out_insn_bitReg(s, OPC_ORNOT, a0, TCG_REG_ZERO, a1);

    tcg_out_insn_simpleImm(s, OPC_ZAPNOT_I, a0, a0, 0xf);

        break;

    case INDEX_op_not_i64:

        tcg_out_insn_bitReg(s, OPC_ORNOT, a0, TCG_REG_ZERO, a1);

        break;

  

    case INDEX_op_mul_i32:

        tcg_out_insn_simpleReg(s, OPC_MULL, a0, a1, a2);

    tcg_out_insn_simpleImm(s, OPC_ZAPNOT_I, a0, a0, 0xf);

        break;

    case INDEX_op_mul_i64:

        tcg_out_insn_simpleReg(s, OPC_MULL, a0, a1, a2);

        break;

  

    case INDEX_op_div_i32:

    case INDEX_op_div_i64:

        tcg_debug_assert(0);

        break;

    case INDEX_op_divu_i32:

    case INDEX_op_divu_i64:

        tcg_debug_assert(0);

        break;

  

    case INDEX_op_rem_i32:

    case INDEX_op_rem_i64:

        tcg_debug_assert(0);

        break;

    case INDEX_op_remu_i32:

    case INDEX_op_remu_i64:

        tcg_debug_assert(0);

        break;

  

    case INDEX_op_shl_i32: /* sw logical left*/

        if (c2) {

            unsigned int bits = ext ? 64 : 32;

            unsigned int max = bits - 1;

            tcg_out_insn_simpleImm(s, OPC_SLL_I, a0, a1, a2&max);

        tcg_out_insn_simpleImm(s, OPC_ZAPNOT_I, a0, a0, 0xf);

        } else {

            tcg_out_insn_bitReg(s, OPC_SLL, a0, a1, a2);

        tcg_out_insn_simpleImm(s, OPC_ZAPNOT_I, a0, a0, 0xf);

        }

        break;

    case INDEX_op_shl_i64:

        if (c2) {

            unsigned int bits = ext ? 64 : 32;

            unsigned int max = bits - 1;

            tcg_out_insn_simpleImm(s, OPC_SLL_I, a0, a1, a2&max);

        } else {

            tcg_out_insn_bitReg(s, OPC_SLL, a0, a1, a2);

        }

        break;

  

    case INDEX_op_shr_i32: /* sw logical right */

        a2 = (int32_t)a2;

        if (c2) {

            int bits = ext ? 64 : 32;

            int max = bits - 1;

            tcg_out_insn_simpleImm(s, OPC_ZAPNOT_I, a1, a1, 0xf);

            tcg_out_insn_simpleImm(s, OPC_SRL_I, a0, a1, a2&max);

        } else {

            tcg_out_insn_simpleImm(s, OPC_ZAPNOT_I, a1, a1, 0xf);

            tcg_out_insn_bitReg(s, OPC_SRL, a0, a1, a2);

        }

        break;

    case INDEX_op_shr_i64:

        if (c2) {

            int bits = ext ? 64 : 32;

            int max = bits - 1;

            tcg_out_insn_simpleImm(s, OPC_SRL_I, a0, a1, a2&max);

        } else {

            tcg_out_insn_bitReg(s, OPC_SRL, a0, a1, a2);

        }

        break;

  

    case INDEX_op_sar_i32:

        a2 = (int32_t)a2;

        tcg_out_sar(s, ext, a0, a1, a2, c2);

        break;

    case INDEX_op_sar_i64: /* sw arithmetic right*/

        tcg_out_sar(s, ext, a0, a1, a2, c2);

        break;

  

    case INDEX_op_rotr_i32: /* loop shift */

    case INDEX_op_rotr_i64:

        if (c2) {/* loop right shift a2*/

            tcg_out_rotr_Imm(s, ext, a0, a1, a2);

        } else {

            tcg_out_rotr_Reg(s, ext, a0, a1, a2);

        }

        break;

  

    case INDEX_op_rotl_i32: /* loop shift */

    case INDEX_op_rotl_i64: /* sw */

        if (c2) {/* loop left shift a2*/

            tcg_out_rotl_Imm(s, ext, a0, a1, a2);  

        } else {

            tcg_out_rotl_Reg(s, ext, a0, a1, a2);

        }

        break;

  

    case INDEX_op_clz_i32:

        tcg_out_ctz32(s, OPC_CTLZ, a0, a1, a2, c2);

        break;

    case INDEX_op_clz_i64: /* counting leading zero numbers */

        tcg_out_ctz64(s, OPC_CTLZ, a0, a1, a2, c2);

        break;

    case INDEX_op_ctz_i32:

        tcg_out_ctz32(s, OPC_CTTZ, a0, a1, a2, c2);

        break;

    case INDEX_op_ctz_i64: /* counting tailing zero numbers */

        tcg_out_ctz64(s, OPC_CTTZ, a0, a1, a2, c2);

        break;

  

    case INDEX_op_brcond_i32:

    case INDEX_op_brcond_i64:

        tcg_out_brcond(s, ext, a2, a0, a1, const_args[1], arg_label(args[3]));

        break;

  

    case INDEX_op_setcond_i32:

        a2 = (int32_t)a2;

    case INDEX_op_setcond_i64:      

        tcg_out_setcond(s, ext, args[3], a0, a1, a2, c2);

        break;

  

    case INDEX_op_movcond_i32:

        a2 = (int32_t)a2;

    case INDEX_op_movcond_i64:

        tcg_out_movcond(s, ext, args[5], a0, a1, a2, c2, REG0(3), REG0(4));

        break;

  

    case INDEX_op_qemu_ld_i32:

    case INDEX_op_qemu_ld_i64:

        tcg_out_qemu_ld(s, a0, a1, a2, ext);

        break;

    case INDEX_op_qemu_st_i32:

    case INDEX_op_qemu_st_i64:

        tcg_out_qemu_st(s, REG0(0), a1, a2);

        break;

  

    case INDEX_op_bswap64_i64:

        tcg_out_bswap64(s, TCG_TYPE_I64, a0, a1);

        break;

    case INDEX_op_bswap32_i64:

        tcg_out_bswap32(s, TCG_TYPE_I32, a0, a1);

    if (a2 & TCG_BSWAP_OS) {

                tcg_out_sxt(s, TCG_TYPE_I64, MO_32, a0, a0);

        }

        break;

    case INDEX_op_bswap32_i32:

         tcg_out_bswap32(s, TCG_TYPE_I32, a0, a1);

         break;

    case INDEX_op_bswap16_i32:

    case INDEX_op_bswap16_i64:

        tcg_out_bswap16(s, TCG_TYPE_I32, a0, a1);

        if (a2 & TCG_BSWAP_OS) {

           /* Output must be sign-extended. */

           tcg_out_sxt(s, ext, MO_16, a0, a0);

        } else if ((a2 & (TCG_BSWAP_IZ | TCG_BSWAP_OZ)) == TCG_BSWAP_OZ) {

            /* Output must be zero-extended, but input isn't. */

           tcg_out_insn_simpleImm(s, OPC_EXTLH_I, a0, a0, 0x0);

        }

        break;

  

    case INDEX_op_ext8s_i32:

    tcg_out_insn_simpleReg(s, OPC_SEXTB, a0, TCG_REG_ZERO, a1);

        tcg_out_insn_simpleImm(s, OPC_ZAPNOT_I, a0, a0, 0xf);

        break;

    case INDEX_op_ext8s_i64:

    tcg_out_insn_simpleReg(s, OPC_SEXTB, a0, TCG_REG_ZERO, a1);

        break;

    case INDEX_op_ext16s_i32:

        tcg_out_insn_simpleReg(s, OPC_SEXTH, a0, TCG_REG_ZERO, a1);

        tcg_out_insn_simpleImm(s, OPC_ZAPNOT_I, a0, a0, 0xf);

        break;

    case INDEX_op_ext16s_i64:

        tcg_out_insn_simpleReg(s, OPC_SEXTH, a0, TCG_REG_ZERO, a1);

        break;

    case INDEX_op_ext_i32_i64:

    case INDEX_op_ext32s_i64:

    tcg_out_insn_simpleReg(s, OPC_ADDW, a0, TCG_REG_ZERO, a1);

        break;

    case INDEX_op_ext8u_i32:

    case INDEX_op_ext8u_i64:

    tcg_out_insn_simpleImm(s, OPC_EXTLB_I, a0, a1, 0x0);

        break;

    case INDEX_op_ext16u_i32:

    case INDEX_op_ext16u_i64:

    tcg_out_insn_simpleImm(s, OPC_EXTLH_I, a0, a1, 0x0);

        break;

    case INDEX_op_extu_i32_i64:

    case INDEX_op_ext32u_i64:

        tcg_out_insn_simpleImm(s, OPC_ZAPNOT_I, a0, a1, 0xf);

        break;

  

    case INDEX_op_deposit_i32:

    case INDEX_op_deposit_i64:

        tcg_out_dep(s, ext, a0, REG0(2), args[3], args[4]);

        break;

  

    case INDEX_op_extract_i32:

    case INDEX_op_extract_i64:

        tcg_out_extract(s, a0, a1, a2, args[3]);//refer mips

        break;

  

    case INDEX_op_sextract_i32:

    case INDEX_op_sextract_i64:

        tcg_out_sextract(s, a0, a1, a2, args[3]);

        break;

  

    case INDEX_op_extract2_i32:

    case INDEX_op_extract2_i64:

        tcg_out_extract2(s, ext, a0, REG0(2), REG0(1), args[3]);

        break;

  

    case INDEX_op_add2_i32:

    case INDEX_op_add2_i64:

        tcg_debug_assert(0);

        break;

    case INDEX_op_sub2_i32:

    case INDEX_op_sub2_i64:

        tcg_debug_assert(0);

        break;

  

    case INDEX_op_muluh_i64:

        tcg_out_insn_simpleReg(s, OPC_UMULH, a0, a1, a2);

        break;

    case INDEX_op_mulsh_i64:

        tcg_out_mulsh64(s, a0, a1, a2);

        break;

  

    case INDEX_op_mb:

        tcg_out_mb(s);

        break;

  

    case INDEX_op_mov_i32:  /* Always emitted via tcg_out_mov.  */

    case INDEX_op_mov_i64:

    case INDEX_op_call:     /* Always emitted via tcg_out_call.  */

    default:

        g_assert_not_reached();

    }

  

#undef REG0

}
```