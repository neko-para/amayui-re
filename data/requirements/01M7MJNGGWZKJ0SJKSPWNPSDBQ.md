# 🐞S2 SN0000→SC0000 的「渐变到黑」被一段 39.5e9 指令的计算整段吃掉 ⇒ 渐变塌成一帧（疑似空转/等线程）

- id: REQ-01M7MJNGGWZKJ0SJKSPWNPSDBQ
- type: bug
- status: open
- parent: REQ-01M7JHSD5MWTB9N8WJC2Y2T69M
- repro: docs/03-emulator/scripts/SC0000.md#第 4 轮读数
- severity: S2
- tags: [emulator-lift, sc0000, script-wait]

## 一句话

SN0000 最后一句之后那段「横向模糊 + 渐变到黑」**不是没画、也不是空转**：模糊是引擎
`BlurTexture`（`sub_4A62A0`）里的**逐像素 × 逐 tap 整数滑窗循环**（`0x4A6960–0x4A6A51`，83 条/tap；
⚠ 不是 radial 分支 `0x4A67E1–0x4A68F3`，那支有 `__ftol2_sse` 而本段零 ftol），每帧调一次。
**半径 `dot` 由脚本端点 + `timeGetTime` 线性插值**（`0x4B31C4..0x4B321D`；
`progress = ([ebx+0xB5A4] - v384[1] - v384[2]) / v384[3]`，`dot = (v384[20]-v384[16])*progress + v384[16]`；
`[ebx+0xB5A4]` 的唯一写点 `0x41279F` ← `0x412496 call timeGetTime`，主循环每帧一次）。
端点来自脚本 `i250`（`sub_425980` → `sub_4AF880`）：`SC0000.BIN label_00041a9c` = dot 0→200、3600 ms
（本段实测走的就是它；另一条 `label_00006180` = 0→0x64/3000 ms）。

⇒ **"每帧 2.7×" = 线性斜坡 × 帧时长正反馈**：`cost ≈ 921600 × (187 + 83 × (2·dot+1))`，
dot 反推 ≈ 2.5 / 9.8 / 29 / 81 / 132（建核/格收敛到常数 466，模型自洽）⇒ 一帧 0.6–20.4e9 条。
慢跑时每帧更长 ⇒ 下一帧 dot 跳得更大 ⇒ 更慢，直到 `progress ≥ 1` 取端点。**两跑指纹**：
S1/S5 逐位相同、S2–S4 慢跑大 6–10% ⇒ 证明是**时间驱动**而不是"按 step 给半径"。

## 与"渐变塌成一帧"的关系

每一帧的工作量（4–5 s 墙钟）都远超渐变一步的 `wait` 预算（2500 ms 墙钟；AOT 是墙钟档）⇒ `wait` 的追赶语义
让渐变剩余步骤在**同一帧**里全部通过 ⇒ 观感 = 一帧跳黑。**机制已定**；下一步是"为什么真机不需要这么久"。

## ★ 最强的下一步假设（未验证，别当结论）

我们 `GetDeviceCaps` 报 **PixelShaderVersion = 0**（`docs` 里登记为"偏离"）。若引擎按它选路（`Engine+46668`
就是 PixelShaderVersion 门），真机有 shader ⇒ 走 `D3DXCreateEffectFromResourceA`（FX/pixel shader）的**GPU 模糊**；
我们报 0 ⇒ 走上面这条**软件模糊** ⇒ 每帧 20e9 条 ⇒ 时间预算塌陷。
**判据（下一轮第一件事）**：把 caps 的 PS 版本改成 1.1/2.0/3.0 之一，跑同一段：
若 `D3DXCreateEffectFromResourceA` 真的出现（并停在那里）⇒ 分支确认；若没有出现 ⇒ 假设被否，回到"软件路径就是唯一路径"。
⛔ 不要在确认前实现 FX/shader（那是独立大单）。

## 读数（2026-11-17 / mac arm64 / `links/raw` / headless / 副本 sha `8332003f…`；报告 `.tmp/sc0000-fade/` + `.tmp/sc0000-blur/`）

- S1..S5 = 846,141,019 / 1,741,351,560 / 4,684,011,794 / 12,611,538,487 / 20,455,714,096 条（合计 39.49e9）。
- 段内 shim 每段 613–618 次（一次 BlurTexture 的 surface I/O：`LockRect`×2 / `UnlockRect`×2 / `HeapAlloc`×4 / `HeapFree`×1 / `UpdateSurface`）；
  `WaitForSingleObject`/`GetExitCodeThread` 全跑 0 ⇒ 不是等线程。计算体止点 6/6 次 = `0x00508B27`（`operator delete[]`）。
- ★ 取数坑：这段跑在 **S7 结构化单元**里 ⇒ 4–5 s 内 `--max-insns`/`--time-limit`/控制层闸门拿不到控制权；
  `--profile` 的 dispatch 采样**看不见这段**（`__ftol2_sse` 的采样点全在 43.98e9 之后）。

## 判据（收口时）

- 那一段有**多级**灰阶帧（逐帧 hash 序列单调变化 ≥ N 帧），而不是一帧跳黑；
- 且给出"每帧计算量为什么是 20e9"的答案（caps 分路 / 软件路径本身就是唯一路径，二选一，要有读数）。
- ⛔ 不许为了让画面像而改时钟/改预算（那是 guest 语义，要先立裁决）。

## 追加：caps 分路的验证与两处撤回（2026-11-17 探针）

`--until <EA>` 探针（`LIFT_D3D9_PS_VERSION=0x200` ⇒ 等级 2）：
`0x004A62A0`（`BlurTexture`）、`0x004B3792`（其唯一动静调用点）、`0x004B36EF`（软件模糊那一支的入口）**各 0 命中**；
`0x004A71B0`（effect draw 帮助函数）**命中（frame 1384）** ⇒ **软件模糊根本没进，走的是 FX 支**。

⇒ 两处撤回：
1. **"那段重活 = 软件模糊"只在 caps=0（等级 0）下成立**；等级 ≥2 时它 0 次调用。本单描述的"渐变塌成一帧"
   仍是**缺省档（caps=0）**的现象，修法仍是让 FX 路真正画出来（P2/P3）。
2. 判据"整跑 `insns` 从 108e9 下降"作废：两次 run 帧数不同（8,729 vs 10,150），整跑总量不可横比；正确口径是**同一帧窗口**。
3. 剩下的 ~1.1e11 在 **frames ≈8500–9800**（每帧 ≈1.3e8、profile 分散、含 `sub_4904D0` 文本查找）⇒ 是**转场之后的正文内容**，
   与本单无关；它的归因需要新工具（`REQ-01M7MW746DSB2G7NCBSJC6XV92`：`--profile` 按帧区间归因）。
