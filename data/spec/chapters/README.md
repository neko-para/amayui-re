# 引擎规范 · 章节（分发物）

本目录**就是可分发正文**，不打包成单文件；按下面的顺序阅读：

1. [`00-overview.md`](00-overview.md) —— 总览与约定（范围 / 术语 / 与现有实现的关系）
2. [`10-format.md`](10-format.md) —— 脚本与执行（脚本头 / 指令编码 / 操作数编码 / 帧）
3. [`20-alf.md`](20-alf.md) —— ALF 归档（索引头 / 段 / LZSS / TOC / 载荷）
4. [`30-agf.md`](30-agf.md) —— AGF 图像（ACGF 容器 / meta / body / ACIF / 像素）
5. [`40-pools.md`](40-pools.md) —— **数据池**（global/local 池、访问原语、类型标签、值编码、字符串、容量）
6. [`50-engine-state.md`](50-engine-state.md) —— 引擎级状态（按索引表）

`20-pools.md` 是**一个完整主题**：池的族、下标、访问、值表示与容量都在这一章。

规则小节用 `### 标题` 标记；`<!-- rule: ID -->` 是内部关联用的 HTML 注释，阅读时不可见。
本版尚未覆盖的章节不在这里 —— 见 `../audit.json` 的 `planned[]`。

★ 脚本的字节布局、指令表与 `type` / `raw` 编码由 **AGE 汇编格式**（`age-format` 包）规定；
本目录只写运行期语义，不复制 opcode 表。
