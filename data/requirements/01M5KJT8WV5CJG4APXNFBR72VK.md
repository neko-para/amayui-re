# emulator-new · TITLE 输入循环：CORE-INPUT 边界 + CLI 输入脚本 + 未建模子系统推进

- id: REQ-01M5KJT8WV5CJG4APXNFBR72VK
- type: req
- status: done
- parent: REQ-01M3TCB0S0BTW41KW0JBJMWHHH
- verify: tools/test/emulator-new-title.assets.test.mjs#★ TITLE 输入环

## 判据

- `data/spec/chapters/60-ops-input.md` 的 `CORE-INPUT` 边界（输入字 / 回调登记与派发 / 键鼠查询 / 模式字）在 `apps/emulator-new` 里落地，且 `pnpm typecheck` 绿。
- TITLE 能在 CLI 下推进到输入循环：不再因音频 `0xb4` 停住，音频 / ADV 文字 / Live2D 按 `00-overview` 的「未建模子系统（实现注记，不是规范合同）」逐条记录请求并推进；`0xcd` / `0x100` 的控制转移与返回栈正确。
- CLI 提供脚本化输入（`--input` / `--input-script`），无输入时不会 CPU 全速空转：`sleep 1` 推进虚拟时间，输入脚本耗尽后以 `input-idle` 停因收口。
- 守卫：`tools/test/emulator-new-input.test.mjs`（输入族）、`tools/test/emulator-new-audio.test.mjs`（音频请求边界）、`tools/test/emulator-new-title.assets.test.mjs`（真实安装到 TITLE 输入循环）在对应门禁档下绿。

## 边界

- 音频输出、ADV 文字显示、Live2D 内部语义仍不在 spec；本单只要求它们的**请求边界**与 TITLE 所需输入。
- 鼠标命中查找 `0x12e`、外部对话框 `0x140`、退出清理 `0x23d` / `0x32b` 仍留 gap；鼠标移动若触发 `0x12e`，实现应停在它前面并报告缺口，不伪造命中。
- 键 4（打开菜单）之后的菜单/ADV 路径仍可能撞到未成篇的类型分派（`0xa2` 的文本键）；本单只收口到输入环，菜单内部另单。
