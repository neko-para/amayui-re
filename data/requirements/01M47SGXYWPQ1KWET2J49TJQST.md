# M4-1 · 模拟器无头核心（vm/script/save/text/audio/frame/host + fixture 测试设施 + 拆守卫）

- id: REQ-01M47SGXYWPQ1KWET2J49TJQST
- type: req
- status: open
- parent: REQ-01M3TCSNH00RVXCPZGM1J6EXRC
- order: 61
- tags: [emulator]

## 范围
旧仓 `app/amayui-emulator` 的**无头核心**按新结构重写：`src/{vm,script,save,text,audio,frame,host,util}`
+ **fixture 驱动的测试设施**（旧仓三档：T0 纯合成 / T1 需要语料与真存档槽 / T2 需要真 Electron + GUI 会话）。

★ **为什么只有它挡知识线**：实测 79 条 `modeled-verified` 的守卫文件**全部落在 T0/T1**
（T2 全仓只有 `e4-gamestart-shot.test.ts` 一个文件，且不进 `all`/`verify`）⇒
K2 的复核**不需要真窗口、不需要原生输入、不需要真游戏安装**。

**本节点不含**（→ M4-2）：`src/renderer`（Pixi）· `electron/`（窗口壳，8 文件）· `src/web`（浏览器 / 消息桥）·
`packages/host-input`（原生输入）· `plugins/amayui-emulator`。
★ B 类守卫跑的是 `StubNative`，旧仓引用 `native/host-input` 的地方只有 3 处、全在输入路径 ⇒ 押后无风险。

## 子节点
* **拆跨域守卫** `REQ-01M47T0D3C3S6EDTAPMG347DY3` —— 10 个按域归位（4 个台账守卫 → M3）。

## 判据
（待写实：无头实例可跑（T0 档在无窗口下全绿）；fixture 驱动的守卫可枚举、可单跑。
★ 跨域守卫的归位判据在子节点里，不在这里复述。）
