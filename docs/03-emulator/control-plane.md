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
- 退出码：`0` 成功（含 `script.available:false`）· `1` 端点回 `ok:false` · `2` 参数/协议错 · `3` 超时（端点仍活）· `4` 找不到/多个实例。

## 2. 协议事实（客户端要按这个实现，⛔ 不要按设计稿）

- JSON 文本帧一问一答；握手后**服务端先发 `hello`**，客户端必须先读到它。`res` 回显 `nonce`（与 `hello.nonce` 不一致 ⇒ 客户端拒收）。
- 命令参数是**顶层字段**（`{"op":"resume","for_ms":200}`），不是 `args` 包一层。
- `hello.present` 在**顶层**，值 ∈ `{visible, hidden}`（取 `windowHidden()`/`describe()` 的同一判据；⛔ 不由 `--present` 推）。
- `bye` 两个字段各管一件事：`reason` = run 为什么结束（`stopped` / `window-closed` / …），`requested_by ∈ {client, idle, orphan}` = 谁请求的收口。
- `error.code` 是枚举：`bad_request` · `unknown_op` · `not_paused` · `busy` · `unavailable` · `timeout` · `io` · `internal`。
  `unavailable` = **数据没到**（例：一次 Present 都还没有）；`io` 只留给"有像素但写不出去"。
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
| 陈旧事件 | resume 时丢弃 `event.timestamp` 早于恢复时刻的 OS 输入；暂停态的关窗/ESC 当**宿主 stop**（`bye.reason:"window-closed"`）⇒ 否则 resume 首轮会把这一轮结束掉 |
| 截图来源 | `guest-backbuffer-frozen`：`present()` 当场冻结的 guest 后缓冲**像素副本**（只在 `--control-root` 时拷；1280×720×32bpp ≈ 3.7 MB/帧）。⛔ 不是窗口回读（不叠层 / 不带 letterbox / 不带光标），⛔ 也不是"暂停那一刻的现场后缓冲"——现场这时已被下一帧的 `Clear` / 回读改写（实测：`paused_at=block` 时现场是清除色、`present` 时才是画面） |
| `hash_still_matches` | 语义 = "写出的这份字节仍等于 presenter 记录的那一帧"。**冻结路径下恒 true** ⇒ 它只回答"**冻结有没有生效**"（把冻结去掉会变 false —— 守卫就是靠这条红的）；⛔ **不能当"图对不对"的判据**：判内容要判 BMP 非全黑，或与同帧 `--dump-frame` 逐字节对照 |
| 读不到的两处（残余） | 冻结**分配失败**时退回写现场（极端情况仍可能写出一张全黑图；只 warn 一次 + 计数）；计数与最近一次冻结耗时在 AOT 摘要的 `present :` 行（`freezeFailures=` / `freezeNs=`）。⛔ 没有协议级 `unavailable`（2026-11-16 收窄裁决：不为它扩协议） |
| 病史 | `REQ-01M7JAGEKMYWWQPMWGV01Y1FNQ`（🐞S2，2026-11-16 收口）：旧实现只留"最后一张"的**描述符**、暂停时才读像素 ⇒ 读到的不是那一帧（`paused_at=block` 时全黑 0.000%，`present` 时 99.679%） |

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
⚠ 仍未测：`TerminateProcess` 后的残留识别、Low 完整性标签下 `ctl.*.json` 的可读性（用户裁决"后续不考虑沙箱" ⇒ W5 已 drop）。
