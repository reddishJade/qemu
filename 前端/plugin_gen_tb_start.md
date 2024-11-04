```c
bool plugin_gen_tb_start(CPUState *cpu, const DisasContextBase *db,

                         bool mem_only)

{

    bool ret = false;

  

    if (test_bit(QEMU_PLUGIN_EV_VCPU_TB_TRANS, cpu->plugin_mask)) {

        struct qemu_plugin_tb *ptb = tcg_ctx->plugin_tb;

        int i;

  

        /* reset callbacks */

        for (i = 0; i < PLUGIN_N_CB_SUBTYPES; i++) {

            if (ptb->cbs[i]) {

                g_array_set_size(ptb->cbs[i], 0);

            }

        }

        ptb->n = 0;

  

        ret = true;

  

        ptb->vaddr = db->pc_first;

        ptb->vaddr2 = -1;

        ptb->haddr1 = db->host_addr[0];

        ptb->haddr2 = NULL;

        ptb->mem_only = mem_only;

  

        plugin_gen_empty_callback(PLUGIN_GEN_FROM_TB);

    }

  

    tcg_ctx->plugin_insn = NULL;

  

    return ret;

}
```