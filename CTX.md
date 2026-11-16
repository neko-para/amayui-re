# CTX · 交接（2026-11-16 晚 · mac arm64 → win32）

> 这是**待办 / 裁决清单**，不是口径文档。口径看 `AGENTS.md` §12 · `apps/emulator-lift/README.md` · `docs/03-emulator/`。
> 进度真源仍是 `pnpm tools requirements plan`（⛔ 不在这里抄数字）。
> **两个工作树**（2026-11-17 起）：`E:\Projects\amayui-re` = `main`（真相仓：需求单 / 本文件）·
> `E:\Projects\amayui-re-dev` = `dev`（代码 / 工具）。提交 / 合并 / 谁改哪条路径 / 缺资产怎么办 → `AGENTS.md` **§14**。

---

## A. 一句话现状

**控制层 M0 + M1 已落地并验完**：AOT 臂有 WS 端点（`--control-root`）+ 独立客户端 `tools/emu-ctl.mjs`
（`list/pause/resume/sample/screenshot/click/wait/quit/gc`），能把二进制**停住 → 采样脚本位置 → 投点击 → 继续**。
**解释器已退役**（target + 6 条 CLI 级 CTest 删除）。**配方前半段已改位置门**（TITLE / GAMESTART / SN0000）。
剩的是 **C1 三份长配方的尾巴**（区间已取证）。截图缺陷已修（冻结 present 那一帧，`REQ-01M7JAGE…` 已收口）；
**自动到加载存档屏**已人眼验过（`REQ-01M7GA4…` 已关）—— 但**第三条门**（`await global/local`，§B2）**仍 open**；
Windows 侧已验过一轮（见 `docs/03-emulator/control-plane.md` §6）。

上一轮读数（macos-arm64 / RelWithDebInfo / HEAD `b6afe77`）：`ctest` **99/99**（skip 3）· `pnpm test` **574** ·
`pnpm verify` **491** · `mutate --list` **108/108** · `requirements validate` **6/6** · `pgrep` 无残留。
Windows 侧（2026-11-17 / `windows-x64` = RelWithDebInfo）：`pnpm test` **574** · `pnpm verify` **491** ·
`mutate --list` **109/109** · `requirements validate` **6/6** · `ctest -R "control-snapshot|control-endpoint|present-window"` **4/4**；
⚠ **`ctest` 全量还没在 Windows 跑过一遍**（W1 待做，别把"抽跑 4 条"说成"全量绿"）。

---

## B. 需要你裁决（agent 不能替）

1. ~~**截图缺陷先修吗**~~ ✅ **已修并收口**（2026-11-17，`REQ-01M7JAGEKMYWWQPMWGV01Y1FNQ` = done）：
   根因是 `dumpLastFrame` 在暂停点读 guest backbuffer（只有 present 那一刻完整），现改为 present 时**冻结副本**；
   `source` 报 `guest-backbuffer-frozen`，判据 5 补了"BMP ≥50% 非黑"。实测 `paused_at=block` 由 **0.000% → 99.679%**。
2. **加载屏的第三条门现在做吗**（`REQ-01M7J97QY66KMP5SBPCTR0S3TP`）：
   加载屏与保存屏**共用 `SAVE.BIN` 且同区间**（已由读数钉死）⇒ 位置门**区分不了**，唯一区分量是 `global a9cb` 位。
   做它需要"按槽号读 global/local 的只读取址原语"（含解码），你已说"可以后续安排"。**不做就只能靠配方自己知道刚才点的是 Load 还是 Save**（不能当判据）。
3. **控制层探索手感 7 条挑哪几条做**（`REQ-01M7J624JNTFBY7WF52J50KQDC`）：
   优先级建议：`--until-pos`（按 pos 条件暂停）> `click` 施加回执 > 门失败 fail-fast > 缺省 orphan/root > 录制导出（M2）。其余两条（headless 截图黑、sample≠门的口径）已写进文档。
4. **AOT 覆盖缺口 6 条挑哪几条补**（`REQ-01M7J6D62QFRZC2454GV7CKJDJ`）：
   最像"该补"的是 **`--dump-frame` 开跑前建目录**（纯启动期判定，不需要 guest 代码）；其余（`--time-limit` 解析、hidden 窗口）成本更高。

---

## C. 需要动手的（按顺序；每条都有判据）

### C1 · 三份长配方的尾巴改位置门（**READY，区间已取证**）

可用区间（全部有读数，出处 `docs/03-emulator/scripts/*.md`）：

| 屏 | 区间 | 说明 |
|---|---|---|
| TITLE | `[0x0460,0x0520)` | 吃 click 的窗口（门 15/15） |
| GAMESTART | `[0x0664,0x0724)` | 门 15/15 |
| SN0000 页1 / 页2 / 页3 | `[0x46B8,0x470C)` · `[0x5038,0x504C)` · `[0x5990,0x59A4)` | 页 1 门 12/12；页 2/3 3/3 |
| **FELLOW** | ★`[0x1684,0x16A4)` | 4/4；**不要**用轮询体 `[0x1570,0x15A8)`（门命中但点击不换屏 ⇒ exit 5） |
| **SAVE**（含加载屏） | `[0x1104,0x11C4)` | 3/3；⚠ 两屏同区间，位置区分不了 |

要改的三份：`sn0000-quit.plan` · `sn0000-save.plan` · `sn0000-save-profile.plan` ——
把 `await script FELLOW` + 其后 `wait 10000/2000` 换成 `await position FELLOW 0x1684 0x16A4 timeout 60000`；
把 `await script SAVE` + `wait 2000` 换成 `await position SAVE 0x1104 0x11C4 timeout 30000`。

**必须保留的 `wait`**（真·时间语义，别删）：`click 650 320` 之后的 `wait 8000/15000`（确认框 `SBUNKI.BIN` 上没有稳定门）、
profiler 窗内时长、`se-eof` 的 `wait 12000`（SE 尾音）、`title-loaddata` 的 `wait 20000`（缺陷复现件，**该文件不要动**）。

判据：每份跑 3 次 ⇒ `steps/fired/pending/presents` 与 `region` 三元组逐位相同、exit 0；负对照（换成该屏永不执行的区间）⇒ exit 5 + 点名。
⛔ **5 条 `repro:` 点击行一个空格都不许改**（`1070 480` / `650 320` / `1186 373` / `1180 640` / `830 592`）。
⚠ 缺读数：`sn0000-quit` 的 `click 100 270` 与"回 TITLE"段 ⇒ 那一段**保持原样 + 加注释**，别硬改。

### C2 · 修截图缺陷（见 B1）

判据：headless 跑到 ≥30 帧后 `screenshot` ⇒ 非黑像素 > 50%（拿 `--dump-frame` 同帧对照）；window 同样；
两拍 `shasum` 相同、`hash_still_matches` 语义不变。临时规避见 §G-3。

### C3 · 第三条门 `await global/local`（见 B2）

判据（单据正文有）：`await global a9cb 1` 在加载路径成立、保存路径不成立；`local` 越界 ⇒ exit 2 点名；
门未满足 ⇒ 停在原步 + `timeout` ⇒ exit 5 点名；读数必须来自执行中的 guest（⛔ 不读宿主缓存）。
⚠ 实现落点应是**共享 runtime 的只读取址原语**（与 guest 同一份解码），⛔ 不在配方驱动器里另写一份。

### C4 · Windows 侧验证（**一条都没做过**）

| # | 验什么 | 怎么判 |
|---|---|---|
| W1 | `ctest` 全绿 | Windows 上应**真跑** mac 上 skip 的 3 条：`cli-aot-flags`（Windows 专有）、`gpu-raster`、`frame-dump-failure`（用 MSVC `io.h`） |
| W2 | vcpkg 冷装 | 本轮为控制层加 `boost-asio`(default-features:false)/`boost-beast`/`nlohmann-json`；⚠ **实测不是"全 header-only"**：`boost-beast` 把 asio 的默认特性重新打开 ⇒ 装了 `boost-context`/`boost-date-time`/`boost-container` 三个**编库**端口。Windows 要另付一次冷装 |
| W3 | 编译 | clang-cl + MSVC STL 编 `<boost/beast/websocket.hpp>`（`_WIN32_WINNT` / `/permissive-` / 告警等级）；`find_package(Boost CONFIG COMPONENTS asio beast)` 实测 mac 可用，Windows 待验 |
| W4 | **受限令牌/普通令牌下能否 listen `127.0.0.1`** | 端点起不来就是"控制层在 Windows 不可用"⇒ 记成事实，⛔ 不要改传输 |
| W5 | `ctl.<pid>.<nonce8>.json` 的 ACL / Low 完整性 | 沙箱起的模拟器写文件、DSH 宿主读并连 —— 两个方向各测一次 |
| W6 | `TerminateProcess` 之后 | `list` 应报 `alive:false, why:"refused"` 且文件残留；`emu-ctl gc` 只删残留、不删 `keep.txt` |
| W7 | `netsh int ipv4 show excludedportrange protocol=tcp` | 对照 `--control-port 0` 拿到的端口是否落在保留段 |
| W8 | 真 OS 事件洪峰（判据 13 的另一半） | Windows 上比 mac 容易（可直接 `SendInput`/`PostMessage`）：暂停期间点/拖窗口，resume 后**这一轮不许结束**、frame 继续 +1 |
| W9 | 单实例/多实例/授权 | `--control-port <已占用>` ⇒ exit 2 点名；两实例 ⇒ 2 alive、不带 `--session` ⇒ exit 4；错 token ⇒ 握手失败；带 `Origin` ⇒ 拒 |

### C5 · 旧账（仍是活单，别重复开）

- `REQ-01M7704NBZEMADBHRZCKPQ7W1S`：把**剩余 Windows 专有守卫搬 POSIX**（`title-entry-paths` / `movie-logo` / `frame-dump-failure` 等）—— 注意 `cli-time-limit` / `cli-dump-frame` 已随解释器退役，**不在其列**。
- `REQ-01M7G7GPBG9BK2360WSG6DBFNP`：AOT 音频管线（真出声）。
- `REQ-01M7GA4B8T9JXFDAYK46YHNH7K`：AOT 自动到「加载存档」屏 + 人手对照（本轮的 `SAVE.md` / `save-format.md` 已把环境与区间备好）。

---

## D. 已收口（⛔ 不要重做）

| 单 | 结论 | 凭据 |
|---|---|---|
| `REQ-01M7HX0CJ202MJ4PZXW80VFA0J`（M0） | WS 端点 + 端点发现 + 暂停/时钟冻结/截图/继续/idle+orphan 收口；14 条判据在 mac 上逐条取读数 | verify `tests/control_endpoint.cpp#endpoint-surface` |
| `REQ-01M7J2P5S27VX7WMRR3ZC9K2E8`（M1） | `click` 入队到立即队列、读点施加、**不推进配方 `next_`**、队列上界 64 | verify `tests/control_endpoint.cpp#click-enqueue-at-read-point` |
| `REQ-01M7HQ57XV66FZQA5ECGHWD71N`（解释器边界） | 6 条 CLI CTest + target + `src/main.cpp` 删除；13 条全部归位 | done_reason |
| `REQ-01M7HP2YRZG222E0NE2H6MW6NK`（🐞S2 退出码） | AOT 出口读 `shim.exit_code`（非零 ⇒ 2），映射搬到 `src/runtime/guest_exit.h` | verify `tests/guest_exit.cpp#lift-guest-exit` |
| 配方前缀 | TITLE / GAMESTART / SN0000 页 1–3 已改位置门；`se-eof`/`sn0000` 3/3 逐位相同 | `REQ-01M7HTXBADJC7R5CHKRJ1MMADG` |
| 脚本级分析落盘 | `docs/03-emulator/scripts/{TITLE,GAMESTART,SN0000,FELLOW,SAVE}.md` + `_recipe-map.md` | 每份都带命令与读数 |

★ 覆盖缺口**已登记**（⛔ 不是"没有缺口"）：`REQ-01M7J6D62QFRZC2454GV7CKJDJ`（AOT 上无端到端断言的 6 个面）。

---

## E. 我请你做的（只有人能做的）

1. **切机器前确认 commit**（本机 HEAD `b6afe77`，工作树干净；`out/` 不入库，Windows 要重新构建）。
2. **`--cursor-warp` 真机自证**（win32 上只证到状态行 `cursor-warp: on`）：`--present window --cursor-warp` 看宿主光标有没有被挪。
3. **人工对照一次「加载存档」**：用 fixtures 组出的 appdata（见 §G-4），点 Load Data，把收尾 `region` 三数发我（`REQ-01M7GA4B8T9JXFDAYK46YHNH7K`）。
4. **Windows 上重跑一次控制层全链**（`list → pause → sample → resume --for-ms 200 → screenshot → quit`）—— W4/W5/W6 都在这里。

---

## F. 恢复命令

```bash
cd apps/emulator-lift
cmake --preset windows-x64            # vcpkg 清单模式：改 vcpkg.json 就等于装依赖
cmake --build --preset windows-x64
ctest --test-dir out --output-on-failure      # 期望 99/99（Windows 上 skip 应更少）
cd ../.. && pnpm test && pnpm verify && node tools/requirements.mjs --validate
node tools/emu-ctl.mjs --describe      # 控制层命令表 / mutates / 两种 paused_total_ms 口径
```

---

## G. 坑（本轮踩过，别再踩）

1. **`sample` 与 `await position` 的评估点不同**：`sample` 停在**块边界**，位置门在**读点**判定 ⇒
   采样命中率**不能**推门命中率，反之亦然（实测：GAMESTART 门 7/7，而采样 `0/47`）。
2. **`sample`/门的判据要"停在该屏且能吃输入的那一段"**：TITLE/FELLOW/SAVE 都是"回调体吃 click、轮询体不吃"；
   把门下在轮询体上会得到"门命中、click 也施加了，但界面不动"（FELLOW 负对照就是这样 exit 5 的）。
3. **`screenshot` 现在取的是 present 时冻结的那一帧**（缺陷已修，2026-11-17）：与 `--dump-frame` 同一份像素；
   `hash_still_matches` 只回答"冻结有没有成"，**不当内容判据** ⇒ 内容判据看**非黑比例**（`node .tmp/check-black.mjs <bmp>`，`.mjs`）。
4. **存档环境**：只用 `corpus/fixtures/` 的原版样本（旧汉化存档是 GBK：**能看见能加载**，但"标题/当前章节"会乱码）；
   `--appdata` = `%AppData%` 根，实际落点 `<appdata>/Local/Eushully/天結いキャッスルマイスター/SAVE/`；
   ★ **带 fixtures 的 `SAVE.DAT`/`SAVE.BAK` ⇒ 第一屏是 110–119（全空）**；把它们拿掉 ⇒ 000–009（有项）。
5. **`resume --for-ms` 结束会重新暂停**，而"暂停 + 客户端全断"⇒ `--control-orphan-timeout`（缺省 **10 s**）**自己 quit**。
   连续采样要么一个进程里跑完，要么两次命令间隔 <10 s，要么显式 `--control-orphan-timeout 600`。
6. **驱动脚本必须用 `node`（`.mjs`）**：⛔ 不要 Python、不要 `bash + python3 -c`；`--json` 成功信封是
   `{op, ok:true, data:{<op>:…}}` ⇒ 路径是 `data.sample.script.pos`（不是 `sample.script.pos`）。
7. **到 TITLE 的自动化必须带 `--skip-logo`**；`stop @` 在 `--max-frames` 下无证明力；`lastHash` 不是指纹；
   能用的是收尾 `region` 三数（需 `--region-log`）—— ⚠ 但 `region` 对"点没点中"**没有区分力**（实测点了/没点同值）。
8. **⛔ 不跨机器比墙钟**（§12.6）；提交 / 合并按 `AGENTS.md` **§14**（可 `add` / `commit` / 撤暂存；已提交历史不许改写，合回 main 只 `merge`）；
   ⛔ 需求树只经 CLI 写，且**只在 `main` 工作树**写（dev 上不建 / 不改需求单）。
