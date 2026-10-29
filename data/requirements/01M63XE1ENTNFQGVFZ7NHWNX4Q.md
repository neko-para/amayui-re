# spec 成篇 · 文本槽/时间戳/显示状态（0x300/0x301/0x23c/0x25b/0x2ee）

- id: REQ-01M63XE1ENTNFQGVFZ7NHWNX4Q
- type: req
- status: done
- parent: REQ-01M5BK8QMQYK7XQG707YVMQ202
- order: 4
- verify: tools/test/spec-gaps.test.mjs#★ 精确划分
- tags: [engine spec]

## 判据（可判定）

`audit.gaps[]` 的 `GAP-OPS-0200-02FF` 里不再出现：`0x300` `0x301` `0x23c` `0x25b` `0x2ee`；
各自有规则标题、`bindings` 站点、`audit.rules` 条目；`pnpm tools spec validate` 13/13 绿 + 精确划分守卫绿。

## 已落（2026-10-29）

- `CORE-TIMING-STAMP-23C`（`70-vm.md`）：无条件重盖时钟对；同时把 `CORE-TIMING-CLOCK` 里"0x23c 尚未成篇"的措辞改成指向本条。
- `CORE-OPS-RENDER-DISPLAY-STATE-25B`（`60-ops-render.md`）：显示状态 ← 2、图像号 ← 操作数（无条件），**仅窗口态**才发载图请求。
- `CORE-CONFIG-MESSAGE-FADE-2EE`（`55-config.md`）：**双写**（缓存格在前、命名键在后）。
- `CORE-OPS-TEXT-SLOT-TIMER`（`60-ops-text.md`）：十个文本槽的「状态位 + 期限 + 起点」与**服务侧**（主循环文本推进器，只服务槽 1..9）
  一起规定 —— 写侧与服务侧同一条规则，避免"设了期限却不释放"被当成合法。

## 独立复审（第 3 批）与处置

- 硬伤「0xb7 的进度」不在本单（属 `REQ-01M63Y9113D16G0W1GC178SZB1`）。
- 本单被指出的：① 0x300 的**读序**没写（写状态位在读期限之前）→ 已补；② "第 16 位写不进来"与 OR 公式矛盾 → 改成"旧值总被保留"；
  ③ 期限 0 的语义（不是无期限、而是当场到期）→ 已写；④ 参数格判据是"**等于 0**"（不是大于 0）→ 已写；
  ⑤ "十个文本槽"与另两张十项表是否同一张 → 进残差 + 明确"不许默认可互相索引"；
  ⑥ 服务侧的计时器接口 / 什么叫资源"忙" / "现在"取哪次采样 → 进残差 `RES-OPS-300-SERVICE-INTERFACE`；
  ⑦ "每帧"与"没有固定帧率"的措辞 → 改成"每次迭代"，并注明位置见「ADV 文本」条。

## 已知欠账

- `op_25b` / `op_2ee` 在参考实现里只写标量格 ⇒ 已登记成 `REQ-01M63YS3T54VYCXCBJ6RPCHDAY`。
- 本条的 evidence 是通用 CTest（`emulator-core-selftest`），升 reviewed 前要具名断言。
