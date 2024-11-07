```c
static void tcg_out_insn_ldst(TCGContext *s, SW_64Insn insn, TCGReg rd, TCGReg rn, intptr_t imm16)
{                                     
    tcg_debug_assert( imm16 <= 0x7fff && imm16 >= -0x8000 );
    tcg_out32(s, insn | (rd & 0x1f) << 21 | (rn & 0x1f) << 16 | (imm16 & 0xffff) );
}
```