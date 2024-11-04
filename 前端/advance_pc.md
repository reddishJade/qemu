```c
static uint64_t advance_pc(CPUX86State *env, DisasContext *s, int num_bytes)
{
    // 保存当前的程序计数器(pc)
    uint64_t pc = s->pc;

    /* 检查当前指令是否跨越页边界。
     * 如果这不是第一条指令且本条指令的最后一个字节不在同一页中，
     * 则调用siglongjmp跳转，传递2以标识页边界问题。 */
    if (s->base.num_insns > 1 &&
        !is_same_page(&s->base, s->pc + num_bytes - 1)) {
        siglongjmp(s->jmpbuf, 2);
    }

    // 更新pc，将其推进指定的字节数
    s->pc += num_bytes;

    /* 检查指令长度是否超过允许的最大指令长度 */
    if (unlikely(cur_insn_len(s) > X86_MAX_INSN_LENGTH)) {
        /* 如果指令的第16个字节与第一个字节不在同一页内，
         * 需要模拟页错误优先于指令过长的错误。
         * 即使操作数只有一个字节长，也可能发生这种情况！ */
        if (((s->pc - 1) ^ (pc - 1)) & TARGET_PAGE_MASK) {
            // 强制读取当前页的最后一个字节以触发页错误
            volatile uint8_t unused = cpu_ldub_code(env, (s->pc - 1) & TARGET_PAGE_MASK);
            (void) unused; // 避免未使用变量警告
        }

        // 如果指令太长，则调用siglongjmp跳转，传递1以标识长度问题
        siglongjmp(s->jmpbuf, 1);
    }

    // 返回指令执行前的pc
    return pc;
}

```