# M7-2 · 探针 WPF 壳（App：面板 / 全局表 / 帧栈 / Hex，手动快照）

- id: REQ-01M47T0TNAG94EKC6FP2GPGQQR
- type: req
- status: open
- parent: REQ-01M3TCZ5A0D5ZYZQS47DZW2S3X
- order: 92
- tags: [inspector dotnet]

## 范围
`AmayuiInspector.App`（22 文件 / 50 KB）按新结构重写：`ViewModels/`（Main · EnginePanel · GlobalTable ·
FrameStack · Dispatch）· `Views/`（MainWindow · EnginePanel · GlobalTableControl · FrameStackControl · HexView）·
`Services/`（异步刷新服务 · opcode 名对照 Mapper）。

**界面口径（沿用旧仓已定的，别重新发明）**：**只做手动快照**（`Task.Run` 后台读 → `Dispatcher` 回填
不可变快照），**不做定时轮询**（留接口）；全局表默认"筛选"视图（仅非空 / 掉落区 / 指定范围），
"全量"（≈7.4M 槽）另设按钮；★ 所有 `ReadProcessMemory` 在后台线程，UI 只持有快照。

## 判据
（待写实：能在真机上起窗口、选进程、扫出 `this`、刷出一份快照并把它显示成面板/表；
`this` 失效时能重扫而不是崩。）

## 前置
M7-1（只读快照的对象形状由它定）。★ 本节点**需要真机**才能验收（Windows + .NET 10 + WPF）。
