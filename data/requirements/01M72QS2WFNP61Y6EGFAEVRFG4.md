# spike：退出写存档尾段 >500 ms 函数的 native override 可行性（LZSS sub_4367B0 / sub_436550 + 包装层）

- id: REQ-01M72QS2WFNP61Y6EGFAEVRFG4
- type: spike
- status: done
- parent: REQ-01M72GW002QVRSG5J5WJ0KB1KF
- done_reason: 结论已写进本单正文（逐函数判定表 + 收益上界 + 最小第一步）；取证报告在 .tmp/quit-tail/{override-lzss,chain,profile}.md。本轮不实现 override。
- tags: [emulator-lift, profile, native-override]

## 一句话
判定：退出写存档那条尾段（3.58 s / 131.1 M 条指令）里**所有耗时 > 500 ms 的函数**，哪些能用 `emulator-lift` 的 native override 接管、收益多大、风险在哪、最小第一步是什么。

## 结论（逐函数，一行一个；self/incl 均为**实测**窗口读数，见 `docs/03-emulator/observations.md`）
| 函数 | EA | self | incl | 角色 | 判定 |
|---|---|---|---|---|---|
| `sub_4367B0` | 0x4367B0 | 60.2 ms | **3328.1 ms** | LZSS **Encode** 整函数（`retn 8`、thiscall、`EAX=this+0xD42C`、`info={in,raw,comp}`） | **能 —— 首选**：1 次拦截换 92.85% 的尾段 |
| `sub_436550` | 0x436550 | **1265.1 ms** | = self（叶子） | InsertNode（150,086 次调用，只写 `this` 内的树） | 能，但**单独做是最差组合**：150,086 次拦截只换 1/2.63 的收益，循环还在 |
| `sub_4366A0` | 0x4366A0 | 36.0 ms | = self | DeleteNode | 能；收益小，可随整函数一起被覆盖 |
| `sub_437480` | 0x437480 | ≲0.2 ms | 3398.0 ms | 292 B 头 + 20 B 校验 + payload，`arg_14>=2` 才压缩，`WriteFile`×3 | 能但**无意义**（自有区间 69.9 ms，含 CRC 61.1） |
| `sub_438320` | 0x438320 | 3.3 ms | 3409.1 ms | 存档**序列化**（`_strcpy_s`×3043） | **不能**（= 用宿主重实现存档格式） |
| `sub_40AAE0` | 0x40AAE0 | ≲0.2 ms | 3417.4 ms | 存档**事务层**（`$$SAVE.DAT`→`SAVE.BAK`→`SAVE.DAT`） | 能但**不建议**（要接它就得接管序列化；`rand()`×3 + `GetLocalTime` + 双 `MoveFileA`，错一次丢档） |
| `sub_418E60` | 0x418E60 | ≈0 | — | 命令表 slot 1 的 handler，抛 `Command_Exit_Exception` | 无意义 |
| `_CxxThrowException` / `___InternalCxxFrameHandler` / `FindHandler` / `CatchIt` / `_JumpToContinuation` | — | 合计 < 2 ms | — | EH 派发链 | **不能/无意义**：`_CxxThrowException` / `_JumpToContinuation` **不返回**（后者末尾 `FF E0 jmp eax`、无 `ret`），`NativeFn` 只有"写 EAX 正常返回"一条通路 |

## 关键事实（会改变判据）
1. **写存档不在异常展开里**：`catch(Command_Exit_Exception)` 在 `0x413551`，续行 `0x41389A` → `SendMessageA(WM_CLOSE)` → WndProc case 16 → `sub_40AAE0`。展开阶段合计 ≈0.24 ms ⇒ **接管任何 EH 帧一分钱都省不到**。
2. **整文件 sha256 差分对 `SAVE.DAT` 不成立**：`[264..284)` 是 `GetLocalTime` / `timeGetTime` 落点，而虚拟时钟 = 指令数 × 100 ns ⇒ override 省 ~1.2e8 条 ⇒ 两臂时间戳必然差 ~12 s。正确判据 = ①函数级真字节差分（比输出 + `info` + `EAX` + **整个 `0xD430` 对象**）②存档**区域**差分（掩 24 B 时钟）。实测 7 个独立 `--appdata` 的 `SAVE.DAT` / `RT.DAT` 各自全等 ⇒ 单臂可复现。
3. **形状已核**：`Compress` 对象布局 `+4 text_buf[4114)` / `+0x1018 dad` / `+0x501C lson` / `+0x9020 rson` / `+0xD424 match_pos` / `+0xD428 match_len` / `+0xD42C out`，总长 `0xD430`；`sub_4367B0` 会 `delete[]` 旧 `out` 再 `new[](*a3+4096)` ⇒ 整函数 override 需要「在 override 里分配 guest 堆」的机制（已验证 `cpu.heap->alloc/free` 与 guest `operator new[]/delete[]` 同源）。
4. **先例的洞**：`nativeLzss436A80`（解压）成功路径恒写 `EAX=0`，而 guest 返回 `dst` ⇒ 新 override ⛔ 不要照抄这一条。

## 收益上界（估计，不是读数）
尾段 `wall=3584.4 ms`；`sub_4367B0` 子树指令占 **93.46%**、时间占 **92.85%** ⇒ 整函数换掉后尾段 ≈ **257–266 ms**（残余：`sub_437480` 自有 69.9 / 析构 87.8 / 输出释放 50.8 / 注册表 26.8 / 序列化 11.1 / 事务层 8.3）。

## 判据（本单是 spike，产出=结论）
1. 尾段里**所有** self ≥ 500 ms 或 incl ≥ 500 ms 的函数都在上表里，每行带 `self/incl/calls` 读数或"叶子 ⇒ incl=self"的判据。
2. 每行都有 `能 / 不能 / 无意义` 的判定 + 一句**可复核**的理由（EA 指令原文，或"不返回 ⇒ `NativeFn` 表达不了"这类机制判据）。
3. 「写存档不在异常展开里」有静态锚（`0x413551` 的 catch + 续行 `0x41389A` 的 `SendMessageA(WM_CLOSE)`）。
4. 正确性判据说明**为什么**整文件 sha256 对 `SAVE.DAT` 不成立（时钟区间 + 实测 7 个 appdata 全等）。
5. 最小第一步可执行（改哪些：`kKnown` 一项 + `kMax` + 守卫文件），且写明**撤销**方式与**不解决**清单。

## 最小第一步（若要做）
整函数 `sub_4367B0`（`debug=true`、`arg_bytes=8`、同时抬 `kMax`）：
① 纯档合成树用例（`InsertNode`/`DeleteNode` 对拍）② assets 真字节两臂差分（4 KB 与 150,068 B 两档，按上面第 2 条的判据）③ `lzssEncode` ↔ 现有 `lzssDecode` 往返。
**撤销** = 删 `kKnown` 一项。**不解决**：虚拟时钟差（时间戳仍会早 ~12 s）、`sub_4366A0`、也不证明存档能被游戏读回。

## 未做 / 待核
- `sub_4367B0` 出参 `a3[1]` 的最终语义（`0x436879` 写 18 vs `0x436A50` 写输入长度）落 override 前必须钉死。
- 5 个 COLLAPSED 函数没有指令原文（`_CxxThrowException` / `___InternalCxxFrameHandler` / `FindHandler` / `CatchIt` / `___crtExitProcess`）⇒ EH 侧判定用的是"有没有 `ret`"这一条硬依据 + self 读数。
- 本轮**没有实现任何 override**；`kMax` 未动。
