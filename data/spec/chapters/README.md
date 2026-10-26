# 引擎规范 · 章节（分发物）

本目录**就是可分发正文**，不打包成单文件；按下面的顺序阅读：

1. [`00-overview.md`](00-overview.md) —— 总览与约定（范围 / 术语 / 表示约定）
2. [`05-boot.md`](05-boot.md) —— 启动入口（入口 → 初始化 → 主循环）
3. [`06-window.md`](06-window.md) —— 启动期的平台请求（窗口类 / 窗口 / 图形设备 / 消息泵 / 定时器；请求与少数生命周期行为，不到 Win32/D3D 内部与逐条消息处置）
4. [`10-format.md`](10-format.md) —— 脚本与执行（脚本头 / 指令编码 / 操作数编码 / 帧）
5. [`15-frame.md`](15-frame.md) —— 帧与调用（帧栈：帧记录 / 当前帧 / 父帧链 / 分配与释放；逐条控制转移见「帧类指令」）
6. [`20-alf.md`](20-alf.md) —— ALF 归档（索引头 / 段 / LZSS / TOC / 载荷）
7. [`25-fs.md`](25-fs.md) —— 文件与脚本装载（取件 / 索引）
8. [`30-agf.md`](30-agf.md) —— AGF 图像（ACGF 容器 / meta / body / ACIF / 像素）
9. [`40-pools.md`](40-pools.md) —— **数据池**（global/local 池、访问原语、类型标签、值编码、字符串、容量）
10. [`45-objects.md`](45-objects.md) —— 对象模型（根对象 / 类行为表 / 类型身份 / 对象槽表）
11. [`50-engine-state.md`](50-engine-state.md) —— 引擎级状态（按索引表）
12. [`55-config.md`](55-config.md) —— 配置（库 / 发行默认表 / 玩家文件 / 保存 / 脚本访问；键表见伴随数据 `format/config-keys.json`）
13. [`60-ops-numeric.md`](60-ops-numeric.md) —— 数值运算、比较与位操作
14. [`60-ops-array.md`](60-ops-array.md) —— 数组填充与寻址
15. [`60-ops-text.md`](60-ops-text.md) —— 字符串
16. [`60-ops-frame.md`](60-ops-frame.md) —— 帧类指令
17. [`60-ops-texture.md`](60-ops-texture.md) —— 纹理族
18. [`60-ops-scene.md`](60-ops-scene.md) —— 场景 / 颜色族
19. [`60-ops-render.md`](60-ops-render.md) —— 渲染请求边界（渲染目标 / 场景混合 / 离屏文字 / 帧步进）
20. [`60-ops-voice.md`](60-ops-voice.md) —— 音频（语音 / 音效 / BGM）
21. [`60-ops-list.md`](60-ops-list.md) —— 列表项与子系统写入
22. [`60-ops-save.md`](60-ops-save.md) —— 存档槽的读写与文件级操作
23. [`65-save.md`](65-save.md) —— 存档（槽身份 / 文件头 / 负载信封 / 持久化归属 / 恢复）
24. [`60-ops-forward.md`](60-ops-forward.md) —— 转发型指令
25. [`60-ops-scalar.md`](60-ops-scalar.md) —— 引擎级标量与事件
26. [`60-ops-wait.md`](60-ops-wait.md) —— 等待、影片与空操作
27. [`60-ops-input.md`](60-ops-input.md) —— 输入（输入字 / 键总数 / 采样 / 回调登记与派发 / 键鼠 / 触点查询 / 主循环模式字）
28. [`70-vm.md`](70-vm.md) —— 执行与时间（一次迭代 / 门 / 状态位 / 时钟 / 计时窗）

`40-pools.md` 是**一个完整主题**：池的族、下标、访问、值表示与容量都在这一章。

规则小节用 `### 标题` 标记；`<!-- rule: ID -->` 是内部关联用的 HTML 注释，阅读时不可见。
★ **一个章文件内可以有多个 `##` 分组**（例如各指令族章按族分组）：**章名取该文件里第一个 `##`**，其余 `##` 只是同一章内的分组标题，不是新章。
本版尚未**开写**的主题不在本目录（本目录只含已有正文的章节）。其中部分章节只写到域的一部分：该域仍未成篇的部分在正文里明确标成「本版不规定」（并给出原因），不表示整章已完成。

★ 脚本的字节布局、指令表与 `type` / `raw` 编码由**本规范**定义（协议自持）：表是本规范自带的
`format/opcode-table.json` 与 `format/config-keys.json`（分发时的伴随数据）。协议部分当前是**部分 / 观察值**，
见 [`10-format.md`](10-format.md) 的核验状态表；本规范不引用任何实现的源码或数据文件。
