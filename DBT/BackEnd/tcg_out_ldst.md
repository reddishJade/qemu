```c
static void tcg_out_ldst(TCGContext *s, SW_64Insn insn, TCGReg rd, TCGReg rn, intptr_t offset, bool sign)
{
    if (offset != sextract64(offset, 0, 15)) {
        tcg_out_movi(s, TCG_TYPE_I64, TCG_REG_TMP2, offset);
        tcg_out_insn_simpleReg(s, OPC_ADDL, TCG_REG_TMP2, TCG_REG_TMP2, rn);
        tcg_out_insn_ldst(s, insn, rd, TCG_REG_TMP2, 0);
    }else{
        tcg_out_insn_ldst(s, insn, rd, rn, offset);
    }

    switch (insn){
        case OPC_LDBU:
             if(sign)
                 tcg_out_insn_simpleReg(s, OPC_SEXTB, rd, TCG_REG_ZERO, rd);//for micro-op:INDEX_op_ld8s_i32/64,set rd[63,8]=1
             break;
        case OPC_LDHU:
             if(sign)
                 tcg_out_insn_simpleReg(s, OPC_SEXTH, rd, TCG_REG_ZERO, rd);//for micro-op:INDEX_op_ld16s_i32/64,set rd[63,16]=1
             break;
        case OPC_LDW:
             if(!sign)
                 tcg_out_insn_simpleImm(s, OPC_ZAPNOT_I, rd, rd, 0xf);//for micro-op:INDEX_op_ld32u_i32/64,set rd[63,32]=0
             break;
        default:
             break; 
	}
}
```