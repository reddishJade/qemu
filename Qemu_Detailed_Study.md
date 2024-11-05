# Intro
正如第 6 章所述，QEMU 是一种机器模拟器，因此可以在运行它的机器上模拟一定数量的处理器架构。对于 QEMU 来说，被模拟的架构被称为Target。而运行 QEMU 并模拟Target的真实机器称为Host。将虚拟机（Target）代码动态转换为Host代码的工作由 QEMU 中的一个模块完成，该模块被称为 “微小代码生成器”（Tiny Code Generator），简称 TCG。说到 TCG，“Target ”一词就有了不同的含义。TCG 创建代码来模拟Target代码，因此由 TCG target时其实指的是生成的Host代码。

因此，我们可以将模拟处理器运行的代码（OS +  USER TOOLS）称为Guest Code。QEMU 的功能是提取Guest Code，并将其转换为Host specific code。因此，整个转换任务由两部分组成：首先，目标代码块->翻译块（TB）被转换成 TCG ops(**一种独立于机器的中间符号**)->一种与机器无关的中间符号IR，然后，TB 的 TCG ops 通过 TCG 被转换成主机架构的Host code。在两者之间进行着可选的优化处理。

# 7.1 代码库Codebase

要添加新功能以扩展机器模拟器，并将生成的代码迁移到远程节点上执行，就必须清楚地了解 QEMU 代码库。QEMU 代码库有 1300 多个文件，这些文件被很好地组织到特定的部分。尽管代码组织得井井有条，但其复杂程度足以让任何新开发人员感到困惑。本节将介绍 QEMU 代码库的组织结构。

在本节中，代码库中最浅的目录深度将用“/”表示，而连续的目录深度将遵循通常的 Unix 文件路径符号。

## Start of Execution: 

/ 中对本研究重要的主要 C 文件是：/vl.c、/cpus.c、/exec-all.c、/exec.c、/cpu-exec.c。
执行开始的“main”函数在 /vl.c 中定义。此文件中的函数根据给定的虚拟机规范（如 RAM 大小、可用设备、CPU 数量等）设置虚拟机环境。从 main 函数开始，在设置虚拟机后，执行通过 /cpus.c、/exec-all.c、/exec.c、/cpu-exec.c 等文件分支出来。

## Emulated Hardware: 

虚拟机中模拟所有虚拟硬件的代码位于 /hw/。QEMU 模拟了大量硬件，但本研究不需要详细了解这些硬件是如何模拟的。

## Guest (Target) Specific：
QEMU 中当前模拟的处理器架构有：Alpha、ARM、Cris、i386、M68K、PPC、Sparc、Mips、MicroBlaze、S390X 和 SH4。将 TB 转换为 TCG ops 所需的这些架构特定代码可在 /target-xyz/ 中找到，其中 xyz 可以是上述任何架构名称。因此，可在 /target-i386/ 中找到 i386 特定代码。此部分可称为 TCG 的前端。

## Host (TCG) Specific:：
用于从 TCG ops 生成 host code 的 host specific code 位于 /tcg/ 中。在 TCG 内部，可以找到 /xyz/，其中 xyz 可以是 i386、sparc 等，其中包含将 TCG 操作转换为 architecture specific code 。此部分可以称为 TCG 的后端。

## 总结

| 函数                        | 作用                                                                                 |
| ------------------------- | ---------------------------------------------------------------------------------- |
| /vl.c：                    | 主模拟器循环、虚拟机设置与 CPU 执行。                                                              |
| /target-xyz/translate.c : | 提取的 guest code（guest specific ISA）被转换为架构独立的 TCG ops                                |
| /tcg/tcg.c :              | TCG 的主代码。                                                                          |
| /tcg/*/tcg-target.c：      | 将 TCG ops 转换为 host code（host specific ISA）的代码。                                     |
| /cpu-exec.c :             | /cpu-exec.c 中的函数 cpu-exec()，用于查找下一个翻译块（TB）。<br>如果未找到，则调用该函数生成下一个翻译块（TB），最后执行生成的代码。 |

# 7.2 TCG - 动态翻译

如本文前文所述，在 0.9.1 版之前，QEMU 中的动态翻译由 DynGen 执行。 TB 由 DynGen 转换为 C 代码，然后由 GCC（GNU C 编译器）将 C 代码转换为主机特定代码（host specific code）。该程序的问题在于 DynGen 与 GCC 紧密相连，当 GCC 演变时就会产生问题。为了消除翻译器与 GCC 的紧密联系，我们采用了一种新的程序：TCG。

动态翻译在需要时转换代码。这样做的目的是用最短的时间执行生成的代码，而不是花更多的时间在执行代码的生成上。每次从 TB 生成代码时，都会先将其存储在代码缓存（code cache）中，然后再执行。由于所谓的 “位置参考”（Locality Reference），大多数情况下都需要重复使用相同的 TB，因此最好将其保存起来，而不是重新生成相同的代码。一旦代码缓存满了，为了简化操作，就会刷新整个代码缓存，而不是使用 LRU （最近最少使用的页面置换）算法。

编译器会在执行前从源代码中生成目标代码。为了生成函数调用的目标代码，GCC 等编译器会生成特殊的汇编代码，在函数调用之前和函数返回之前完成必要的工作。这些特殊的汇编代码被称为**Function Prologue and Epilogue**。

## Function Prologue
如果体系结构有Base Pointer和Stack Pointer，**Function Prologue**通常会执行以下操作：

- 将当前的基指针推入堆栈，以便稍后恢复。
- 用当前栈指针替换旧的基指针，以便在旧栈顶部创建新栈。
- 将栈指针沿堆栈进一步移动，以便在当前栈框架中为函数的局部变量腾出空间。
## Function Epilogue
**Function Epilogue** 将**Function Prologue**的操作颠倒过来，将控制权返回给调用函数。它通常执行以下操作

- 用当前的基指针替换栈指针，使栈指针恢复到Prologue之前的值。
- 将基数指针从栈中弹出，因此栈指针会恢复到Prologue之前的值。
序言
- 将上一帧的程序计数器从栈中弹出并跳转到它，从而返回调用函数。

TCG 本身就可以看作是一个编译器，它可以即时生成目标代码。TCG 生成的代码存储在缓冲区buffer（代码缓存code cache）中。如图 7.3 所示，执行控制通过 TCG 的 “序言”（Prologue）和 “尾声”（Epilogue）在代码缓存之间传递。

# 7.3 TB 的链接：

从代码缓存返回到静态代码（QEMU 代码）并跳回代码缓存的速度通常较慢。为了解决这个问题，QEMU 将每个 TB 与下一个 TB 进行了链式处理。因此，在执行完一个 TB 后，执行将直接跳转到下一个 TB，而无需返回静态代码。当 TB 返回静态代码时，执行块链就会发生。因此，当 TB1 返回静态代码时（因为没有链锁），下一个 TB 即 TB2 就会被找到、生成并执行。当 TB2 返回时，它会立即链入 TB1。这就确保了下次执行 TB1 时，TB2 会紧随其后，而不会返回静态代码。

# 7.4 执行跟踪

本节将尝试跟踪 QEMU 的执行情况，并特别指出特定文件的位置和调用函数的声明。本节将主要关注 QEMU 的 TCG 部分，因此是找到生成主机代码的代码段的关键。充分了解 QEMU 中的代码生成是帮助修补 QEMU 以制作 EVM （**超轻量物联网虚拟机**）的必要条件。

文件/文件夹路径符号与上一节 “代码库 ”中使用的符号相同，但为了指定函数声明和定义语句的位置，需要增加相同的符号。

因此，func1(...){/folder/file.c} 表示 func1() 的声明在 /folder/file.c 中，#define symbol_name{/folder/file.c}、var var_name {/folder/file.c} 也是如此。

同样，要突出显示某段代码，也要使用以下约定。
```c
:345
int max=MAX;
:
```
表示 “int max=MAX; ”位于相应文件的第 346 行。

## main(...){/vl.c}:
> 主函数解析启动时传递的命令行参数，并根据内存大小、硬盘大小、启动盘等参数设置虚拟机（VM）。虚拟机设置完成后，`main()` 会调用 `main_loop()`。

## main_loop(...){/vl.c}:
> 函数 `main_loop` 首先调用 `qemu_main_loop_start()`，然后在以 `vm_can_run()` 为条件的 `do-while` 内无限循环 `cpu_exec_all()` 和 `profile_getclock()`。无限 for 循环继续检查一些虚拟机停止情况，如 `qemu_shutdown_requested()`、`qemu_powerdown_requested()`、`qemu_vmstop_requested()` 等。这些停止条件将不再进一步研究。

## qemu_main_loop_start(...){/cpus.c}:
> 函数 `qemu_main_loop_start` 设置变量 `qemu_system_ready = 1` 并调用 `qemu_cond_broadcast()`，该函数主要处理重启所有等待条件变量的线程。在此不做进一步研究。请查阅 /qemu-thread.c 了解更多详情。

## profile_getclock(...){/qemu-timer.c}:
> 函数 `profile_getclock` 主要处理定时（CLOCK_MONOTONIC），在此不再深入研究。

## cpu_exec_all(...){/cpus.c}:
> 函数 `cpu_exec_all` 基本上是循环使用虚拟机中可用的 CPU（内核）。QEMU 最多可以有 256 个内核。但所有这些内核都将以循环方式执行，因此无法完全模拟所有内核并行运行的多核处理器。一旦选择了下一个 CPU，就会找到它的状态（`CPUState *env`），并将该状态传递给 `qemu_cpu_exec()`，以便在检查 `cpu_can_run()`条件后，从当前状态继续执行所选的 CPU。

## struct CPUState{/target-xyz/cpu.h}:
> 结构 CPUState 是特定于体系结构的，主要保存 CPU 的状态，如标准寄存器、段、FPU 状态、异常/中断处理、处理器特性以及一些模拟器特定的内部变量和标志。

## qemu_cpu_exec(...){/cpus.c}:
> 函数 `qemu_cpu_exec` 主要调用 `cpu_exec()`。

## cpu_exec(...){/cpu-exec.c}:
> 函数 `cpu_exec` 被称为 “主执行循环”。 在这里，第一次初始化翻译块 TB（TranslationBlock *tb），然后代码基本上继续处理异常。在两个嵌套的无限 for 循环深处，我们可以找到 `tb_find_fast()` 和  `tcg_qemu_tb_exec()` 。生成的 host code 随后通过  `tcg_qemu_tb_exec()` 执行。

## struct TranslationBlock {/exec-all.h}:
> 结构 TranslationBlock 包含以下内容：`PC、CS_BASE、与此 TB 对应的 Flags、tc_ptr（指向此 TB 翻译代码的指针）、tb_next_offset[2]、tb_jmp_offset[2]（均用于查找链入此 TB 的 TB，即此 TB 之后的 TB）、*jmp_next[2]、*jmp_first（指向跳转到此 TB 的 TB）`。

## tb_find_fast(...){/cpu-exec.c}:
> 函数 `tb_find_fast` 调用 `cpu_get_tb_cpu_state()` ，该函数从 CPUState（环境）中获取程序计数器（PC），并将 PC 值传递给哈希函数，以获取 `tb_jmp_cache[]`（哈希表）中的 TB 索引。利用该索引，可以从 `tb_jmp_cache` 中找到下一个 TB。

```c
:200
tb = env->tb_jmp_cache[tb_jmp_cache_hash_func(pc)]
:
```

> 因此可以发现，一旦找到一个 TB（对于特定 PC 值），它就会被存储到 `tb_jmp_cache`，这样以后就可以通过使用哈希函数（`tb_jmp_cache_hash_func(pc)`）找到的索引，从 `tb_jmp_cache` 中重新使用它。接下来的代码将检查找到的 TB 的有效性，如果找到的 TB 无效，则调用 `tb_find_slow()`。

## cpu_get_tb_cpu_state(...){/target-xyz/cpu.h}: 
> 函数 `cpu_get_tb_cpu_state` 基本上是从当前的 CPUState (env) 中查找 PC、BP、Flags。

## tb_jmp_cache_hash_func(...){/exec-all.h}:
> 这是一个哈希函数，用于以 PC 为键查找 TB 在 `tb_jmp_cache` 中的偏移量。

## tb_find_slow(...){/cpu-exec.c}:
> 当使用 `tb_find_fast()` 失败时使用。这次将尝试使用物理内存映射来查找 TB。

```c
:142
phys_pc=get_page_addr_code(env, pc)
:
```

> `phys_pc` 应该是客户操作系统 PC 的物理内存地址，用于通过哈希函数查找下一个 TB。

```c
:147 
h=tb_phys_hash_func(phys_pc)
ptb1 = &tb_phys_hash[h];
:
```

> 上面的 ptb1 应该是下一个 TB，但在接下来的代码中会检查其有效性。如果没有找到有效的 TB，则通过 `tb_gen_code()` 生成新的 TB；如果找到了有效的 TB，则迅速将其添加到 `tb_jmp_cache` 中，其索引由 `tb_jmp_cache_hash_func()` 确定。

```c
:181
env->tb_jmp_cache[tb_jmp_cache_hash_func(pc)] = tb;
:
```

## tb_gen_code(...){/exec.c}: 
> 函数 `tb_gen_code` 首先通过 `tb_alloc()` 分配一个新的 TB，然后使用 `get_page_addr_code()` 从 CPUState 的 PC 中找到 TB 的 PC。

```c
:957
phys_pc = get_page_addr_code(env, pc);
tb = tb_alloc(pc);
:
```

> 完成后，将调用 `cpu_gen_code()`，然后调用 `tb_link_page()`，添加新的 TB 并将其链接到物理页表。

## cpu_gen_code(...){translate-all.c}:
> 函数 `cpu_gen_code` 启动实际代码生成。其中有一连串的后续函数调用，如下所示。
>
> >gen_intermediate_code(){/target-xyz/translate.c}
> >↓
> >gen_intermediate_code_internal(){/target-xyz/translate.c 
> >↓
> >disas_insn(){/target-xyz/translate.c}
>
> 函数 `disas_ins` 通过 target(Guest) 指令的长 `switch-case` 和相应的函数组，将 guest code 实际转换为 TCG ops ，最终将 TCG 操作添加到 code_buff。TCG 操作生成后，将调用 `tcg_gen_code()` 。

## tcg_gen_code(...){/tcg/tcg.c}:
> 函数 `tcg_gen_code` 将 TCG ops 转换为 host specific code。有关 TCG 功能的更多信息，请参见前一节 “TCG- Dynamic Translator”。


## #define tcg_qemu_tb_exec(...){/tcg/tcg.g}:
> 一旦通过上述所有流程获得下一个 TB，就需要执行该 TB。TB 通过 /exec-cpu.c 中的 `tcg_qemu_tb_exec()` 执行。

```c
:644
next_tb = tcg_qemu_tb_exec(tc_ptr)
:
```

> 事实上，`tcg_qemu_tb_exec()` 是在 /tcg/tcg.h 中定义的宏函数。

```c
:484 （in /tcg/tcg.h ）
extern uint8_t code_gen_prologue[];
:
#define tcg_qemu_tb_exec(tb_ptr) ((long REGPARM(*)(void *))
code_gen_prologue)(tb_ptr)
:
```

要理解上面这行代码中发生的事情，需要很好地掌握函数指针的知识。下面几行将详细说明如何理解这一点。

> 众所周知，`(int) var` 将显式地把一个变量转换为 `int` 类型。同样，`((long REGPARM (*)(void *))`是一个指向函数的类型指针，它接收 `void *` 参数并返回 `long` 。 在这里，`REGPARAM(*)` 是 GCC 编译器的一个指令，它使函数的参数通过寄存器而不是堆栈传递。
>
> 如果函数名以 `((long REGPARM (*func_name)(void *))` 的形式出现，那么 `((long REGPARM (*)(void *))` 的意图就很明显了。然而，这里使用的是没有函数名的数组（但起到了作用）。当使用数组名称时，将获得数组基地址，从而指向（指针）数组。 因此，`(function_pointer) array_name` 将把数组指针作为函数指针。
>
> 函数是通过指针 `(*pointer_to_func)(args)` 调用的，因此 `((long REGPARM (*)(void *))code_gen_prologue)(tc_ptr)` 会进行函数调用。可以看出，上述函数调用中缺少了一个 `“*”`，但可以通过测试发现，`(*pointer_to_func)(args)` 和 `(pointer_too_func)(args)` 是等价的。
>
> 因此，上述解释说明，`code_gen_prologue` 是一个数组，它被转换为函数并执行。`code_gen_prologue` 中包含一个二进制形式的函数，它接收一个参数 `tc_ptr`，并返回一个 `long`，即下一个 TB。`code_gen_prologue` 中的函数是 `Function Prologue` ，它将控制权转移到 `tc_ptr` 指向的生成主机代码。