
QEMU 的动态翻译任务主要包含以下两部分:

1. 将 Guest 代码转换为 TCG 操作（TCG operations）:
   - 首先,QEMU 会从要执行的 Guest 代码中提取一个代码块,称为 Translation Block（TB）。
   - 然后,QEMU 将这个 TB 转换成一种机器无关的中间表示 - TCG 操作。这个过程是由 `gen_intermediate_code()` 函数完成的。
   - 在转换 TB 为 TCG 操作时,QEMU 会利用一个大的 switch-case 语句,根据不同的 Guest 指令类型调用相应的转换函数。这些转换函数最终会将 Guest 指令翻译成对应的 TCG 操作。

2. 将 TCG 操作转换为 Host 代码:
   - 在第一步转换完成后,QEMU 会进一步把 TCG 操作转换为针对 Host 机器的具体指令代码。
   - 这个过程是由 `tcg_gen_code()` 函数完成的。它会遍历 TCG 操作并生成对应的 Host 代码。
   - 在生成 Host 代码的过程中,QEMU 还会执行一些可选的优化步骤。

总之,QEMU 的动态翻译任务分为两个主要步骤:

1. 将 Guest 代码转换为中间表示 TCG 操作
2. 将 TCG 操作转换为最终的 Host 代码

这里的两个步骤也就是现在的前端翻译阶段和后端翻译的阶段[[Front&Back]]

这种分步的转换方式使得 QEMU 能够实现跨平台的模拟功能,将 Guest 代码转换为宿主机能够直接执行的本地代码。