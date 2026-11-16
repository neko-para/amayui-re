# GAMESTART.BIN

## 身份

- 清单：`dist/views/data/GAMESTART.BIN.txt`（1359 行；头 `signature = SYS4450`）。
  该名字在 `dist/views/data/` 里**只有一份**（没有 `$1$` / `$2$` / `$3$` 副本）。
- 地址规则核对：按本目录 `README.md` 的"行 → 地址"规则在这份清单上复算 **204 个 `label`，0 个对不上**
  （同一支脚本在 5 份清单上 1491 个 label 全对）。本文所有地址都是这条规则算出来的**清单地址**。
- `script` 身份名怎么来的：`await script` / `await position` 的 `<SCRIPT>` 是**运行期**名字匹配
  （ASCII 大小写不敏感、两侧剥 `.bin`），判定在 `apps/emulator-lift/src/runtime/recipe.cpp` 的 `scriptNameMatches`。
- 被装载的机制：配方在 TITLE 点 `1186 373` 之后用 `await script GAMESTART` 进来
  （`scripts/recipes/se-eof.plan` / `sn0000.plan` 都是这条路径）。引擎侧"谁装载 GAMESTART"不在本文范围内。
- 结构（本文三条区间都靠它）：
  - 主**轮询体** `0x05D8`（清单行 84–91：`get-input-type` + 2×`eq/jcc` + `sleep 1` + `jmp 0x05D8`）。
  - 鼠标回调登记 `mouse-callback 32 label_00000664`（行 189）⇒ **回调体** `0x0664`（行 93–96）
    = `call 0x50b0` / `call 0x0680` / `ret`。
  - **读输入子过程** `0x0680`（行 98–108）：`i2fc`@`0x069C`（行 100）、`read-mouse-pos`@`0x0704`（行 106）、
    `read-mouse-button`@`0x0718`（行 107）；下一 label `0x0724`（行 109）。
  - `0x50B0` = `i20c` + `ret`（行 1302–1304）。全清单**只有一处** `call label_000050b0`（行 94 = 回调体的第一句）
    ⇒ "采样落在 `0x50B0`"就是"**回调体正在执行**"的机制证据（不是"我看到那屏了"）。

## 位置区间

读数口径：判据只有 `{script, pos}` 两个字段（`sample --json` 的 `data.sample.script`）。
`pos` = `0x3C + (pc − body_base)`，与清单 `label_%08x` 同一坐标系、4 字节对齐。

★ **两个评估点不是一回事**（本文最重要的口径，见 `## 待核` 4）：`node tools/emu-ctl.mjs sample` 的 `pos`
取在**暂停点**（块边界 / Present）；`await position` 在**读点**上判。⇒ 同一个区间可以"**门 7/7 命中、采样 0/47**"。
**判"这个区间能不能当门"只能用 `--input-plan` 跑一次**，⛔ 不能用采样命中率否定一个门；反之，采样命中率高
也不保证能当点击门（`SN0000.md` 的粗区间就是这种反例）。

| 区间 `[lo,hi)` | 是什么 | 复现命令 | 读数日期 |
|---|---|---|---|
| `[0x0664,0x0724)` | **回调体的输入读段** = 吃 click 的窗口（回调入口 + `call 0x0680` 整条读输入链） | 见下 ①（门） | 2026-11-16：门 **7/7** 命中且点击被消费（E1/E2/E4/E6/E7/E8/E9 七次 run）；采样 **0/47** |
| `[0x05D8,0x0664)` | 轮询体（`get-input-type` + `sleep 1` + `jmp`）：**采样最常落在这一段**，但它不是输入读段 | 见下 ②（采样） | 2026-11-16：采样 **24/47**（`0x5D8/0x5DC/0x5F8/0x614/0x630/0x64C/0x658`） |
| `[0x50B0,0x50B4)` | 回调体第一句 `call 0x50b0` 的 `i20c` 块（每帧的主要墙钟落点）：**门 + 点击都成立** | 见下 ③（门） | 2026-11-16：采样 **23/47**；门 **1/1**（E3：点击被消费 ⇒ 进 SN0000） |

★ **配方侧命中率**（2026-11-16，本轮把 `scripts/recipes/*.plan` 的前缀改成位置门时取的读数）：
`[0x0664,0x0724)` 在 `se-eof` / `sn0000` / `sn0000-save` / `sn0000-save-profile` / `sn0000-quit`
**各 3 次**（同一台机器、`--present headless --skip-logo --game-dir ../../links/install`、
每份一个新 `--appdata`）⇒ **15/15 命中**，紧跟的 `click 830 592` 都被施加（`fired` 计的是 click 数）。
⇒ 它当"GAMESTART 的点击门"可用；`[0x50B0,0x50B4)` 仍然只是**备选**（本文件"待核"2：1 次 run 不够）。
读数表在 `.tmp/reql/REQ-01M7HTXBADJC7R5CHKRJ1MMADG/report.md`。

```bash
# ---- 共用：起一个实例（仓库根；host 墙钟 120 s，暂停时间不计入 --time-limit）----
apps/emulator-lift/out/emulator-lift-aot --present headless --skip-logo --game-dir links/install \
  --appdata "$(mktemp -d)" --control-root .tmp/p1/ctl --control-port 0 \
  --control-orphan-timeout 600 --time-limit 120 --max-frames 100000 &
# ★ 客户端每次调用都是独立进程：两条命令之间若超过 --control-orphan-timeout（缺省 10 s），端点会自己收口

# ---- ① 门：区间 [0x0664,0x0724) 命中后点击被消费（本文件 E9；期望 exit 0 / pending=0）----
cat > /tmp/gs-window.plan <<'PLAN'
await script TITLE timeout 30000
await position TITLE 0x0460 0x0520 timeout 30000
click 1186 373
await script GAMESTART timeout 30000
await position GAMESTART 0x0664 0x0724 timeout 30000
click 830 592
await script SN0000 timeout 15000
quit
PLAN
apps/emulator-lift/out/emulator-lift-aot --present headless --skip-logo --game-dir links/install \
  --appdata "$(mktemp -d)" --max-frames 100000 --time-limit 40 --input-plan /tmp/gs-window.plan
# 读数：exit 0 · input-plan: steps=8 fired=2 pending=0 presents=308 · stop: stopped @ … — input-plan: quit

# ---- ② 采样：GAMESTART 屏上落在哪一段（屏上不点任何东西）----
#   先把 TITLE 点过去：**暂停在 0x3844（i20c）时入队**（轮询体里入队的点击不生效，见"输入点"）
node tools/emu-ctl.mjs sample --json --control-root .tmp/p1/ctl    # ⇒ TITLE.BIN:0x3844
node tools/emu-ctl.mjs click 1186 373 --control-root .tmp/p1/ctl
node tools/emu-ctl.mjs resume --for-ms 3000 --control-root .tmp/p1/ctl
node tools/emu-ctl.mjs sample --json --control-root .tmp/p1/ctl    # ⇒ GAMESTART.BIN:0x…（data.sample.script）
node tools/emu-ctl.mjs resume --for-ms 200 --control-root .tmp/p1/ctl
# 重复 ≥3 次；47 次采样的分布见上表

# ---- ③ 门：i20c 块 [0x50B0,0x50B4) 命中后点击被消费（E3）----
#   把 ① 里的 `await position GAMESTART 0x0664 0x0724` 换成 `await position GAMESTART 0x50B0 0x50B4`
#   读数：input-plan: steps=8 fired=2 pending=0 · 进 SN0000

# 收尸
node tools/emu-ctl.mjs quit --control-root .tmp/p1/ctl
```

- 读数的执行源：`apps/emulator-lift/out/emulator-lift-aot`，`sha256 = 0fcb1416…83abbad`
  （2026-11-16 20:31 构建，macos-arm64 RelWithDebInfo；门那七次 run 全用它）。
  采样跨了两个相邻构建（20:22 / 20:31），在 20:31 那份上**重跑过一遍**（TITLE 10 次、GAMESTART 4 次），
  落点集合与上表一致。

## 输入点

- **吃 click**：`[0x0664,0x0724)`。实测路径：`await position GAMESTART 0x0664 0x0724` ⇒ `click 830 592`
  ⇒ 脚本变 `SN0000.BIN`（门 7/7；`se-eof.plan` / `sn0000.plan` 用的就是这个坐标）。
- **吃 click（另一个可用窗口）**：`[0x50B0,0x50B4)`。`i20c` 是回调体的第一句调用，点击在这里入队后
  **下一个读点就落在读输入链里**。读数：门 1/1（E3）+ 控制层直接入队 **2/2**
  （两次都是"暂停在 `0x50B0` ⇒ `click 830 592` ⇒ `resume --for-ms 4000` ⇒ `script == SN0000.BIN`"）。
- **采样最常落在**：`[0x05D8,0x0664)`（24/47）与 `[0x50B0,0x50B4)`（23/47）。前者是轮询体、后者是 i20c；
  两者都是"**在屏上**"的读数，但**只有后者**被实测证明能消费点击。
- ⚠ **轮询体"点不动"这条在 GAMESTART 上没有单独取证**：TITLE 侧有过 1 次直接读数
  （暂停在 `0x454`＝轮询体末尾 ⇒ `click 1186 373` ⇒ 3000 ms 后仍是 `TITLE.BIN`，与 `TITLE.md` 的 5/5 基线一致），
  GAMESTART 只是同形 ⇒ 见 `## 待核` 1。
- ★ **`click` 入队 ≠ 施加**（控制层口径）：入队只接受暂停态，**施加在下一次 guest 读点**。⇒ "在哪一点暂停"
  决定"点击落在哪一段"：暂停在轮询体里入队，下一个读点仍在轮询体（大概率白点）；暂停在 `0x50B0` 入队，
  下一个读点就在读输入链里（生效）。

## 待核

1. **轮询体 `[0x05D8,0x0664)` 点不动**：只有 TITLE 的 1 次直接读数（`0x454` 入队 → 无效）+ 同形推断，
   缺本屏自己的读数（一次 `await position GAMESTART 0x05D8 0x0664` + `click 830 592` 的 run 就能裁决）。
2. **`[0x50B0,0x50B4)` 只跑了 1 次门**：采样命中率高（23/47）使它看起来比 `[0x0664,0x0724)` 好用，
   但 1 次 run 不足以当推荐门 ⇒ 要 ≥3 次 run 且都 `pending=0` 才升格。
3. **`i20c` 的语义**：读数只说明"采样停点 + 每 200 ms 帧数 +12–13"在这一块上，⛔ 不能由此写成"它等于等一帧"
   （§6：语义结论要么锚 EA 的可再校验观察，要么另开单取证）。
4. **"门 7/7 命中 vs 采样 0/47"的机制**：两个评估点（读点 / 暂停点）不同这件事是**口径**，有协议出处
   （`input-plan.md` §2.8：本臂在 guest **读点**推进；`control-plane.md` §3：`sample` 取在暂停点）；
   但"为什么读点总在读输入链上、暂停点总在 i20c 上"没有机制取证 ⇒ 只当读数用。
5. **`script_sha`**：位置谓词今天没有字节绑定参数（同 `TITLE.md` 待核 4），⇒ "`pos` 门"等价于"脚本名 + 安装 sha"。
6. **「下一步」的命中区**：`830 592` 只按配方沿用（7/7 生效），`i12e` 命中数组与菜单语义的对应关系**未取证**
   （同 `TITLE.md` 待核 2 的性质）。
