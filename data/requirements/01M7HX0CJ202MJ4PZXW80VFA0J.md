# M0 · AOT 控制层端点（WS · 只读控制：停 / 采样脚本位置 / 截图 / 继续 / 收口）

- id: REQ-01M7HX0CJ202MJ4PZXW80VFA0J
- type: req
- status: done
- parent: REQ-01M7HC1WJ9X0QCQ51Q9F01988W
- verify: apps/emulator-lift/tests/control_endpoint.cpp#endpoint-surface
- tags: [emulator-lift, control-plane]

## 一句话

AOT 臂落 **M0 控制层**：`--control-root <dir>` 起一个只绑 loopback 的 WS 端点，`emu-ctl` 能**停住 / 采样脚本位置 / 截图 / 继续 / 收口**；不带 `--control-*` 时逐字节不变。

## 为什么

探索配方不该是"跑到底 → 看末帧 → 改配方 → 重跑"。M0 只做**只读**那一半（停、看、继续）；动态投递（`click`/`key`）与录制导出是 M1/M2。

## 判据（每条都要能红；⛔ 不许为绿改弱）

1. **惰性**：`--control-root X --max-frames 60`（不发命令）与不加 `--control-*` 的语义指纹逐位相同（`exit`/`frames`/`stop @`/arena 三元组/`table misses`）。
2. **握手与身份**：`list` 1 alive；`hello.pid`/`nonce`/`port` == `ctl.*.json` 的；`schema=="emu-ctl/2"`；**顶层** `hello.present ∈ {visible,hidden}`（取 `describe()`）；每条 `res.nonce` 回显一致。
3a. **位置在 TITLE 输入循环**：`sample` ⇒ `script=="TITLE.BIN"`、`pos ∈ [0x039C,0x0460)`、`pos%4==0`。⚠ `header_len==0x3C` 是编译期常量 ⇒ 恒真，不作判据字段。
3b. **两次采样逐位相同**（不 resume）：`pos`/`cur`/`script_sha` 逐位相同、`state_seq` 不变。
3c. **`script_sha` 覆盖脚本体**：`.tmp` scratch 安装树对 TITLE.BIN `insert-after` ⇒ sha 变；负对照 = 垃圾裸件 ⇒ 引擎自报文件名。
3d. **读不到是枚举**：boot 前 ⇒ `available:false` + `why` ∈ 既有 8 个字面量，退出码 0。
4. **步进可重复**：3 次 `resume --for-ms 200` ⇒ **`state_seq` 单调增（每个暂停边界至少 +1）**、`insns` 前进；帧会推进的屏 `resume --for frames 1` ⇒ frame +1；帧不推进 ⇒ `timeout`（exit 3）**且谓词已清**（之后 `resume` = `not_paused`）。
5. **截图是最后呈现的那一帧**（2026-11-16 二次改字面）：`paused_at ∈ {present,block}` + `source:"guest-backbuffer-frozen"`
   + 两拍 `shasum` 相同 + **BMP 非全黑（≥50% 非黑）**。⛔ 不强制 `present`（那会让从不 Present 的屏永远等不到）；
   ⛔ 不再把 `hash_still_matches` 当"内容对不对"的判据 —— 它在冻结路径下**恒 true**，只回答"冻结有没有生效"
   （见缺陷 `REQ-01M7JAGEKMYWWQPMWGV01Y1FNQ`；删掉冻结它会变 false，守卫因此能红）。
6. **时钟冻结**：暂停 2 s ⇒ `guest_ms` 差 < 5 ms、`paused_total_ms` ≈ +2000；**配方守卫钟同冻**；`--time-limit` 也扣暂停。
9. **僵尸判死 = `connect()`，不是 pid**：(a) `kill -9` 暂停中实例 ⇒ `list` 报 `alive:false, why:"refused"` 且文件仍在；(b) 同 root 再起 ⇒ 成功；**`emu-ctl gc`** 只删残留（条件 = 连接失败 + pid 不存活 + 文件名严格 `ctl.<pid>.<8hex>.json`），⛔ 不删 `keep.txt`、`list` 不自动删；(c) 残留文件 port 改到别人在听的 ⇒ `alive:false`；(d) `--control-idle-timeout 2` 暂停 3 s ⇒ 自收口（`bye.reason=="stopped"` + `requested_by=="idle"`）且文件删。
10. **只绑 loopback + 未授权被拒**：端点 `local_endpoint()` == `127.0.0.1`；错 token ⇒ 握手失败；带 `Origin` ⇒ 拒。
11. **多实例不猜**：两实例 ⇒ 2 alive / 2 port；`sample` 无 `--session` ⇒ exit 4 + 列出两个。
12. **监听失败响亮**：`--control-port <已占用>` ⇒ exit 2 + 点名端口，⛔ 不静默换端口。
13. **window 档必须可用**：`--present window --control-root` ⇒ pause/sample/screenshot/resume 全通、resume 后 frame 严格 +1。⚠ 本机边界：真 OS 点/拖**做不到**（窗口被遮挡、提窗被拒、`CGEvent.postToPid` 空投）⇒ 陈旧事件那一半由 `present-window` CTest 的机制面承担；⛔ 不许把"没做成"写成"通过"。
14. **收口不挂**：暂停态 `quit` ⇒ `bye` 先于摘要、0 s 级退出、exit 0、文件删；`bye.reason` 来自 run 结果，`requested_by` 说谁请求的。

## 裁决（2026-11-16）

- **window 暂停 = 完全停**：主线程停在汇点等 cv，`pumpEvents` 一次不跑（OS 判无响应可接受）；⛔ 不做过滤泵。
- **不做** `--control-hard-exit-after-s`：端点卡在不返回的 shim ⇒ 客户端 `timeout`（exit 3）、端点仍活。
- **`state_seq` = 每次暂停状态变化 +1**（进入与离开都算）⇒ 判据 4 只要求单调增。
- **`paused_total_ms` 同名不同口径**：`sample` 用**已结算**（否则 3b 必红），`pause`/`resume` 的 `res` 含当前区间 ⇒ 必须在 `--describe` 写明。
- **`script.sha_why ∈ {none,no_identity,unresolved,unreadable}`**；boot 的 `why` 仍用既有 `identity-invalid`。
- **`bye` 两字段各管一件事**：`reason` = run 为什么结束，`requested_by ∈ {client,idle,orphan}` = 谁请求的。
- **`pause` 幂等**（已暂停 ⇒ `ok:true`）；`resume` 在未暂停态才 `not_paused`。`--control-predicate-timeout`（缺省 30）保留公开。
- **`--lst` 永不给 AOT 补**（`aot-arm-shape` 禁词表里有它）；`--report` 也不补（AOT 无 BlockCache 账）。
- **`key` 不进 M0**；`--control-root` 与 `--input-plan` 可同用（`click` 走立即队列，M1）。
- **截图 = `present()` 当场冻结的像素副本**（只在 `--control-root` 时拷，≈3.7 MB/帧；⛔ 不是"暂停那一刻的现场后缓冲"——
  现场这时已被下一帧改写）。`hash_still_matches` 因此只回答"冻结有没有生效"，⛔ 不为它扩协议、不拆新字段（2026-11-16 收窄裁决）。

## 落点

`control.h`（PIMPL）+ `control_ws.cpp`（唯一 include beast/nlohmann 的 TU）+ `control_snapshot.{h,cpp}`（POD，主线程填）+ `tests/control_{endpoint,snapshot}.cpp`（顶层 pure）+ `clock.h` 的 freeze + 两个汇点（块边界 `aot_run.cpp` / Present `present.cpp` sink 之后）。

## 未做（M1/M2）

`click`/`key` / `resume --until-ea` / 录制导出 / DSH 插件包装。
**Windows 已验（2026-11-16，win32）**：`--control-root` 能 listen `127.0.0.1`，`list/pause/sample/screenshot/resume/click/quit` 全通；
`--control-port 0` 取到的端口不在保留段（`netsh` 排除段 50000–50059，取到 62556）。
仍未测：`TerminateProcess` 后的残留识别、Low 完整性（后者按用户裁决 drop："后续不考虑沙箱"）。
