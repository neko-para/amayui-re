# 长程轮次交接（第 1–12 轮）：守卫 / 残差 / 需人项

- id: REQ-01M60EQ77AENEJG9DA40HPWQDJ
- type: req
- status: done
- parent: REQ-01M5BK8QMQYK7XQG707YVMQ202
- verify: tools/test/emulator-cxx-audio-slots.test.mjs#音效槽量：shape ↔ 实现 ↔ spec 三处说同一件事

## 这一批长程轮次（12 轮）交回了什么

不是"进度表"——进度看 `pnpm tools requirements plan`。这里只记**下一轮接手时必须知道的三件事**：

### 1. 已收口 / 已推进（可复核凭据）

| 单 | 结果 | 凭据 |
|---|---|---|
| `REQ-01M5Z9SYN0AF3QC61B41AEZPTP`（音效槽量 10 vs 15） | **done** | `verify: tools/test/emulator-cxx-audio-slots.test.mjs#音效槽量：shape ↔ 实现 ↔ spec 三处说同一件事`；另有具名 CTest `emulator-core-audio-voice` |
| `REQ-01M5Z5D786WF65YWHFK7K8Z7Q8`（设置界面） | 仍 doing | 门位两条支路已补行为守卫（`check_gate_split_branches`）；文本记录字段表已取证；剩 tooltip 缩放（需人）与三条残差 |
| `REQ-01M60CFPK2X36ZCQD21M3E5G7E`（门位第 27 位） | **新开** | 挂在 `emulator-cxx 原生宿主` 下；判据写实 |

### 2. 新增的守卫（都会红，且都过了变异/手工验证）

`tools/test/emulator-cxx-audio-slots.test.mjs`（shape ↔ 两条实现 ↔ spec 的 storage 三处一致 + 判别力自检）·
`tools/test/spec-rule-refs.test.mjs`（代码注释里的 `CORE-…` 必须是真实条目 id 的前缀）·
CTest `emulator-core-audio-voice`（音效槽量 + 门位两条支路）· `tools/mutate-check.mjs` 加了 2 条变异，
并把"清单过期"从"守卫没红"里分开报。

### 3. 下一轮的第一件事

`data/spec/audit.json` 的 `RES-OPS-TEXT-SPAN-RECORD` 残差 ④：用三种写形态（`memcpy` 目的 / 字符串赋值到 `record+0x2c` /
逐字节写 `[record+2Ch]`）从 ADV 文本更新与 `0x82` 同族入口找"把记录文本写非空"的写者。
两种结局都能收口：找到 ⇒ 补读者合同；找不到 ⇒ 正文升级成"内容恒空（未观察到非空）"并记为引擎缺陷证据。
★ 本批最后一轮的 spec 改动**还没过 subagent 复审**——先送审再往下走。

### 需要人的三件（不要绕着等）

tooltip 显示缩放（真机同尺寸比字形像素高度）· `REQ-01M5Z9S8SVADWPYFDZH98QK98P` 的 AGERC 产品取舍 · 白闪残余的目视终审。
