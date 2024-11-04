```c
static inline void gen_tb_start(const TranslationBlock *tb)
{
    TCGv_i32 count;

    // 判断是否使用指令计数，如果需要则创建本地计数器临时变量
    if (tb_cflags(tb) & CF_USE_ICOUNT) {
        count = tcg_temp_local_new_i32();
    } else {
        count = tcg_temp_new_i32();
    }

    // 加载负的计数值（icount_decr.u32），用于计数指令数
    tcg_gen_ld_i32(count, cpu_env,
                   offsetof(ArchCPU, neg.icount_decr.u32) -
                   offsetof(ArchCPU, env));

    // 如果启用了指令计数，则将指令数递减，并将操作标记保存
    if (tb_cflags(tb) & CF_USE_ICOUNT) {
        /* 在指令数参数未知时，生成一个空的减法操作。
           当指令数已知时，我们会更新这个参数 */
        tcg_gen_sub_i32(count, count, tcg_constant_i32(0));
        icount_start_insn = tcg_last_op();  // 保存该指令位置
    }

    /* 检查是否设置了 CF_NOIRQ 标志。如果没有该标志，则生成一个
       中断检查标签，若指令计数小于 0 则跳转至退出 */
    if (tb_cflags(tb) & CF_NOIRQ) {
        tcg_ctx->exitreq_label = NULL;
    } else {
        tcg_ctx->exitreq_label = gen_new_label();
        tcg_gen_brcondi_i32(TCG_COND_LT, count, 0, tcg_ctx->exitreq_label);
    }

    // 如果使用了指令计数，则存储递减计数值并初始化 I/O 标记
    if (tb_cflags(tb) & CF_USE_ICOUNT) {
        tcg_gen_st16_i32(count, cpu_env,
                         offsetof(ArchCPU, neg.icount_decr.u16.low) -
                         offsetof(ArchCPU, env));
        /* 在每个翻译块开始时，将 can_do_io 标记清零，避免重复生成 io_end */
        tcg_gen_st_i32(tcg_constant_i32(0), cpu_env,
                       offsetof(ArchCPU, parent_obj.can_do_io) -
                       offsetof(ArchCPU, env));
    }

    // 释放临时计数器变量
    tcg_temp_free_i32(count);
}

static inline void gen_tb_end(const TranslationBlock *tb, int num_insns)
{
    // 如果使用指令计数，更新减法操作的实际指令计数
    if (tb_cflags(tb) & CF_USE_ICOUNT) {
        /* 在指令计数已知的情况下更新空参数的指令计数 */
        tcg_set_insn_param(icount_start_insn, 2,
                           tcgv_i32_arg(tcg_constant_i32(num_insns)));
    }

    // 若存在中断检查标签，设置为出口标签并跳出翻译块
    if (tcg_ctx->exitreq_label) {
        gen_set_label(tcg_ctx->exitreq_label);  // 设置出口标签
        tcg_gen_exit_tb(tb, TB_EXIT_REQUESTED);  // 退出当前翻译块
    }
}
```

### 代码解析

1. **`gen_tb_start` 函数**：初始化翻译块指令计数、检查中断条件并准备跳出翻译块的条件。
   - 创建计数器 `count`，决定是否使用本地或一般临时变量。
   - 加载指令计数器，若使用指令计数则生成一个减法操作，并为该位置做标记以便在后续填入真实指令数。
   - 检查 `CF_NOIRQ` 标志，决定是否生成中断检查标签，并在需要时生成标签以便指令数低于零时退出翻译块。
   - 若使用指令计数，存储当前计数值并清除 `can_do_io` 标记，防止指令翻译后遗留旧的 I/O 状态。

2. **`gen_tb_end` 函数**：翻译块结束时更新实际指令数和设置中断跳出标签。
   - 若使用了指令计数，则更新减法操作中的指令计数参数，以实际指令数填充。
   - 若中断检查标签存在，将其设置为出口标签，并在中断触发时跳出当前翻译块。