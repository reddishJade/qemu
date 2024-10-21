
在`main.c`文件中，程序通过以下方式加载目标平台的程序：
```c
ret = loader_exec(execfd, exec_path, target_argv, target_environ, regs);
```
随后，通过执行以下代码进入模拟CPU的循环过程：
```c
cpu_loop(env);
```
在模拟CPU循环中，位于`x86_64/../i386/cpu_loop.c`的函数`cpu_exec(cs)`被调用来执行CPU指令，并动态翻译以获取翻译块（Translation Block, TB）。其中，`cs`代表代码段寄存器。
`cpu_exec()`函数进一步调用`accel/tcg/cpu-exec.c`中的`tb_lookup(cpu, pc, cs_base, flags, cflags)`，在TB缓存中查找翻译块。如果翻译块已存在且未被淘汰，则直接返回该缓存中的翻译块。若未找到，则通过以下代码生成新的翻译块：
```c
tb = tb_gen_code(cpu, pc, cs_base, flags, cflags);
```
`tb_gen_code()`函数则调用`translate-all.c`中的`tcg_gen_code(tcg_ctx, tb, pc)`，该函数利用TCG进行动态翻译。参数说明：`tcg_ctx`为翻译上下文，`tb`为翻译块，`pc`为当前程序计数器。
在`cpu_exec.c`中，已翻译的机器码通过以下方式交由主机CPU执行：
```c
strlret = tcg_qemu_tb_exec(env, tb_ptr);
```
随后，循环继续执行，重复以下过程以查找和生成翻译块：
```c
tb = tb_lookup(cpu, pc, cs_base, flags, cflags);
tb = tb_gen_code(cpu, pc, cs_base, flags, cflags);
return tcg_gen_code(tcg_ctx, tb, pc);
```
动态翻译的核心函数为`tb_lookup()`和`tb_gen_code()`。目标平台的代码存储在模拟的内存中，通过程序计数器（PC）进行读取。而翻译后的代码（适用于主机平台）则存储在翻译块缓存中，供主机平台执行。
