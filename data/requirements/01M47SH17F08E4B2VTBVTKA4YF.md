# M4-2 · 模拟器表现层与窗口（renderer/electron/web + host-input + 插件）

- id: REQ-01M47SH17F08E4B2VTBVTKA4YF
- type: req
- status: open
- parent: REQ-01M3TCSNH00RVXCPZGM1J6EXRC
- order: 62
- tags: [emulator]

## 范围
旧仓 `app/amayui-emulator` 的**表现层与窗口**按新结构重写：`src/renderer`（46 文件 / 844 KB，Pixi 后端）·
`electron/`（窗口壳 8 文件：`main` / `preload` / `windows` / `nativeAddon` / `paths` / `logging`）·
`src/web`（浏览器 / 消息桥）· `packages/host-input`（N-API + CMake 原生输入）· `plugins/amayui-emulator`。

## 判据
（待写实：真窗口下模拟器可跑；原生输入与真机一致（缺原生产物时**加载器降级 + 一行诊断**，不让缺编译器变成崩溃）；
T2 档用例（`e4-gamestart-shot`）可显式跑。）

## 与其它节点的关系
* 前置 M4-1（无头核心）；**不是**知识线 K2/K3 的前置（T2 档不进 `verify`）。
* ★ **`apps/inspector` 不是本节点**：那是真机探针（C# / .NET 10 + WPF，读真游戏进程内存），归 **M7**。
