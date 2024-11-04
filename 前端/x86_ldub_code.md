```c
static inline uint8_t x86_ldub_code(CPUX86State *env, DisasContext *s)

{

    return translator_ldub(env, &s->base, advance_pc(env, s, 1));

}
```