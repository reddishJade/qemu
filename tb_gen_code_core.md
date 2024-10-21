```c
TranslationBlock *tb_gen_code(CPUState *cpu,
                              target_ulong pc, target_ulong cs_base,
                              uint32_t flags, int cflags) 
{
    CPUArchState *env = cpu->env_ptr;
    TranslationBlock *tb, *existing_tb;
    tb_page_addr_t phys_pc;
    tcg_insn_unit *gen_code_buf;
    int gen_code_size, search_size, max_insns;
    int64_t ti;
    void *host_pc;

    // 获取物理页面地址
    phys_pc = get_page_addr_code_hostp(env, pc, &host_pc);

    // 如果物理地址无效，设置 cflags，使 TB 只包含一个指令
    if (phys_pc == -1) {
        cflags = (cflags & ~CF_COUNT_MASK) | CF_LAST_IO | 1;
    }

    // 设置最大指令数
    max_insns = cflags & CF_COUNT_MASK;
    if (max_insns == 0) {
        max_insns = TCG_MAX_INSNS;
    }

buffer_overflow:
    // 分配 TranslationBlock
    tb = tcg_tb_alloc(tcg_ctx);
    if (!tb) {
        tb_flush(cpu);
        mmap_unlock();
        cpu->exception_index = EXCP_INTERRUPT;
        cpu_loop_exit(cpu);
    }

    // 初始化 TB 的字段
    gen_code_buf = tcg_ctx->code_gen_ptr;
    tb->tc.ptr = tcg_splitwx_to_rx(gen_code_buf);
    tb->pc = pc;
    tb->cs_base = cs_base;
    tb->flags = flags;
    tb->cflags = cflags;

tb_overflow:
    // 生成和优化翻译块
    gen_code_size = setjmp_gen_code(env, tb, pc, host_pc, &max_insns, &ti);
    if (gen_code_size < 0) {
        // 如果生成过程遇到问题，处理溢出或其他错误
        switch (gen_code_size) {
        case -1:
            // 缓冲区溢出，重新生成
            goto buffer_overflow;
        case -2:
            // 生成的代码块过大，减少指令数
            max_insns /= 2;
            goto tb_overflow;
        default:
            g_assert_not_reached();
        }
    }

    // 编码搜索、缓存和跳转列表初始化
    search_size = encode_search(tb, (void *)gen_code_buf + gen_code_size);
    if (search_size < 0) {
        goto buffer_overflow;
    }
    tb->tc.size = gen_code_size;

    // 更新生成的代码指针，确保对齐
    qatomic_set(&tcg_ctx->code_gen_ptr, (void *)ROUND_UP((uintptr_t)gen_code_buf + gen_code_size + search_size, CODE_GEN_ALIGN));

    // 初始化跳转列表
    qemu_spin_init(&tb->jmp_lock);
    tb->jmp_list_head = (uintptr_t)NULL;
    tb->jmp_list_next[0] = (uintptr_t)NULL;
    tb->jmp_list_next[1] = (uintptr_t)NULL;

    // 检查并重置跳转
    if (tb->jmp_reset_offset[0] != TB_JMP_RESET_OFFSET_INVALID) {
        tb_reset_jump(tb, 0);
    }
    if (tb->jmp_reset_offset[1] != TB_JMP_RESET_OFFSET_INVALID) {
        tb_reset_jump(tb, 1);
    }

    // 如果没有关联物理页面地址，返回临时 TB
    if (tb_page_addr0(tb) == -1) {
        return tb;
    }

    // 将 TB 插入缓存并检查是否已经存在
    tcg_tb_insert(tb);
    existing_tb = tb_link_page(tb, tb_page_addr0(tb), tb_page_addr1(tb));
    if (existing_tb != tb) {
        // 如果 TB 已经存在，返回现有的 TB
        uintptr_t orig_aligned = (uintptr_t)gen_code_buf;
        orig_aligned -= ROUND_UP(sizeof(*tb), qemu_icache_linesize);
        qatomic_set(&tcg_ctx->code_gen_ptr, (void *)orig_aligned);
        tcg_tb_remove(tb);
        return existing_tb;
    }

    return tb;
}
```

