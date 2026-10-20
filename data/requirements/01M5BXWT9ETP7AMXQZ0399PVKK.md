# emulator-new 执行 LOGO.BIN（spec 驱动：VM 派发 + 操作数原语 + CORE-OPS 按族推进）

- id: REQ-01M5BXWT9ETP7AMXQZ0399PVKK
- type: req
- status: done
- parent: REQ-01M3TCB0S0BTW41KW0JBJMWHHH
- verify: tools/test/emulator-new-logo.assets.test.mjs#★ 前沿棘轮：LOGO.BIN 执行到
- tags: [emulator, engine spec]

## 判据（已达到）

`apps/emulator-new`（只读 `data/spec` 的参考实现）能执行真实脚本 `LOGO.BIN` 并**停在 `0x20F play-movie`**：

1. **执行，不是解码**：每条被执行的指令都改它该改的引擎数据（池 / 纹理 / 绘制项 / 网格 / 计时窗 / 等待门），并留下可断言的观察。
2. **每条 opcode 有 spec**：LOGO 路径上出现的每个 opcode 都在 `data/spec/chapters/` 里有规则、`bindings.json` 有 EA 站点；未实现的 opcode **停在它前面并报缺口**（⛔ 不 no-op）。
3. **守卫**：`tools/test/emulator-new-logo.assets.test.mjs` 的前沿棘轮钉住"执行到第几条 / 停在哪"；数值族 / 纹理族 / 场景与等待门各有 pure 守卫。
4. **门禁**：`pnpm test` / `pnpm tools spec validate` / `pnpm tools requirements validate` / `pnpm typecheck` 全绿。

## 落地（凭据）

- **实跑**：`LOGO.BIN` 执行 46 条，停在 `0x20F`（第 46 条前）；虚拟时钟 314 格 / 5024ms（16ms/格），等待位已清。
  引擎数据：两个网格（顶点数 4、顶点色被 0x322 烘过）、纹理槽 42 重建为空白纹理、43 被释放、
  绘制项被 0x1f7 区间拆离后由 0x1fb 重画；6 次"未初始化浮点读"全部进诊断（显式 zero 策略，不是静默补 0）。
- **spec**：`60-ops.md` 的数值 / 纹理 / 场景 / 等待与输入 / 影片规则；`70-vm.md` 的迭代 / 门 / 状态位 / 时钟 / 计时窗。
- **守卫**：`emulator-new-logo.assets.test.mjs`（assets）· `emulator-new-ops` / `emulator-new-texture` / `emulator-new-scene`（pure）。
- **已知不绿（与本单无关）**：`pnpm test:assets` 仍有先前就红的 3 条本机环境项（spec-isa 1 / corpus 2；shape 的 5 条见 `REQ-01M5CBD13M3XQYKTA2YFA82YJC` 已收口），本单的 assets 用例通过。

## 非目标

- 渲染 / 音频 / 输入的**呈现语义**不在本单（core 的边界，见 `data/spec/core/README.md` §1）；只规定"是否改引擎数据、改了哪一部分"。
- 完整启动链（SYSTEM4 → INIT → TITLE）不在本单；本单只要 LOGO.BIN 这一段能执行。
- ⛔ 不许用 `apps/emulator` 的代码当实现来源（cleanroom：只读 spec + 语料 EA）。

## 下一步（另开单）

- 执行 `0x20f` 本身并处理 0x1fa 对共享对象槽的影响（引用计数未建模）→ 再收 `0x2 exit` / `CORE-FRAME`。
- 把本轮四族规则从 `draft` 推进到 `reviewed`（补 accepted 台账锚）。
