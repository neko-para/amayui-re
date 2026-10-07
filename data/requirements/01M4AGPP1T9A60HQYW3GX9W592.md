# M4-1b · 走完整条启动链（SYSTEM4 → … → LOGO → INIT → TITLE）+ 指针族与容器句柄 + 剩余取证包登记（订正类）

- id: REQ-01M4AGPP1T9A60HQYW3GX9W592
- type: req
- status: open
- parent: REQ-01M47SGXYWPQ1KWET2J49TJQST

## 这一单要什么

**按标准的从 `SYSTEM4` 启动的流程实现**（用户口径 A2）。根脚本 = **统一文件 id 0**（已登记观察）；`LOGO.BIN` 是 `SYSTEM4` 里的 `call-script 5262`。

## 已经做完的（本轮）

1. **启动链缺口报告**：`pnpm emulator --chain-report [--json]` —— 纯静态、可复跑，把「实现启动链」变成有数字的清单。
   实测（本机安装，`--until LOGO.BIN` 截断）：
   ```
   闭包：65 份脚本 · 共 74541 条指令
   截断于 SYSTEM4.BIN#133 的 call-script LOGO.BIN（id 0x5262）
   缺 handler 的 opcode：79 种
   最大几块：set-string(7775 次/28 份) · lookup-array-2d(1327/8) · lookup-array(214/17)
             jcc(195/19) · copy-local-array(167/7) · jmp(127/19) · call-script(66/6)
             fill-zero(51/5) · save-int(29/9) · load-int(28/3) · call(10/6) · save-string(10/2) · ret(8/5)
   ```
   ★ **截断是必须的**：不截断时闭包 = 212 份脚本 / 15 万条指令（`TITLE` 会把整个游戏拉进来）。
2. **按 id 装载**（引擎自己的寻址方式）：`host/scripts.ts` 的 `ScriptLoader` + `AlfIndexSource.nameOfId` + 前端装配（id→名字→分层 fs 取字节，**松散文件优先**）。
3. **帧机制**：`Machine.pushFrame/popFrame`；`call-script`(0x03) 压帧（caller = 调用者 `cur`）、`ret`(0x05)/`exit`(0x02) 弹帧。
4. **控制流**：`jmp`(0x8c)、`jcc`(0xa0)、`comment`(0x1a7，已取证 = no-op)。
   ★ `jcc` 的形状由**脚本自身的控制流**三处独立互证：`jcc <cond> <trueLabel> <falseLabel>`，`0xFFFFFFFF` = 该支无目标（落下）。判据逐字在 `ops.ts` 的 `opJcc` 头注里。
5. **可复核的"跑到哪"**：停止原因带 **脚本名 + 指令下标 + opcode**；`--script <名字>` 保留为**直装模式**（跳过启动链，旧守卫仍用它）。
6. 守卫：`emulator-headless-logo.assets.test.mjs` 新增两条（按 id 装载 + 链规模可复算，缺口种数是**棘轮**只许下降）。

## 还缺什么（按实现顺序，数字由 `--chain-report` 复算）

* **字符串那一族**（最大一块，`set-string` 7775 次）：需要 28 字节元素 = **MSVC `std::string`**（布局已取证，见台账）与"源可以是数字 → `_itoa_s` 文本"。
* **数组/表那一族**：`lookup-array`(0x61) · `lookup-array-2d`(0x12c) · `copy-local-array`(0x64) · `fill-zero`(0x6c)。
  ★ **`fill-zero` 填的不是 0** —— 它填 `Engine+0x5EC90`（= `ENC(key,0)`）。
* **配置四条**：`load-int`/`save-int`/`load-string`/`save-string` —— 键是**运行时拼**出来的（`"%c%8.8x"`），没有静态键表。
* **其余无名 opcode**（SYSTEM4 前几条就在撞：`0x1a8 dev_ukn` 等）。
* **`call`(0x8f) / `load-frame`(0x06)**：帧语义的另一半（与 `call-script` 的差别待取证回报）。
* **动态 `call-script` 目标**：实测 `SETSTAGE.BIN#70` 的操作数 0 **不是立即数**（运行时才算得出）
  ⇒ 静态报告算不出它（已显式列出，不静默丢）；**实现侧不受影响**（`opCallScript` 按普通操作数读）。

## 已登记的台账

* `KN-01M4ASXQ587J7G7E6E5R3R2Y2K` —— 四条会误导实现的结论（`fill-zero` 不填 0 / 配置键运行时拼 / 28 字节元素布局 / `set-string` 按目标 type 分派）+ 数组族越界不检查等。

## 复算与验收

```
pnpm emulator --install <安装> --chain-report     # 缺口清单（脚本数 / 指令数 / 缺的 opcode）
pnpm emulator --install <安装>                    # 标准启动流程：跑到哪、停在哪条（脚本名 + 下标 + opcode）
pnpm test:assets                                  # 链规模与缺口种数棘轮
```
