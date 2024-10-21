>1006                        tb = tb_gen_code(cpu, pc, cs_base, flags, cflags);
accel/tcg/translate-all.c───────────
TranslationBlock *tb_gen_code(CPUState *cpu,
    target_ulong pc, target_ulong cs_base,
    uint32_t flags, int cflags)
781             CPUArchState *env = cpu->env_ptr;
>792             assert_memory_lock();

linux-user/mmap.c───────────────────
>46              return mmap_lock_count > 0 ? true : false;

accel/tcg/translate-all.c───────────
>793             qemu_thread_jit_write();

/home/dongwei/qemu-7.2.0/include/qemu/osdep.h
>692         static inline void qemu_thread_jit_write(void) {}

../accel/tcg/translate-all.c─────────────────
>795             phys_pc = get_page_addr_code_hostp(env, pc, &host_pc);

../accel/tcg/user-exec.c─────────────────────
>204             flags = probe_access_internal(env, addr, 1, MMU_INST_FETCH, false, 0);
>145             switch (access_type) 
152             case MMU_INST_FETCH:
>153                 acc_flag = PAGE_EXEC;
>154                 break;
>159             if (guest_addr_valid_untagged(addr)) 

/home/dongwei/qemu-7.2.0/include/exec/cpu_ldst.h
>101             return x <= GUEST_ADDR_MAX;

../accel/tcg/user-exec.c────────────────────────
>160                 int page_flags = page_get_flags(addr);

../accel/tcg/translate-all.c────────────────────
>1354            p = page_find(address >> TARGET_PAGE_BITS);

../accel/tcg/internal.h─────────────────────────
>63              return page_find_alloc(index, false);

../accel/tcg/translate-all.c────────────────────
424         PageDesc *page_find_alloc(tb_page_addr_t index, bool alloc)
>431             lp = l1_map + ((index >> v_l1_shift) & (v_l1_size - 1));
434             for (i = v_l2_levels; i > 0; i--)                                                                                                                          │
│   435                 void **p = qatomic_rcu_read(lp);                                                                                                                                │
│   436                                                                                                                                                                                 │
│   437                 if (p == NULL) {                                                                                                                                                │
0x4 438                     void *existing;
0x40439 000028:                  
│   440                     if (!alloc) {                                                                                                                                               │
│   441                         return NULL;                                                                                                                                            │
│   442                     }                                                                                                                                                           │
│   443                     p = g_new0(void *, V_L2_SIZE);                                                                                                                              │
│   444                     existing = qatomic_cmpxchg(lp, NULL, p);                                                                                                                    │
│   445                     if (unlikely(existing)) {                                                                                                                                   │
│   446                         g_free(p);                                                                                                                                              │
│   447                         p = existing;                                                                                                                                           │
│   448                     }                                                                                                                                                           │
│   449                 }                                                                                                                                                               │
│   450                                                                                                                                                                                 │
│  >451                 lp = p + ((index >> (i * V_L2_BITS)) & (V_L2_SIZE - 1));

454             pd = qatomic_rcu_read(lp);
455             if (pd == NULL) 
487             return pd + (index & (V_L2_SIZE - 1));

│   1355            if (!p) {                                                                                                                                                           │
│   1356                return 0;                                                                                                                                                       │
│   1357            }
>1358            return p->flags;
>161                 if (page_flags & acc_flag) {
>162                     return 0; /* success */
}

../accel/tcg/user-exec.c────────────────────────
>205             g_assert(flags == 0);
>207             if (hostp) 
>208                 *hostp = g2h_untagged(addr);

/home/dongwei/qemu-7.2.0/include/exec/cpu_ldst.h
>91              return (void *)((uintptr_t)(x) + guest_base);

../accel/tcg/user-exec.c────────────────────────
>210             return addr;

../accel/tcg/translate-all.c────────────────────
>797             if (phys_pc == -1) 
>802             max_insns = cflags & CF_COUNT_MASK;
>803             if (max_insns == 0) 
>804                 max_insns = TCG_MAX_INSNS;
>809             tb = tcg_tb_alloc(tcg_ctx);

../tcg/tcg.c────────────────────────────────────
>704             uintptr_t align = qemu_icache_linesize;
>709             tb = (void *)ROUND_UP((uintptr_t)s->code_gen_ptr, align);
>710             next = (void *)ROUND_UP((uintptr_t)(tb + 1), align);
>712             if (unlikely(next > s->code_gen_highwater)) 
>718             qatomic_set(&s->code_gen_ptr, next);
>719             s->data_gen_ptr = NULL;
>720             return tb;

../accel/tcg/translate-all.c────────────────────
>810             if (unlikely(!tb)) 
>819             gen_code_buf = tcg_ctx->code_gen_ptr;
>820             tb->tc.ptr = tcg_splitwx_to_rx(gen_code_buf);

/home/dongwei/qemu-7.2.0/include/tcg/tcg.h──────
>647             return rw ? rw + tcg_splitwx_diff : NULL;

../accel/tcg/translate-all.c────────────────────
>822             tb->pc = pc;
>824             tb->cs_base = cs_base;
>825             tb->flags = flags
>826             tb->cflags = cflags;
>827             tb->trace_vcpu_dstate = *cpu->trace_dstate;
>828             tb_set_page_addr0(tb, phys_pc);

/home/dongwei/qemu-7.2.0/include/exec/exec-all.h
>633             tb->page_addr[0] = addr;

../accel/tcg/translate-all.c────────────────────
>829             tb_set_page_addr1(tb, -1);

/home/dongwei/qemu-7.2.0/include/exec/exec-all.h
>639             tb->page_addr[1] = addr;

../accel/tcg/translate-all.c────────────────────
>830             tcg_ctx->tb_cflags = cflags;
>839             trace_translate_block(tb, pc, tb->tc.ptr);

trace/trace-accel_tcg.h─────────────────────────
>144                 _nocheck__trace_translate_block(tb, pc, tb_code);

>123             if (trace_event_get_state(TRACE_TRANSLATE_BLOCK) && qemu_loglevel_mask(LOG_TRACE)) {

../accel/tcg/translate-all.c────────────────────
>841             gen_code_size = setjmp_gen_code(env, tb, pc, host_pc, &max_insns, &ti);
>753             int ret = sigsetjmp(tcg_ctx->jmp_trans, 0);
>754             if (unlikely(ret != 0)) 
>758             tcg_func_start(tcg_ctx);

../tcg/tcg.c────────────────────────────────────
>806             tcg_pool_reset(s);
>538             for (p = s->pool_first_large; p; p = t) 
>542             s->pool_first_large = NULL;
>543             s->pool_cur = s->pool_end = NULL;
>544             s->pool_current = NULL;

../tcg/tcg.c────────────────────────────────────
>807             s->nb_temps = s->nb_globals;
/* No temps have been previously allocated for size or locality.  */
>810             memset(s->free_temps, 0, sizeof(s->free_temps));
812             /* No constant temps have been previously allocated. */
│   813             for (int i = 0; i < TCG_TYPE_COUNT; ++i) {                                                                                                                          │
│   814                 if (s->const_table[i]) {                                                                                                                                        │
│   815                     g_hash_table_remove_all(s->const_table[i]);                                                                                                                 │
│   816                 }                                                                                                                                                               │
│   817             }

>819             s->nb_ops = 0;
>820             s->nb_labels = 0;
>821             s->current_frame_offset = s->frame_start;
>827             QTAILQ_INIT(&s->ops);
>828             QTAILQ_INIT(&s->free_ops);
>829             QSIMPLEQ_INIT(&s->labels);

../accel/tcg/translate-all.c────────────────────
>760             tcg_ctx->cpu = env_cpu(env);

/home/dongwei/qemu-7.2.0/include/exec/cpu-all.h─
>461             return &env_archcpu(env)->parent_obj;
>450             return container_of(env, ArchCPU, env);

../accel/tcg/translate-all.c────────────────────
>761             gen_intermediate_code(env_cpu(env), tb, *max_insns, pc, host_pc);

/home/dongwei/qemu-7.2.0/include/exec/cpu-all.h─
>461             return &env_archcpu(env)->parent_obj;
>450             return container_of(env, ArchCPU, env);

../target/i386/tcg/translate.c──────────────────
7068        /* generate intermediate code for basic block 'tb'.  */                                                                                                                 │
│   7069        void gen_intermediate_code(CPUState *cpu, TranslationBlock *tb, int max_insns,                                                                                          │
│   7070                                   target_ulong pc, void *host_pc)                                                                                                              │
│   7071        {                                                                                                                                                                       │
│   7072            DisasContext dc;                                                                                                                                                    │
│   7073                                                                                                                                                                                │
│  >7074            translator_loop(cpu, tb, max_insns, pc, host_pc, &i386_tr_ops, &dc.base);                                                                                           │
│   7075        }

../accel/tcg/translator.c───────────────────────
>49              uint32_t cflags = tb_cflags(tb);

/home/dongwei/qemu-7.2.0/include/exec/exec-all.h
>617             return qatomic_read(&tb->cflags);

../accel/tcg/translator.c───────────────────────
│   52              /* Initialize DisasContext */                                                                                                                                       │
│   53              db->tb = tb;                                                                                                                                                        │
│   54              db->pc_first = pc;                                                                                                                                                  │
│   55              db->pc_next = pc;                                                                                                                                                   │
│   56              db->is_jmp = DISAS_NEXT;                                                                                                                                            │
│  >57              db->num_insns = 0;                                                                                                                                                  │
│   58              db->max_insns = max_insns;                                                                                                                                          │
│   59              db->singlestep_enabled = cflags & CF_SINGLE_STEP;                                                                                                                   │
│   60              db->host_addr[0] = host_pc;                                                                                                                                         │
│   61              db->host_addr[1] = NULL;
>64              page_protect(pc)

../accel/tcg/translate-all.c────────────────────
>1484            p = page_find(page_addr >> TARGET_PAGE_BITS);

../accel/tcg/internal.h─────────────────────────
>63              return page_find_alloc(index, false);

../accel/tcg/translate-all.c────────────────────
│   430             /* Level 1.  Always allocated.  */                                                                                                                                  │
│   431             lp = l1_map + ((index >> v_l1_shift) & (v_l1_size - 1));                                                                                                            │
│   432                                                                                                                                                                                 │
│   433             /* Level 2..N-1.  */                                                                                                                                                │
│  >434             for (i = v_l2_levels; i > 0; i--)                                                                                                                           │
│   435                 void **p = qatomic_rcu_read(lp);                                                                                                                                │
│   436                                                                                                                                                                                 │
│   437                 if (p == NULL)                                                                                                                                        │
│   438                     void *existing;

../accel/tcg/translate-all.c────────────────────
>451                 lp = p + ((index >> (i * V_L2_BITS)) & (V_L2_SIZE - 1));
>454             pd = qatomic_rcu_read(lp);
>455             if (pd == NULL) 
>487             return pd + (index & (V_L2_SIZE - 1));
>1485            if (p && (p->flags & PAGE_WRITE)) 

../accel/tcg/translator.c───────────────────────
>67              ops->init_disas_context(db, cpu);

../target/i386/tcg/translate.c──────────────────
>6902            DisasContext *dc = container_of(dcbase, DisasContext, base);
>6903            CPUX86State *env = cpu->env_ptr;
│  >6904            uint32_t flags = dc->base.tb->flags;                                                                                                                                │
│   6905            uint32_t cflags = tb_cflags(dc->base.tb);                                                                                                                           │
>617             return qatomic_read(&tb->cflags);
>6906            int cpl = (flags >> HF_CPL_SHIFT) & 3;
>6907            int iopl = (flags >> IOPL_SHIFT) & 3;
>6909            dc->cs_base = dc->base.tb->cs_base;
>6910            dc->pc_save = dc->base.pc_next;
>6911            dc->flags = flags;
/* We make some simplifying assumptions; validate they're correct. */
>6918            g_assert(PE(dc) == ((flags & HF_PE_MASK) != 0));
>6919            g_assert(CPL(dc) == cpl);
>6920            g_assert(IOPL(dc) == iopl);
│  >6921            g_assert(VM86(dc) == ((flags & HF_VM_MASK) != 0));                                                                                                                  │
│   6922            g_assert(CODE32(dc) == ((flags & HF_CS32_MASK) != 0));                                                                                                              │
│   6923            g_assert(CODE64(dc) == ((flags & HF_CS64_MASK) != 0));                                                                                                              │
│   6924            g_assert(SS32(dc) == ((flags & HF_SS32_MASK) != 0));                                                                                                                │
│   6925            g_assert(LMA(dc) == ((flags & HF_LMA_MASK) != 0));                                                                                                                  │
│   6926            g_assert(ADDSEG(dc) == ((flags & HF_ADDSEG_MASK) != 0));                                                                                                            │
│   6927            g_assert(SVME(dc) == ((flags & HF_SVME_MASK) != 0));                                                                                                                │
│   6928            g_assert(GUEST(dc) == ((flags & HF_GUEST_MASK) != 0));
│  >6930            dc->cc_op = CC_OP_DYNAMIC;                                                                                                                                          │
│   6931            dc->cc_op_dirty = false;                                                                                                                                            │
│   6932            dc->popl_esp_hack = 0;                                                                                                                                              │
│   6933            /* select memory access functions */                                                                                                                                │
│   6934            dc->mem_index = 0;
│  >6938            dc->cpuid_features = env->features[FEAT_1_EDX];                                                                                                                     │
│  >6939            dc->cpuid_ext_features = env->features[FEAT_1_ECX];                                                                                                                 │
│   6940            dc->cpuid_ext2_features = env->features[FEAT_8000_0001_EDX];                                                                                                        │
│   6941            dc->cpuid_ext3_features = env->features[FEAT_8000_0001_ECX];                                                                                                        │
│   6942            dc->cpuid_7_0_ebx_features = env->features[FEAT_7_0_EBX];                                                                                                           │
│   6943            dc->cpuid_7_0_ecx_features = env->features[FEAT_7_0_ECX];                                                                                                           │
│   6944            dc->cpuid_xsave_features = env->features[FEAT_XSAVE];                                                                                                               │
│   6945            dc->jmp_opt = !((cflags & CF_NO_GOTO_TB) ||                                                                                                                         │
│   6946                            (flags & (HF_TF_MASK | HF_INHIBIT_IRQ_MASK)));
│   6947            /*                                                                                                                                                                  │
│   6948             * If jmp_opt, we want to handle each string instruction individually.                                                                                              │
│   6949             * For icount also disable repz optimization so that each iteration                                                                                                 │
│   6950             * is accounted separately.                                                                                                                                         │
│   6951             */                                                                                                                                                                 │
│  >6952            dc->repz_opt = !dc->jmp_opt && !(cflags & CF_USE_ICOUNT);
│  >6954            dc->T0 = tcg_temp_new();

│  >6955            dc->T1 = tcg_temp_new();                                                                                                                                            │
│   6956            dc->A0 = tcg_temp_new();                                                                                                                                            │
│   6957                                                                                                                                                                                │
│   6958            dc->tmp0 = tcg_temp_new();                                                                                                                                          │
│   6959            dc->tmp1_i64 = tcg_temp_new_i64();                                                                                                                                  │
│   6960            dc->tmp2_i32 = tcg_temp_new_i32();                                                                                                                                  │
│   6961            dc->tmp3_i32 = tcg_temp_new_i32();                                                                                                                                  │
│   6962            dc->tmp4 = tcg_temp_new();                                                                                                                                          │
│   6963            dc->cc_srcT = tcg_temp_local_new();

/home/dongwei/qemu-7.2.0/include/tcg/tcg.h──────
>902             TCGTemp *t = tcg_temp_new_internal(TCG_TYPE_I64, false);

../tcg/tcg.c────────────────────────────────────
>947             TCGContext *s = tcg_ctx;
>948             TCGTempKind kind = temp_local ? TEMP_LOCAL : TEMP_NORMAL;
>952             k = type + (temp_local ? TCG_TYPE_COUNT : 0);
>953             idx = find_first_bit(s->free_temps[k].l, TCG_MAX_TEMPS);

/home/dongwei/qemu-7.2.0/include/qemu/bitops.h──
│   189             for (result = 0; result < size; result += BITS_PER_LONG) {                                                                                                          │
│   190                 tmp = *addr++;                                                                                                                                                  │
│   191                 if (tmp) {                                                                                                                                                      │
│   192                     result += ctzl(tmp);                                                                                                                                        │
│   193                     return result < size ? result : size;                                                                                                                       │
│   194                 }                                                                                                                                                               │
│   195             }                                                                                                                                                                   │
│   196             /* Not found */                                                                                                                                                     │
│  >197             return size;

../tcg/tcg.c────────────────────────────────────
>954             if (idx < TCG_MAX_TEMPS) {
│   962             } else {                                                                                                                                                           │
│  >963                 ts = tcg_temp_alloc(s)
}

../tcg/tcg.c────────────────────────────────────
>834             int n = s->nb_temps++;
>836             if (n >= TCG_MAX_TEMPS) 
>839             return memset(&s->temps[n], 0, sizeof(TCGTemp));
>978                     ts->base_type = type;
>979                     ts->type = type;
>980                     ts->temp_allocated = 1;
>981                     ts->kind = kind;
>988             return ts;

/home/dongwei/qemu-7.2.0/include/tcg/tcg.h──────
>903             return temp_tcgv_i64(t);
>727             return (TCGv_i64)temp_tcgv_i32(t);
>721             (void)temp_idx(t); /* trigger embedded assert */
>658             ptrdiff_t n = ts - tcg_ctx->temps;
>659             tcg_debug_assert(n >= 0 && n < tcg_ctx->nb_temps);
>660             return n;
>722             return (TCGv_i32)((void *)t - (void *)tcg_ctx);
