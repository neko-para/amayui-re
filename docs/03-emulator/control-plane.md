# 控制层（AOT inspect 式端点 + `emu-ctl` 客户端）

> 回答："怎么在**不改配方**的前提下停下来看 AOT 臂跑到哪、截一张图、再继续"。规格真源是需求单
> `REQ-01M7HC1WJ9X0QCQ51Q9F01988W`（项目）/ `REQ-01M7HX0CJ202MJ4PZXW80VFA0J`（M0 判据）；
> 本文只写**口径与怎么用**，⛔ 不写进度与计数（§10）。

## 0. 一句话形状

```
主线程（SDL owner，执行循环）                     控制线程（一条，唯一碰 socket）
  Present 汇点 ─┐                                 io_context
  块边界汇点  ─┴─ if (ctl && ctl->pending())        ├─ accept / 握手（loopback+token+拒 Origin）
                    ctl->serve(ctx);  ← 暂停/取数/组 JSON   └─ 帧 ↔ 队列（mutex + cv）
```

- **两个汇点**：Present（`present.cpp`，sink 与 `--dump-frame` **之后**）· 块边界（`aot_run.cpp`，
  `syncCpuFromRegs(); cpu_.eip = ea;` 之后）。⛔ 没有第三个泵、⛔ 不在执行循环里加 pump。
- **热路径代价**：块边界一次 `atomic load`；`control_ == nullptr` 时只多一次指针比较。
- **归属**：只有主线程读 guest 内存 / 碰 `Presenter` / 改 `InputPlan`；控制线程只搬字节。
  `control_snapshot.h` 只放值（无 guest 指针），beast/nlohmann 只在 `control_ws.cpp` 一个 TU 里。

## 1. 怎么跑

```bash
# 端点（必须显式给 --control-root；不给 = 控制层完全不启动，逐字节不变）
apps/emulator-lift/out/emulator-lift-aot --present headless --skip-logo \
  --game-dir ../../links/install --control-root .tmp/ctl --control-port 0

# 客户端（独立进程；Node 内置 WebSocket，零新依赖）
node tools/emu-ctl.mjs list                      # 扫 root + 每个连一次，报 alive/why
node tools/emu-ctl.mjs sample --json             # 暂停（若未停）+ 快照
node tools/emu-ctl.mjs resume --for-ms 200       # guest 跑 200 ms 后重停
node tools/emu-ctl.mjs resume --for frames 1     # 帧 +1 后重停（帧不推进 ⇒ exit 3 timeout）
node tools/emu-ctl.mjs screenshot /tmp/a.bmp     # 自己先停住，再写"最后呈现那帧"
node tools/emu-ctl.mjs click 1070 480            # 暂停态入队一条点击（1280×720 参考坐标；下一个读点施加）
node tools/emu-ctl.mjs click 1070 480 right      # 右键
node tools/emu-ctl.mjs move 740 355              # 暂停态入队一条**纯 move**（⛔ 不按键；与 click 同一条队列）
node tools/emu-ctl.mjs global-get --kind int --index 0xb1b6 --json          # 读一格 script global（index 只收 0x hex）
node tools/emu-ctl.mjs global-set --kind int --index 0xb1b6 --value 0x00000010 --json   # 写一格（哑原语，见 §7）
node tools/emu-ctl.mjs gc --dry-run              # 只清残留端点文件（见 §4）
node tools/emu-ctl.mjs quit                      # 收口：bye → 摘要 → 退出
```

- 端点发现 = `<--control-root>/ctl.<pid>.<nonce8>.json`（含 url/port/nonce/token/session/exe_sha256/shape）。
  ⛔ 不用 `--appdata`：那是 **guest 映射**（游戏存档面），写进去会污染 guest 可见文件系统。
- 多实例：同一 root 多份文件、多个端口；`sample` 不给 `--session` 而 alive ≠ 1 ⇒ 退出码 **4** 并全部列出（⛔ 不猜）。
- ★ **驱动/批量一律用 `node`**（一次性脚本写成 `.mjs`，放 `.tmp/` 也可以）：⛔ 不要用 Python、也不要 `bash + python3 -c` 内联解析。
  本仓的工具语言只有 `.mjs`（`AGENTS.md` §3），Python 在 Windows 那台机器上不保证存在；而且内联解析极易把 `--json` 的**信封**读错
  —— 成功时是 `{op, ok:true, data:{<op>:…}}`，所以位置是 **`data.sample.script.pos`**，不是 `sample.script.pos`。
  （实测教训：有人写了 `d.get('sample')` 的 shell 循环，于是每一行都是空读数，且看不出错。）
- 退出码：`0` 成功（含 `script.available:false`）· `1` 端点回 `ok:false`（含 global 的 `id_out_of_range` /
  `type_mismatch` / `encode_failed`）· `2` 参数/协议错 · `3` 超时（端点仍活）·
  `4` 找不到 / **多个实例 —— 仅对"要挑一个实例"的 op**（`pause`·`resume`·`sample`·`screenshot`·`move`·`click`·
  `global-get`·`global-set`·`wait`·`quit`）；
  ⛔ **`list` 恒 0**：它只报告、不挑实例，多实例也 0（全列）。
- 退出码读数（2026-11-17 / win32 / `windows-x64` = RelWithDebInfo）：同一 `--control-root` 下两个 alive 实例 ⇒
  `node tools/emu-ctl.mjs list` **exit 0**（`count:2 alive:2`、两条都列出）；同目录 `node tools/emu-ctl.mjs sample`
  不给 `--session` ⇒ **exit 4** + `有 2 个 alive 端点，必须用 --session 选一个（⛔ 不猜）`。

## 2. 协议事实（客户端要按这个实现，⛔ 不要按设计稿）

- JSON 文本帧一问一答；握手后**服务端先发 `hello`**，客户端必须先读到它。`res` 回显 `nonce`（与 `hello.nonce` 不一致 ⇒ 客户端拒收）。
- 命令参数是**顶层字段**（`{"op":"resume","for_ms":200}`），不是 `args` 包一层。
- `hello.present` 在**顶层**，值 ∈ `{visible, hidden}`（取 `windowHidden()`/`describe()` 的同一判据；⛔ 不由 `--present` 推）。
- `bye` 两个字段各管一件事：`reason` = run 为什么结束（`stopped` / `window-closed` / …），`requested_by ∈ {client, idle, orphan}` = 谁请求的收口。
- `error.code` 是枚举：`bad_request` · `unknown_op` · `not_paused` · `busy` · `unavailable` · `timeout` · `io` · `internal`。
  `unavailable` = **数据没到**（例：一次 Present 都还没有）；`io` 只留给"有像素但写不出去"。
  `global-*` 失败时另给 **`error.reason`**（闭集 `id_out_of_range` / `type_mismatch` / `encode_failed`）——
  机器可读的是它，`code` 仍是 `bad_request`；`error.message` 只给人看（⛔ 客户端不许按它分支）。
- 未知字段忽略；未知 `op` ⇒ `unknown_op`（⛔ 不静默）。

## 3. `sample` 快照的字段与两个"反直觉"点

`paused` · `paused_at ∈ {block, present}` · `state_seq` · `insns` · `frame` · `last_hash` · `present` ·
`clock.guest_ms` · `clock.paused_total_ms` · `script{available, why, name, script_id, cur, sha, sha_why, pos, pos_dwords, header_len}`

1. **`state_seq` = 每次暂停状态变化 +1**（进入与离开都算）⇒ 断言只能写"单调增"，⛔ 不要写"恰好 +1"。
2. **`paused_total_ms` 同名不同口径**（已在 `--describe` 写明）：`sample` 里是**已结算**（不含当前这段未结束的冻结区间，
   否则同一暂停点两次采样不会逐字节相同）；`pause`/`resume` 的 `res` 里**含**当前区间。
3. **位置**：`pos = kScriptHeaderLen(0x3C) + (pc − base)`，绝对字节偏移、4 字节对齐，与 `dist/views/data/<NAME>.BIN.txt`
   的 `label_%08x` 同一坐标系。⚠ `header_len` 是**编译期常量**（头只在栈/文件里，guest 内存里没有落点）⇒ 它不是判据。
4. **`script.sha256` 只对"散装文件"成立**：脚本在 `DATA*.ALF` 容器里时 `sha_why == "unresolved"`
   （本安装的 `TITLE.BIN` 就是这一档）。⇒ 位置门的"字节绑定"目前**等价于"脚本名 + 安装 sha"**，⛔ 不要说成已实现。

## 4. 边界（都是实测出来的，不是保守估计）

| 边界 | 口径 |
|---|---|
| 只绑 loopback | `acceptor` 绑 `127.0.0.1`、**不开** `reuse_addr`；判据用 `local_endpoint()`，不靠 `lsof` |
| 未授权 | `?token=` 不匹配 ⇒ 401 不升级；带 `Origin` ⇒ 403（Node 内置客户端**不发** Origin，浏览器必发） |
| 单客户端 | 一次只有一条 WS 会话；并发 `list` 会排队（设计如此） |
| 僵尸 = `connect()` | 判死只认连接结果（`refused`/`timeout`），⛔ 不认 pid；`list` 只报告，删除只能靠 **`gc`**（三条同时满足：连接判死 + pid 不存活 + 文件名严格 `ctl.<pid>.<8hex>.json`） |
| 暂停/掉线 | idle（暂停且无未决命令，缺省 300 s）与 orphan（连接断但还暂停，缺省 10 s）都会让端点**自己收口** |
| 端点卡在不返回的 shim | 汇点不到 ⇒ 客户端 `timeout`（exit 3）、端点仍活；⛔ 不 kill、⛔ 不假装成功 |
| window 档 | **可用**（`--present window` + `--control-root`）。代价：暂停期间不泵事件 ⇒ OS 可能判"无响应"。**别停太久。** |
| 陈旧事件 | **arm 点在 `Control::serve()` 的 Resume 分支**：普通 `resume` 与 `resume --for-*`（两者都放行 guest）各 arm 一次；`pause` / `sample` / `screenshot` / `click` 与 idle/orphan 收口**都不 arm**（它们不产生"恢复首轮"）。arm = 把"恢复时刻"记到宿主 sink 上。暂停期间事件**排在 Win32 队列**里（不跑 `SDL_PumpEvents` ⇒ 根本没进 SDL 队列；下一次 pump 才转换进来）。判据 = `SDL_Event.common.timestamp` 与"恢复时刻"比，**三档**：① **输入 ✅**：早于恢复时刻 ⇒ 丢弃（不进 guest）；② **ESC ✅**：早于恢复时刻 ⇒ 当**宿主 stop**（`bye.reason:"window-closed"`）—— 判得动是因为 Win32 后端用 `WIN_GetEventTimestamp()` 把戳回溯到投递时刻；③ **关窗 ❌ 不可判**：`WM_CLOSE` → `SDL_SendWindowEvent(…, SDL_EVENT_WINDOW_CLOSE_REQUESTED, 0, 0)` 把 `timestamp` 置 0、`SDL_PushEvent` 再填**当下**（pump 时刻，必晚于恢复时刻）⇒ `classifyStaleEvent` 恒 `Keep` ⇒ 暂停期间的关窗**仍是"投 `WM_CLOSE` 进 guest"**，见 `REQ-01M7JNVD59RD49RSKD9N2S9TXW` |
| 截图来源 | `guest-backbuffer-frozen`：`present()` 当场冻结的 guest 后缓冲**像素副本**（只在 `--control-root` 时拷；1280×720×32bpp ≈ 3.7 MB/帧）。⛔ 不是窗口回读（不叠层 / 不带 letterbox / 不带光标），⛔ 也不是"暂停那一刻的现场后缓冲"——现场这时已被下一帧的 `Clear` / 回读改写（实测：`paused_at=block` 时现场是清除色、`present` 时才是画面） |
| `hash_still_matches` | 语义 = "写出的这份字节仍等于 presenter 记录的那一帧"。**冻结路径下恒 true** ⇒ 它只回答"**冻结有没有生效**"（把冻结去掉会变 false —— 守卫就是靠这条红的）；⛔ **不能当"图对不对"的判据**：判内容要判 BMP 非全黑，或与同帧 `--dump-frame` 逐字节对照 |
| 读不到的两处（残余） | 冻结**分配失败**时退回写现场（极端情况仍可能写出一张全黑图；只 warn 一次 + 计数）；计数与最近一次冻结耗时在 AOT 摘要的 `present :` 行（`freezeFailures=` / `freezeNs=`）。⛔ 没有协议级 `unavailable`（2026-11-16 收窄裁决：不为它扩协议） |
| 病史 | `REQ-01M7JAGEKMYWWQPMWGV01Y1FNQ`（🐞S2，2026-11-16 收口）：旧实现只留"最后一张"的**描述符**、暂停时才读像素 ⇒ 读到的不是那一帧（`paused_at=block` 时全黑 0.000%，`present` 时 99.679%） |

★ **陈旧事件闸门要两层判据，⛔ 只留第二层就等于让"接线掉了"静默通过**（2026-11-17 / win32 / `windows-x64` = RelWithDebInfo）：

1. **接线**（`lift-control-endpoint` 的 `control-resume-arms-host-hook`）：真协议 + 真 `serve()`，数 `controlOnResume()` 被调几次 ——
   `pause`→0 · `resume`→1 · 运行态 `resume`（`not_paused`）→1 · `sample`→1 · `resume` + `"for_ms":30`→2 · 再 `resume`→3。
   `ctest --test-dir out -R "control|present"` ⇒ **5/5 通过**；把 `Control::serve()` 里那一行拆掉 ⇒ **`lift-control-endpoint` 失败**
   （`506 checks, 5 failures`，读数打印 `controlOnResume 被调 0 次（期望 3）`）。
   `node tools/mutate-check.mjs --match controlOnResume` ⇒ 守卫红（CTest 退出码 8）。
2. **映射**（`present-window` 的 `control-resume-stale-events`）：陈旧 OS 输入丢弃 / 陈旧关窗当宿主 stop。
   ⚠ 其中的**关窗**只在"带陈旧戳"时成立：生产路径上关窗拿不到陈旧戳（上表 ❌ 档）⇒ ⛔ 不能读成"暂停期间的关窗已被挡住"。
   ⚠ 它**自己**调 `win.noteControlResume()` 造恢复时刻 ⇒ 只能证明纯映射 + sink 一侧。
   病史（`REQ-01M7JFDZK1SZW72SBAW6BC3DRT`，🐞S2，2026-11-17 收口）：这一层一直是绿的，而生产路径上
   `controlOnResume()` **零调用** ⇒ `stale_input_before_ns` 恒 0 ⇒ `classifyStaleEvent` 恒 `Keep` ——
   暂停 10 s 里的点击 / 拖动 / 关窗会被 resume 首轮当"刚发生"灌进 guest。

## 5. 怎么查

```bash
node tools/emu-ctl.mjs --describe          # 命令表 / mutates / 两种 paused_total_ms 口径 / wait 的同名陷阱
node tools/emu-ctl.mjs list --json         # 现有哪些端点、谁还活着、为什么死
node tools/emu-ctl.mjs sample --json       # 当前 <script>:<pos>
pnpm tools requirements show REQ-01M7HX0CJ202MJ4PZXW80VFA0J   # 判据 14 条（含每条"怎么红"）
```

★ **`click` 是"入队"不是"施加"**：它只接受**暂停态**（否则 `not_paused`），入队后在**下一个 guest 读点**生效；
  施加走的是**配方 click 的同一条**通道（`cursor_map::referenceToClientPixel` 一次 + `input::*` + 同一份 down→待抬起记账），
  ⛔ 没有第二套坐标映射。它**不推进**配方的步进指针 ⇒ 配方里未到期的门不会被跳过（判据 2）。
  参考坐标恒 `1280×720`，与窗口缩放无关；每次命令的上界 = 64 条（超界 `bad_request`，⛔ 不静默丢）。

★ **同名陷阱**：控制层的 `wait <ms>` = "让 guest 跑 N ms 后重停"（= `resume --for-ms`）；
`--input-plan` 里的 `wait` 是**配方 DSL** 的一条 deadline。两者不是一回事。

## 6. 未做

`key`（暂不做）· `resume --until-ea` · 录制导出配方（M2）· "点击是否真的被施加"没有回执字段（要 e2e 断言只能看配方摘要的 `fired`）·
DSH 插件薄包装（它只 exec CLI，⛔ 不自己实现协议）。

★ **Windows 已验（2026-11-16，win32）**：`--control-root` 能 listen `127.0.0.1`，`list/pause/sample/screenshot/resume/click/quit`
全通（`control : ws://127.0.0.1:<port>` + `alive:1`）；`--control-port 0` 取到的端口不在保留段（实测 `netsh` 排除段 50000–50059，取到 62556）。
★ **已测：`TerminateProcess` 后的残留识别**（2026-11-17 / win32 / `windows-x64` = RelWithDebInfo）：
`taskkill /F /PID <pid>` 杀一个实例后 `ctl.<pid>.<nonce8>.json` **仍残留**（实测 403 B）；同 root
`node tools/emu-ctl.mjs list` **exit 0**、该条 `alive:false, why:"refused"`（TCP ECONNREFUSED）、活条目 `why:"ok"`；
`gc --dry-run` 与 `gc` 的删除集合**一致**（`remove ctl.<pid>.<nonce8>.json … why:"dead"` / `keep ctl.<alive>.json … why:"alive"`），
`gc` 后只剩活条目、**exit 0**（读数：`共 2 条 · alive 1 · dead 1` → gc `removed 1 · kept 1` → `共 1 条 · alive 1 · dead 0`）。
Low 完整性标签下 `ctl.*.json` 的可读性按既有裁决 drop，⛔ 不复活。

## 7. typed global：哑原语 + **调用方**的配方

`global-get` / `global-set` 读写 **script global**（引擎的 global 池，落在 guest 内存里）。
它们是**哑原语**：⛔ 不猜类型（`kind` 必须显式给）· ⛔ 不挑时机（不等 TITLE / 不等配置加载）· ⛔ 不重试 ·
⛔ 不写后校验 · ⛔ 不为任何具体 global 或选项开特例。**"什么时候写、写哪个"是调用方的知识** —— 就在本节。

- 请求：顶层 `{op, kind, index, value}`；`kind ∈ {int, uint, float}`（`string` 第二期未实现 ⇒ `type_mismatch`）。
- **`index` 与 int/uint 的 `value` 只收 `0x` 十六进制**（脚本里这些字段就是 hex raw；支持十进制只会制造
  "`0xB1B6` 到底是 hex 还是十进制"的歧义）。▸ **唯一例外**：float 的 `value` 是十进制浮点字面量
  （它不是脚本字段编码，而是 IEEE-754 裸 32 位的语义值）。
- 答复 `data.global{kind, index, value, raw}`：`index` / `raw` / int-uint 的 `value` 一律 `0x%08X`（便于与脚本逐位对照）。
- `index` 的语义 = **raw 池下标**（脚本 token `(global-int b1b6)` 里的 `b1b6`）；⛔ 没有"名 → id"映射。
- 读写的实现是一层 **AGE 引擎层**（`src/runtime/script_globals.{h,cpp}`）：寻址 `rd32(Engine+0x5D800) + 4*id`、
  每次现读（`0x6d` 会把 `+0/+4` 两个槽整体对调 ⇒ ⛔ 缓存基址）、int/uint 槽里是 `ENC(value, key)`、float 是裸 32 位。
  ★ 它是引擎逻辑（AGE 专属、可替换），⛔ 不是"跨引擎通用能力"。
- 写的是**一个 guest 数据槽**，不是执行流：⛔ 不写 `cpu_` / `regs_`；命令在主线程**块边界**执行（与 Present 闸门同一入口），
  下一块自然从内存重新加载 ⇒ 写完立刻对引擎可见。运行态也照做（处理完立即放行），暂停态则留在暂停态。

### 7.1 已知配方：关掉 SO033A 导航覆盖图

`SO033A`（"ナビゲーション画像"，`SYS4INI.BIN` 文件表 index `0x5126`）由 `FIELD.BIN:label_000416fc` 的
32 ms 回调画出：`b224 == 0 && 2fb3 == 0 → 2fb7++`，当 `2fb7 ≥ 100`（≈3.2 s 静止）**且** `(b1b6 & 0x10) == 0` 时
画一张全屏覆盖图。⇒ 置上 **bit4** 就关掉它。

```bash
node tools/emu-ctl.mjs global-get --kind int --index 0xb1b6 --json   # 读 data.global.value（形如 "0x00000000"）
# 把那个值按位或 0x10，写成 8 位十六进制（例：0x00000000 ⇒ 0x00000010）
node tools/emu-ctl.mjs global-set --kind int --index 0xb1b6 --value 0x<V|0x10 的 8 位 hex> --json
node tools/emu-ctl.mjs global-get --kind int --index 0xb1b6 --json   # 读回确认
```

★ 用 `.mjs` 脚本 / 手工读那个 JSON，⛔ 不要用 shell 内联解析（信封是 `{op, ok, data:{global:…}}`，
按 `sample` 那次的教训很容易读成空值却看不出错）。

- ⚠ **极性是反的**：UI 上那个选项的存位与显示相反（`CONFIG1.BIN` 的 `eq 57bb, 7f4, 0`）⇒ **关 = `| 0x10`**，
  ⛔ 不是 `& ~0x10`。（写成 `& ~0x10` 是**打开**它。）
- ⚠ **写太早会被覆盖**：`INITCONFIG1` / `LOADCONFIG` 在 TITLE 之前整词写这个 global ⇒ 必须在**进 TITLE 之后**再写。
- 验据（判据不是"看起来没了"）：写前 / 写后各在**同一屏静止 ≥ 3.2 s** 截一张，比较覆盖图是否出现；再做**反向对照**
  （改回 `& ~0x10` 应重现），否则无法排除"只是那一次没到 100 拍"。
- ⛔ 这不是 `--skip-logo` 那一类"工具替你决定"的开关：命令只写一个槽，**什么时候写**由你按上面的知识决定。
- ★ **实测（2026-11-19 / win32 / main `234d887`，SAVE05 场景内）**：`global-get` 得 `value=0x00000000 raw=0x71C7CBC6`
  ⇒ `global-set --value 0x00000010` ⇒ 读回 `value=0x00000010 raw=0x71C3CBC6`。三条都成立：寻址对、
  **写的是 ENC 后的位模式**（raw 不是明文 `0x10`）、下一拍就读到新值。同一轮 A/B 还证明了**这块覆盖图
  与 SAVE05 的黑矩形无关**（置位前后黑框都在；黑框的成因见需求单
  `REQ-01M7R1ABAP86QVMNBZ5S4EDKD5`，证据 `.tmp/save05-black/report.md` §4）。

### 7.2 副作用与边界

- 写的是**运行中**的 guest 槽：游戏之后的 `save-int` 可能把它写进配置库（下次启动 `LOADCONFIG` 会读回）
  ⇒ 不想污染存档就用**私有 `--appdata`**。
- `index` 越界 / 引擎未发布（boot 早期 / 退出后）/ 池基址为 0 / 槽未提交 ⇒ `id_out_of_range`；
  `kind=string` 或写入值的 kind 与 `kind` 不符 ⇒ `type_mismatch`；int 的编码 key 读不到 ⇒ `encode_failed`。
- `type_mismatch` **只在能判定时报**：槽里没有运行期类型标签 ⇒ 判不了就**放过**（⛔ 不要指望它当类型检查器）。
- ⛔ 不提供"读-改-写一个位"的复合命令（那会变成"猜意图"）；要改位就自己发两条命令，并接受两条之间值可能变。

★ 守卫与读数：`ctest --test-dir apps/emulator-lift/out -R '^lift-control-globals$'`（pure；夹具 = 假 Engine /
假池基址 / 假 key，⛔ 不读安装）；变异 `node tools/mutate-check.mjs --match script-globals-int-enc-plaintext`
与 `--match control-global-hex-only`（两者都必须**当场红**）。协议面看 `node tools/emu-ctl.mjs --describe`。
