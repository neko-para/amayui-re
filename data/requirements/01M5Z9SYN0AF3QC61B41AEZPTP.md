# 🐞S2 音效槽量不一致：实现按 0..14，引擎侧 T_SE 只有 10 格且不检查越界

- id: REQ-01M5Z9SYN0AF3QC61B41AEZPTP
- type: bug
- status: done
- parent: REQ-01M5BK8QMQYK7XQG707YVMQ202
- verify: tools/test/emulator-cxx-audio-slots.test.mjs#音效槽量：shape ↔ 实现 ↔ spec 三处说同一件事
- repro: apps/emulator-cxx/src/core/vm.h#se_slot_resource_
- severity: S2

## 结论（已按取证收口）

音效有**两个**不同的槽量，实现以前把它们合成一个 `15`：

| | 数量 | 取证 |
|---|---|---|
| 声音后端（`T_Sound`）的槽 | **15**（0..14） | 三条受理边界：播放 `0x4b603e cmp edi,0Eh / ja`、装载 `0x4b65c5 cmp edi,0Fh / jl`、释放 `0x4b639a cmp edi,0Fh / jl`；shape 里 `T_Sound` 的 +0x51c / +0x564 / +0x5a0 / +0x5dc / +0x618 / +0x690 / +0x7f8 七张表 count 15 |
| 引擎自己的「槽 → 当前资源号」表（`T_SE+0x4BC`） | **10**（0..9） | 构造器 `sub_4B4E60` 逐格清 10 个（`0x4b4eab`..`0x4b4ee1`，0x4BC…0x4E0）；重装循环 `0x4b50db cmp edi,0Ah / jge`；**对象边界互证**：`Engine+0x143bc` 是 T_SE、下一块嵌入对象在 `+0x148a0`（`0x4156ea` / `0x4156f9`）⇒ 大小 0x4E4，而 0x4BC + 10×4 = 0x4E4 正好是表尾 |

⇒ 装载成功路径 `0x4b5004 [T_SE+0x4BC+slot*4] ← 资源` 与停止路径的清格（`0x4b506a` / `0x4b5075`）**都没有边界检查**：
槽号 10..14 落在**表外**（写进下一个嵌入对象，未定义）。所以原单的"实现按 0..14 判、引擎不检查越界"要拆成两件事说：
**后端**确实受理到 14；**引擎表**只有 10 格。

## 处置

- `emulator-cxx`：`kSeSlotTableSize = 10` / `kSoundSlotCount = 15` 两个常量（取证写在 `vm.h` 里）。
  0xb4：0..9 写表；10..14 照常记装载请求 + 诊断 `se-load-slot-past-engine-table`、**不写表**；≥15 后端拒绝（不记请求、不写表）。
  0xb6：0..9 记释放请求并**清那一格**；10..14 记请求 + 诊断；≥15 只诊断。
  ★ 顺带订正 0xb6 的旧断言"没有后端 ⇒ 不清格（规范：不清）"——规范里此前**没有** 0xb6 这一条（它在未成篇 gap 里），
  而 0xb4 又按"后端存在"写了表 ⇒ 同一对象上两条指令用了两套前提。现在两条统一按「后端存在 + 后端受理」建模。
- `emulator-new`（参考实现）：同样两常量（`machine.ts`）+ `seSlotResource` 10 格；0xb4/0xb5 的越界各记诊断、不发请求/不写表。
- spec `CORE-OPS-AUDIO-WAV`：删掉"不改任何引擎数据"（被 `0x4b5004` 证伪），补"装载成功写该槽资源号格"、
  "表 10 格 / 后端 15 槽"、"10..14 是已知偏离"；bindings 加 `storage: shape:T_SE@0x4bc, shape:T_Sound@0x51c`
  与 6 个站点；audit 加两条残差（表外写 / 后端不存在）+ 三条 falsifier。
- 顺带：代码注释里 4 个**不存在**的规范条目 id（`CORE-OPS-AUDIO-SE-STOP` / `-BGM-RESTART` / `-BGM-STOP` / `-MODE`）
  与 `emulator-new` 的 8 处 `CORE-FRAME-*`（少一段 `OPS`）都改成真 id 或"本版尚无规范条目"，并新立守卫防复发。

## 守卫

- `tools/test/emulator-cxx-audio-slots.test.mjs`：shape ↔ 两条实现 ↔ spec 的 storage 指针三处对不上就红（含判别力自检）。
- `emulator-core-selftest`（CTest，顶层注册 ⇒ pure）：`check_audio_slot_table`（读 `data/shape/types.json` 比数量）
  + `check_audio_slot_bounds`（0/9/10/12/14/15 六个槽号逐个咬）。
- `tools/test/spec-rule-refs.test.mjs`：代码注释里的 `CORE-…` 必须是真实条目 id 的前缀。
- `tools/test/emulator-new-audio.test.mjs`：0xb4 写格 + 两个界的诊断。

★ 残差（不在这张单里）：表外写（槽 10..14）在引擎里写进的那个嵌入对象是什么、那笔越界是否可观察 —— 见 audit 的
`RES-AUDIO-WAV-SLOT-OOB`。
