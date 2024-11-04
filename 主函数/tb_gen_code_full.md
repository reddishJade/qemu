```c
/* tb_gen_code 函数在用户模式仿真中调用，生成一个新的翻译块(Translation Block, TB) */
TranslationBlock *tb_gen_code(CPUState *cpu,
                              target_ulong pc, target_ulong cs_base,
                              uint32_t flags, int cflags)
{
    // 获取当前CPU的架构状态和指令翻译上下文
    CPUArchState *env = cpu->env_ptr;
    TranslationBlock *tb, *existing_tb;
    tb_page_addr_t phys_pc; // 存储物理页的地址
    tcg_insn_unit *gen_code_buf; // 指向生成的代码缓冲区
    int gen_code_size, search_size, max_insns; // 代码大小、查找大小、最大指令数
#ifdef CONFIG_PROFILER
    TCGProfile *prof = &tcg_ctx->prof; // 配置分析器
#endif
    int64_t ti;
    void *host_pc;

    // 检查是否持有内存锁，确保安全写入
    assert_memory_lock();
    qemu_thread_jit_write();

    // 获取指定PC的物理地址，如果失败则返回 -1
    phys_pc = get_page_addr_code_hostp(env, pc, &host_pc);

    // 若物理地址无效，则生成一个仅包含一条指令的 TB
    if (phys_pc == -1) {
        cflags = (cflags & ~CF_COUNT_MASK) | CF_LAST_IO | 1;
    }

    // 确定生成TB的指令数上限
    max_insns = cflags & CF_COUNT_MASK;
    if (max_insns == 0) {
        max_insns = TCG_MAX_INSNS;
    }
    QEMU_BUILD_BUG_ON(CF_COUNT_MASK + 1 != TCG_MAX_INSNS);

 buffer_overflow:
    tb = tcg_tb_alloc(tcg_ctx); // 为新的 TB 分配内存
    if (unlikely(!tb)) {
        // 若 TB 分配失败，则刷新 TB 缓存并退出
        tb_flush(cpu);
        mmap_unlock();
        cpu->exception_index = EXCP_INTERRUPT;
        cpu_loop_exit(cpu);
    }

    gen_code_buf = tcg_ctx->code_gen_ptr; // 获取生成代码的缓冲区指针
    tb->tc.ptr = tcg_splitwx_to_rx(gen_code_buf); // 将缓冲区转换为只读区域
#if !TARGET_TB_PCREL
    tb->pc = pc; // 设置指令的起始PC
#endif
    tb->cs_base = cs_base;
    tb->flags = flags;
    tb->cflags = cflags;
    tb->trace_vcpu_dstate = *cpu->trace_dstate;
    tb_set_page_addr0(tb, phys_pc); // 设定页地址
    tb_set_page_addr1(tb, -1);
    tcg_ctx->tb_cflags = cflags;
 tb_overflow:

#ifdef CONFIG_PROFILER
    // 如果启用分析器，则记录 TB 数量及翻译开始时间
    qatomic_set(&prof->tb_count1, prof->tb_count1 + 1);
    ti = profile_getclock();
#endif

    // 记录翻译块，用于调试跟踪
    trace_translate_block(tb, pc, tb->tc.ptr);

    // 开始生成代码，返回生成的代码大小
    gen_code_size = setjmp_gen_code(env, tb, pc, host_pc, &max_insns, &ti);
    if (unlikely(gen_code_size < 0)) {
        // 处理生成代码时的异常
        switch (gen_code_size) {
        case -1:
            // 代码缓冲区溢出，重新生成代码
            qemu_log_mask(CPU_LOG_TB_OP | CPU_LOG_TB_OP_OPT,
                          "Restarting code generation for "
                          "code_gen_buffer overflow\n");
            goto buffer_overflow;

        case -2:
            // TB的代码大小超出允许的范围，减少指令数并重试
            assert(max_insns > 1);
            max_insns /= 2;
            qemu_log_mask(CPU_LOG_TB_OP | CPU_LOG_TB_OP_OPT,
                          "Restarting code generation with "
                          "smaller translation block (max %d insns)\n",
                          max_insns);
            goto tb_overflow;

        default:
            g_assert_not_reached();
        }
    }

    // 编码查找功能，处理缓冲区溢出
    search_size = encode_search(tb, (void *)gen_code_buf + gen_code_size);
    if (unlikely(search_size < 0)) {
        goto buffer_overflow;
    }
    tb->tc.size = gen_code_size; // 设置 TB 的大小

#ifdef CONFIG_PROFILER
    // 更新分析器记录的代码生成和查找时间
    qatomic_set(&prof->code_time, prof->code_time + profile_getclock() - ti);
    qatomic_set(&prof->code_in_len, prof->code_in_len + tb->size);
    qatomic_set(&prof->code_out_len, prof->code_out_len + gen_code_size);
    qatomic_set(&prof->search_out_len, prof->search_out_len + search_size);
#endif

#ifdef DEBUG_DISAS
    // 若启用指令反汇编输出，则打印反汇编指令
    if (qemu_loglevel_mask(CPU_LOG_TB_OUT_ASM) &&
        qemu_log_in_addr_range(pc)) {
        FILE *logfile = qemu_log_trylock();
        if (logfile) {
            int code_size, data_size;
            const tcg_target_ulong *rx_data_gen_ptr;
            size_t chunk_start;
            int insn = 0;

            // 根据生成的代码和数据段的大小，打印相关的指令和数据信息
            if (tcg_ctx->data_gen_ptr) {
                rx_data_gen_ptr = tcg_splitwx_to_rx(tcg_ctx->data_gen_ptr);
                code_size = (const void *)rx_data_gen_ptr - tb->tc.ptr;
                data_size = gen_code_size - code_size;
            } else {
                rx_data_gen_ptr = 0;
                code_size = gen_code_size;
                data_size = 0;
            }

            fprintf(logfile, "OUT: [size=%d]\n", gen_code_size);
            fprintf(logfile,
                    "  -- guest addr 0x" TARGET_FMT_lx " + tb prologue\n",
                    tcg_ctx->gen_insn_data[insn][0]);
            chunk_start = tcg_ctx->gen_insn_end_off[insn];
            disas(logfile, tb->tc.ptr, chunk_start);

            while (insn < tb->icount) {
                size_t chunk_end = tcg_ctx->gen_insn_end_off[insn];
                if (chunk_end > chunk_start) {
                    fprintf(logfile, "  -- guest addr 0x" TARGET_FMT_lx "\n",
                            tcg_ctx->gen_insn_data[insn][0]);
                    disas(logfile, tb->tc.ptr + chunk_start,
                          chunk_end - chunk_start);
                    chunk_start = chunk_end;
                }
                insn++;
            }

            if (chunk_start < code_size) {
                fprintf(logfile, "  -- tb slow paths + alignment\n");
                disas(logfile, tb->tc.ptr + chunk_start,
                      code_size - chunk_start);
            }

            if (data_size) {
                int i;
                fprintf(logfile, "  data: [size=%d]\n", data_size);
                for (i = 0; i < data_size / sizeof(tcg_target_ulong); i++) {
                    if (sizeof(tcg_target_ulong) == 8) {
                        fprintf(logfile,
                                "0x%08" PRIxPTR ":  .quad  0x%016" TCG_PRIlx "\n",
                                (uintptr_t)&rx_data_gen_ptr[i], rx_data_gen_ptr[i]);
                    } else if (sizeof(tcg_target_ulong) == 4) {
                        fprintf(logfile,
                                "0x%08" PRIxPTR ":  .long  0x%08" TCG_PRIlx "\n",
                                (uintptr_t)&rx_data_gen_ptr[i], rx_data_gen_ptr[i]);
                    } else {
                        qemu_build_not_reached();
                    }
                }
            }
            fprintf(logfile, "\n");
            qemu_log_unlock(logfile);
        }
    }
#endif

    qatomic_set(&tcg_ctx->code_gen_ptr, (void *)
        ROUND_UP((uintptr_t)gen_code_buf + gen_code_size + search_size,
                 CODE_GEN_ALIGN));

    // 初始化跳转列表，设置默认的跳转目标地址
    qemu_spin_init(&tb->jmp_lock);
    tb->jmp_list_head = (uintptr_t)NULL;
    tb->jmp_list_next[0] = (uintptr_t)NULL;
    tb->jmp_list_next[1] = (uintptr_t)NULL;
    tb->jmp_dest[0] = (uintptr_t)NULL;
    tb->jmp_dest[1] = (uintptr_t)NULL;

    // 初始化在生成代码过程中设置的跳转地址
    if (tb->jmp_reset_offset[0] != TB_JMP_RESET_OFFSET_INVALID) {
        tb_reset_jump(tb, 0);
    }
    if (tb->jmp_reset_offset[1] != TB_JMP_RESET_OFFSET_INVALID) {
        tb_reset_jump(tb, 1);
    }

    /* 如果TB未关联物理内存页，则返回 */
    if (tb_page_addr0(tb) == -1) {
        return tb;
    }

    /* 插入 TB 到对应的区域树中 */
    tcg_tb_insert(tb);

    // 检查翻译块是否已存在，若已存在则返回
    existing_tb = tb_link_page(tb, tb_page_addr0(tb), tb_page_addr1(tb));
    if (unlikely(existing_tb != tb)) {
        uintptr_t orig_aligned = (uintptr_t)gen_code_buf;
        orig_aligned -= ROUND_UP(sizeof(*tb), qemu_icache_linesize);
        qatomic_set(&tcg_ctx->code_gen_ptr, (void *)orig_aligned);
        tcg_tb_remove(tb);
        return existing_tb;
    }
    return tb;
}

```