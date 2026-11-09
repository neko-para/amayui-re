# emulator-lift · M1 · guest CPU 核心 + §3.1 T0 七族基座

- id: REQ-01M6H6FW5JGQGHACXHPWZD4Y3E
- type: req
- status: done
- parent: REQ-01M3TC7BK0J9TBQKGGWQ35A3NR
- done_reason: M1 判据 A1–A5 全绿（2026-11-09 macOS arm64 / RelWithDebInfo）：ctest 65/65；conformance 260 cases / 571 checks / 0 failures（含 24 条 sse-*）；locale-ext 548 checks / 0 failures；A2 未映射 0、A3 decode/len/mnem 不一致 0；A4 eip=0x004BA890、两次 trace 逐字节相同。三个遗留子单已收口，判据挂各自 CTest。
- tags: [emulator-lift m1]

## 一句话
`apps/emulator-lift` 的第一条可运行线：执行**拆壳件的原机器码**，穿过静态 CRT 启动走到 `_WinMain@16`，并把 `platform-effort.md` §3.1 的 **T0 语义七族**做成自写的 Win32 形状层。
★ 判据与切片的真源：`apps/emulator-lift/docs/plan-m1-cpu-base.md`；本单只记**边界 / 判据 / 状态**。

## 已裁决（用户）
| # | 内容 |
|---|---|
| D1 | 执行源 = 拆壳件字节（`binary/age-sectfix`，已入库 LFS：`corpus/assets/age/AGE.EXE__dumped.sectfix.EXE`）；`.lst` 降为符号 / 对照 / 覆盖真源 |
| D2 | 纯内存解释 + 块缓存，**默认零生成代码** |
| D3 | x87 算术内核 = vendor 的 Berkeley SoftFloat `extF80`（BSD-3，`third_party/berkeley-softfloat/`），不进 vcpkg |
| D6 | 基座口径 = T0 七族；**不含** GDI32 的 7 个 / d3dx9 的 6 个；USER32 只留字符串族 |
| — | AGERC 整体按 `SCOPE.md` stub（3 个导出返回合理值 + 登记），**不做**真模块装载 |

## 判据
| # | 判据 | 档 |
|---|---|---|
| A1 | 语义夹具全绿 + `LIFT_SAFE_MEMORY=1` 复跑全绿 + 至少两处变异能变红 | pure |
| A2 | 助记符未映射 = 0 | pure + assets |
| A3 | 解码对照：`EA` 差与助记符不一致 = 0 | assets |
| A4 | 到 `_WinMain@16`；同一命令两次 trace 逐字节相同；**输入清单固定**（路径 / 命令行 / 环境块 / 时钟起点） | assets |
| A5 | trace 里出现的导入 ⊆ 实现 ∪ 登记 stub；**七族内不许是 stub**（除非写进 `SCOPE.md`） | assets |

## 验收（2026-11-04 · M1a，审查者独立复跑 —— **历史快照**，当前数值见下）
- `ctest` **10/10 通过**；A1 `114 cases / 267 checks / 0 failures`
- A2 `136/136`（未映射 0）；A3 `entries=298299 checked=298299`，`decode_fail / len / mnem = 0 / 0 / 0`
- A4 到达 `eip=0x004BA890`，两次运行 hash 相同
- A5 动态缺口 **0**；静态缺口 213/310
- 仓库侧 `pnpm test` 1148 pass；`SCOPE.md` / 语料 / 旧仓未越界

## 现查（2026-11-04 工作树 · macOS arm64；数字变了就重跑同一组命令）
- `ctest` **25/25**（含 `codepage` / `noop`）；`LIFT_SAFE_MEMORY=1 ctest` **25/25**。
- A1 `conformance` **183 cases / 383 checks / 0 failures**；A2 `136/136` 未映射 0。
- A3 `entries=298299`、`len_checked=298299`、`decode_fail/len/mnem = 0/0/0`；collapsed 1805。
- A4 到达 `eip=0x004BA890`；`insns=60660 hash=E0379BF37C2DFAF3`（`LIFT_AGE_EXE` 为相对路径）；
  两份 shim trace 各 108,914 字节且逐字节相同。
- A5 静态缺口 **164/310**（其中 34 条已按 `SCOPE.md` §1 no-op）、七族内 stub **0**。★ "动态缺口 0" 在裸缺口=0 时近乎恒真 ——
  独立证据是 **boot 到 L2 期间没有命中任何 stub**（命中即 trap）。
- 覆盖质量（2026-11-04 审计）：146 实现 = **100 real / 26 thin-legit / 20 thin-suspect**；
  reason 里的"S6 后续 / M1b 之后"等错误批次词已统一（归属见 `REQ-01M6K68YRBG3GB9ETF2P478VS1`）。
- `pnpm test` 1159 pass / 0 fail；`pnpm tools lift check` 绿。

## 收口（2026-11-09）
- 早期已收口：S6 三族夹具 · 码页 A/W · 文档漂移 · S0 覆盖清单 / S6 后四族 / S8 / A4 输入归一化。
- 2026-11 最后四条：PE 载入静默跳过 · Virtual* 三处 · 码页族三条质量缺陷 · 平台 / VEH 证据（Windows 侧）。
- 本轮三条遗留（判据都在 lift 自己的 CTest 上）：
  CT_CTYPE1 非 ASCII 区间规则 + CT_CTYPE2/3 明确失败（`locale-ext#get-string-type-w-ctype1`）·
  LCMapStringW 假名/全角/BYTEREV + SORTKEY 明确失败（`locale-ext#lcmap-string-kana-width`）·
  S4-SSE 14 形态按 SDM 实现（`conformance#sse-movss-load-zero-high`，24 条 `sse-*`）。

## 已知边界（不阻塞收口，要点名）
- `--until` 之外曾停在 `USER32!wsprintfA`（`LoadLibraryA("AGERC.DLL")` 的失败分支）⇒ 由 AGERC stub + `wsprintfA` 解决。
- 码页：932 / CP_UTF8 的 A/W 转换走 `runtime/cp932` 真表（`REQ-01M6K4VCMVGT95RRBRRK9Q22PA`）；
  **1252 / 437 等其它码页**仍只有 ASCII（首次用记一行日志，本作 `GetACP()=932` 未观察到使用）。
- `GetStringTypeW` 的 CT_CTYPE1 位抄错、`LCMapStringW` 的 `cchSrc=-1` 只映射 1 字符
  （两张缺陷单已开：`REQ-01M6K69JNGE7EP8ZNRDAXNSYZZ` / `REQ-01M6K69K28AT5VNKZAJXT6PZJ4`）；
  标准句柄三元组也是空实现（`REQ-01M6K69K9HM3ETCDNMJWPHGXJC`）。启动路径不依赖，但**不能再声称"七族全部实现"**——
  七族内 stub 为 0，质量分层是 17 条 thin-suspect。
- 164 条静态缺口里：注册表 5 / 音频 11 / 视频 15 / 摇杆 3 已按 `SCOPE.md` §1 no-op
  （`advapi32_registry.cpp` / `winmm_audio.cpp` / `mf_video.cpp` / `winmm_joystick.cpp`，`ctest:noop`）；
  version / shell32 / shlwapi / imagehlp / imm32 + COM / VARIANT 5 条归 M2；音频 / 视频归
  **M4**（`REQ-01M6K84KHPA1XF3B1HSHXME6W0`）；模态（仅关闭窗口二次确认）归 M2。
  ⇒ 164 条已逐组有主（归属清理单 `REQ-01M6K68YRBG3GB9ETF2P478VS1` 已收口）。
- MMX/SSE 与部分 x87 形态执行即**响亮失败**（可见缺口，不是静默 no-op）。
- A4 的指令数**依赖 `--exe` 的路径写法**（guest 命令行 = 该字符串）：实测 38,456 / 44,184 / 53,134 ⇒ 引用必须连路径一起给。

## 落点与真源
- 代码 `apps/emulator-lift/{src,tests,third_party}`；口径 `README.md`；偏差 `SCOPE.md`；计划 `docs/plan-m1-cpu-base.md`
- 交接备忘 `.tmp/lift-m1a-handoff.md` · `.tmp/lift-m1b-handoff.md`（**不入库**）
