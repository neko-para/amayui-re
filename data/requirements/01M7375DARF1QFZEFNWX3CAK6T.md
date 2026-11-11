# emulator-lift · 退出写存档 LZSS Encode（sub_4367B0）的 native override 落地

- id: REQ-01M7375DARF1QFZEFNWX3CAK6T
- type: req
- status: done
- parent: REQ-01M72GW002QVRSG5J5WJ0KB1KF
- verify: apps/emulator-lift/tests/native_override_e2e.cpp#native-override-e2e
- tags: [emulator-lift profile native-override lzss save]

## 一句话
把退出写存档尾段的 **LZSS Encode 整函数** `sub_4367B0`（`0x4367B0`，`retn 8`、thiscall）做成 native override：
**1 次拦截**覆盖尾段 **92.85%** 的墙钟（`incl=3328.1 ms` / `self=60.2 ms` / `calls=1`）。
可行性评估已由 spike `REQ-01M72QS2WFNP61Y6EGFAEVRFG4` 做完（结论：能，且整函数是首选）；本单只做**落地**。

## 已核（来自 spike + `docs/03-emulator/observations.md` OBS-20261110-05）
- 尾段 = 最后一帧之后 `3.58 s / 131.1 M` 条指令、窗口内 0 次 Present；`sub_4367B0` 子树占 93.46% 指令 / 92.85% 时间。
- 形状：`Compress` 对象长 `0xD430`（`+4 text_buf[4114)` / `+0x1018 dad` / `+0x501C lson` / `+0x9020 rson` /
  `+0xD424 match_pos` / `+0xD428 match_len` / `+0xD42C out`）；整函数会 `delete[]` 旧 `out` 再 `new[](*a3+4096)`
  ⇒ override 里要能在 **guest 堆**分配（`cpu.heap->alloc/free` 与 guest `operator new[]/delete[]` 同源，已验证）。
- **整文件 sha256 对 `SAVE.DAT` 不成立**：`[264..284)` 是 `GetLocalTime` / `timeGetTime` 落点，而虚拟时钟 = 指令数 × 100 ns
  ⇒ 两臂时间戳必然差 ~12 s。正确判据见下。
- 先例的洞：`nativeLzss436A80`（解压）成功路径恒写 `EAX=0`，而 guest 返回 `dst` ⇒ ⛔ 不要照抄。

## 修法
1. 在 `src/runtime/native_overrides.cpp` 的 `kKnown` 里加整函数项（`debug=true`、`arg_bytes=8`）并抬 `kMax`。
2. 实现与 guest **逐字节等价**的 LZSS 编码（同一棵 dad/lson/rson 树语义、同一 `match_len` 选择、同一 `out` 分配与返回 `EAX`）。
3. ⛔ 落 override 之前先钉死 `sub_4367B0` 出参 `a3[1]` 的语义（spike 的 `<待核>`：`0x436879` 写 18 vs `0x436A50` 写输入长度）。

## 守卫（修前必须红）
1. **pure**：合成树用例——`InsertNode`/`DeleteNode` 对拍（同一输入下宿主实现与 guest 实现的树结构/输出逐字节相同）。
2. **pure**：`lzssEncode` ↔ 现有 `lzssDecode` 往返（任意输入编码后解码回原文）。
3. **assets**：真字节两臂差分（4 KB 与 150,068 B 两档）：比**函数输出 + `info` + `EAX` + 整个 `0xD430` 对象**；
   存档**区域**差分时掩掉 `[264..284)` 的 24 B 时钟。
4. **端到端**：同一 `--input-script` 跑 TITLE→Quit 两臂（override 开/关），尾段 `wall` 必须显著下降，
   存档除时钟区外逐字节相同。

## 验收
- `--profile --profile-top 200` 两臂的尾段 wall 与 `sub_4367B0` 的 `incl` 读数（命令 + 读数，不接受"变快了"）。
- `ctest` 全绿 + `pnpm test` 绿 + `pnpm tools requirements validate` 6/6。
- 撤销方式写进正文：删 `kKnown` 一项 + 恢复 `kMax`。

## 交付（2026-11-11）
- **纯核 + 薄包装**：`lzssEncodeObject`（原地改 guest 的 `0xD430` 对象，不动宿主影子）+ `nativeLzss4367B0`（分配 / 写 `EAX` / `a3[1]` / `a3[2]` / 失败回退）。
  `kKnown` 加一项（`debug=true`）、`kMax` **13 → 14**（在 `native_overrides.h`）。
- **守卫**：pure `lzss/{tree-invariants, tie-strict-greater, independent-transcription, roundtrip-decode, wrap-address, admission-no-write}`；
  assets **9 档**真字节两臂（对象预填 `0xCC`、两臂各新 Fixture、`_crtheap` 预置）；e2e `native-override-e2e`（3 run：a1 空 / b 开 / a2 空，唯一差异 `--native-overrides sub_4367B0`）。
- **变异**：`InsertNode` 的平局判据 `>` → `>=` ⇒ `lzss/tie-strict-greater` + 独立转写红（实测 1/1）；
  复审另做 `s < 17u` → `s < 18u` ⇒ assets 4 条 `ovr.obj == base.obj` 红（差异 @0x1015）。
- **实测时钟掩码 = 4 字节 `[276..280)`**（a1 == a2 逐字节相同）⇒ 具名常量 `276/5`。
  ★ 任务单写的 24 与评估单的 `[264..284)` **都不对**——这就是"别照抄数字"的实例。
- **e2e 读数**：尾段指令 131,104,827 → **8,495,927**（−93.53%）· `native-ovr` 明细 `sub_4367B0 @ 004367B0 thiscall(8B) hits=1 fallbacks=0` ·
  墙钟 **7681.5 → 649.9 ms（11.8×）**；三臂 `stop=stopped / ExitProcess`。
- **门禁**：build 绿 · `-R native-override` 3/3 · `pnpm test` 546/0 · 变异 1/1 红 · `requirements validate` 6/6 · `git diff --check` 0。

## 已知红（**不是本单引起**）
- `quit-tail`：子进程日志首行 `没有 --game-dir` —— 夹具用 `game_dir.string()` 把日文路径转坏（pre-existing，已开 `REQ-01M73BWF51T1SKSSE6PV6X455A`）。
- `movie-logo`：nooverride 臂 `stop: time-limit`（墙钟抖动；单跑绿、复审跑绿）。
- `pnpm tools lift check` 4 条 pre-existing 红（`RaiseException` / `RtlUnwind` 与 JSON 不符）。

## 未做（已知边界，别当回归）
- **虚拟时钟差**：省 ~1.2e8 条 ⇒ 存档时间戳仍会比不 override 时早 ~12 s（spike 已裁决不在本单补偿）。
- 不覆盖 `sub_4366A0`（DeleteNode）/ `sub_437480`（写文件）/ `sub_438320`（序列化）/ `sub_40AAE0`（事务层）。
- 不证明"存档能被游戏读回"（那是另一条端到端单）。
