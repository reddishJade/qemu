```c
static void *translator_access(CPUArchState *env, DisasContextBase *db,

                               target_ulong pc, size_t len)

{

    void *host;

    target_ulong base, end;

    TranslationBlock *tb;

  

    tb = db->tb;

  

    /* Use slow path if first page is MMIO. */

    if (unlikely(tb_page_addr0(tb) == -1)) {

        return NULL;

    }

  

    end = pc + len - 1;

    if (likely(is_same_page(db, end))) {

        host = db->host_addr[0];

        base = db->pc_first;

    } else {

        host = db->host_addr[1];

        base = TARGET_PAGE_ALIGN(db->pc_first);

        if (host == NULL) {

            tb_page_addr_t phys_page =

                get_page_addr_code_hostp(env, base, &db->host_addr[1]);

            /* We cannot handle MMIO as second page. */

            assert(phys_page != -1);

            tb_set_page_addr1(tb, phys_page);

#ifdef CONFIG_USER_ONLY

            page_protect(end);

#endif

            host = db->host_addr[1];

        }

  

        /* Use slow path when crossing pages. */

        if (is_same_page(db, pc)) {

            return NULL;

        }

    }

  

    tcg_debug_assert(pc >= base);

    return host + (pc - base);

}
```