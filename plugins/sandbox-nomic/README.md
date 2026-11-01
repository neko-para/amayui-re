# plugins/sandbox-nomic —— 用「受限令牌 + DACL」实现 `workspace-write`（**不打完整性标签**）

> 一句话：把 DSH 在 Windows 上的 `workspace-write` 从「受限令牌 + 全树 Low 标签」换成
> 「受限令牌 + 能力 SID 的 DACL」。目标是去掉 MIC 的**破坏性副作用**（工作区里的 exe 变成
> Low 完整性、离开 DSH 也受影响、需要 `WRITE_OWNER`、全树 SACL 常驻），同时保住写边界。
>
> 这是 **route A 的 PoC**，不是成品：它替换 `ctx.sandbox`，只在 win32 上有意义。

## 1. 它替换掉的是什么

出厂的 win32 链路（`dsh-sandbox-local` → `windows-acl`）做三件事：

| # | 出厂 | 本插件 |
|---|---|---|
| 1 | 复制令牌成 `WRITE_RESTRICTED`，限制 SID = `[logon, Everyone, workspace, temp]` | **不变** |
| 2 | `SetTokenInformation(TokenIntegrityLevel)` 把令牌降到 **Low** | **不降**（保持 Medium） |
| 3 | `SetNamedSecurityInfoW`（info = 20）一次写三样：能力 SID 的 Allow ACE、world 的 `FILE_DELETE_CHILD` Deny、**Low 可继承标签** | info = **4**，只写 Allow + Deny，**不读不写 SACL** |
| 4 | 每 **live session/workspace 对**一个私有 temp（provider 持有 `--temp` + `--temp-write-sid`，dispose 时撤） | **不变**（与出厂同生命周期） |

第 3 条是破坏性的根源：标签可继承、**常驻不撤**，因此工作区里每个文件（包括 `.exe`）都带上
Low 标签；而按微软文档，「把 image 文件设成 low integrity，**它被启动时进程就是 low integrity**」
（[Designing Applications to Run at a Low Integrity Level](https://learn.microsoft.com/en-us/previous-versions/dotnet/articles/bb625960(v=msdn.10))）。
于是 DSH 之外双击同一个 exe 也是 Low：写不了 `%LOCALAPPDATA%`、写不了 HKCU、受 UIPI 限制。

去掉标签后，第 3 条也不再需要 `WRITE_OWNER`（标签在 SACL；DACL 只需 owner 自带的 `WRITE_DAC`），
所以「非系统盘工作区 → `SetNamedSecurityInfoW failed (Win32 5)`」这类失败也一起消失。

## 2. 为什么没有标签仍然有边界

`WRITE_RESTRICTED` 的第二次访问检查**只复核写访问**，且只认限制 SID 列表。于是：

- 工作区 / 私有 temp 上有能力 SID 的 Allow ACE ⇒ 写通过；
- 其他位置除非 DACL 里恰好给了 `Everyone`（或 logon SID）写权限 ⇒ 写被拒；
- `FILE_DELETE_CHILD` 的 Deny 仍然保留 ⇒ 一个授权根不能借父目录的删除权去删另一个根里的东西。

**这就是缺口所在**：`Everyone` 可写目录、没有 DACL 的卷（FAT/exFAT）、NULL DACL 对象、网络共享
（本地令牌不参与远端检查），以及 NTFS 硬链接（同一文件对象跨路径别名，上游也把它列为 partial 理由）。

出厂的 Low 标签正是为了堵第一个；本变体**不堵**它，而是显式接受。

注意 `enforcement: 'partial'` **不是**本变体的选择：出厂的 `windows-acl` rung 本来就报 `partial`
（硬链接 / 读不设防 / AppContainer ACL 边界）。区别在**缺口集合**：出厂靠 MIC 顺带堵住
`Everyone` 可写，本变体没有 MIC，所以那一类也落在边界外。代价与 OpenAI Codex 的 `unelevated`、
Anthropic `srt` 的 Windows 后端同一档。

## 3. 实测（`verify.mjs`，本机 Windows 11 26200）

一次性临时树，13/13（agentless 路径：runner 自己建一次性 temp）：

| 检查 | 结果 |
|---|---|
| 令牌完整性 = `S-1-16-8192`（Medium），不是 `S-1-16-4096`（Low） | ✅ |
| 写工作区 | ✅ 允许 |
| 写工作区外 | ✅ 拒绝（`EPERM`） |
| 写「Everyone 可写」目录 | ⚠️ **允许**（公开接受的缺口） |
| 受限子进程里 `spawn(..., stdio:'pipe')` | ⚠️ 仍然 `EPERM`（**受限令牌**的性质，不是 MIC） |
| `cmd /c echo hi>file`（shell 重定向） | ✅ |
| `read-only` 下写工作区 | ✅ 拒绝 |
| 退出码透传 | ✅（`cmd /c exit 7` → 7） |
| runner 自身失败（如工作区不存在） | ✅ 打 `nomiv-run: …` 并退出 **127** |
| 目标程序不存在 | ✅ 打 `nomiv-run: spawn failed: …` 并退出 **126**（不算 runner 坏） |

每会话 temp 的生命周期由 `test/provider.test.mjs` 覆盖（同一会话复用、不同会话隔离、dispose 删除）。

复现：`node plugins/sandbox-nomic/verify.mjs`（或 `pnpm --filter @amayui/dsh-plugin-sandbox-nomic verify`）。

## 4. 修好了什么 / **没**修好什么

修好：

- 工作区里的 exe 不再被标 Low ⇒ DSH 外启动的程序恢复正常完整性；
- 不再需要 `WRITE_OWNER` ⇒ 非系统盘的 `Win32 5` 不再出现；
- 不再有常驻的可继承 SACL，也就没有「`relabel-medium` 之后下次进 workspace-write 又被改回 Low」；
- `icacls` 无法撤销的能力 ACE 仍然常驻（与出厂同），但不再有标签这一层。

**没**修好（都是受限令牌本身，不是 MIC）：

- 受限子进程里**管道 stdio** `EPERM`（`spawn`/`execFile` 抓输出）——需要结构化重构或提权转发；
- 控制台隔离、`CREATE_NO_WINDOW` 子进程的 DLL 初始化失败；
- 读、网络、进程可见性**本来就不防**；
- **UI 隔离变弱**：Low 进程受 UIPI 限制是 MIC 的副产品，改成 Medium 后能和你的浏览器/Explorer 窗口通信。
  Codex 用「私有桌面」补这一层，本插件没有；
- `Everyone` 可写 / NULL DACL / FAT / 网络共享仍然可写；
- **NTFS 硬链接**：工作区里的文件带能力 ACE，若外面事先存在指向同一文件对象的硬链接，那条路径仍过
  pass-2（上游把这条也列为 partial 的理由）；
- 每会话一个私有 temp：命令**之间**共享（同一会话），但 provider dispose 时整个删掉——
  跨会话靠 `%TEMP%` 中转的流程会丢文件（与出厂同）。

## 5. 安全定位

| 威胁 | 本插件 |
|---|---|
| 模型手滑写到工作区外 | 挡住（NTFS 非 Everyone 路径） |
| 提示注入做定向破坏 | 部分挡住；Everyone/FAT/共享/UI 是缺口 |
| 蓄意逃逸 | 挡不住（同世界沙箱；任何 restricted-token 方案都不是 containment） |

要更强只有换身份：专用本地账户 + 私有桌面（Codex `elevated` / Anthropic `srt`），或 MXC 的
`isolation_session` / Windows Sandbox / VM。

## 6. 装 / 撤

```bash
# 依赖：与正在运行的 DSH 同版本（本机 0.2.1-rc.2）
pnpm install
dsh plugin --profile web install "E:\Projects\amayui-re\plugins\sandbox-nomic"
# 重启 DSH（宿主插件代码不热重载；新的 bundle 层要重新 boot）
```

`cordis.patch.yml` 会 `disabled: true` 掉出厂的 `sandbox` 行，再插入本插件 —— **只在 Windows profile 里启用**。

卸载：

```bash
dsh plugin --profile web uninstall "@amayui/dsh-plugin-sandbox-nomic"
```

★ 若这棵树**曾经**被出厂沙箱打过 Low 标签，卸载后要自己改回来：
`icacls "<树>" /setintegritylevel Medium /T /C`（改完该子树就不再落在出厂沙箱的可写范围内，
但本插件不依赖标签，所以无碍）。

★ **改用本插件时也要做这一次**：它只保证「不再写标签」，**不负责清理已有标签**。已经带 Low 标签的树
在本插件下仍能正常读写（Medium 写 Low 是 write-down），但树里的 exe 仍然会在 DSH 之外以 Low 启动。
一次 `icacls <root> /setintegritylevel Medium /T /C` 即可（之后不会被本插件改回去）。

## 7. 本地开发：`node_modules` 里的 junction

PoC 不重新下载依赖：`node_modules/@deepseek-ai/{dsh-win32-process,dsh-lazy-require,dsh-sandbox,cordis}`
与 `node_modules/koffi` 是指向**当前 DSH 安装**里同版本包的目录 junction。正式安装由
`pnpm install` / `dsh plugin install` 解析 `package.json` 里的 `dependencies`。
（junction 只是本机 PoC 的解析捷径；`node_modules` 不入 git。）

## 8. Provenance（vendored，MIT）

`lib/acl-sandbox.mjs` 来自 `@deepseek-ai/dsh-sandbox-windows-acl@0.2.1-rc.2` 的
`lib/types-Cl_DXjhk.js`（上游 MIT，见 `LICENSE.dsh-sandbox-windows-acl`），只改这些：

1. 删掉 `acl-skill` 区（诊断技能的注册）与 `@deepseek-ai/dsh-skill` / `yaml` 两个 import；
2. `readCurrentSecurity`：`SecurityInformation` 20 → **4**（只读 DACL，不读 SACL）；
3. `grantWrite`：幂等判据去掉 `hasExactLabel`；apply 从 `{kind:'apply', acl: label}` 改成 `{kind:'keep'}`；
4. `revokeWrite`：永远 `{kind:'keep'}`（不再清 SACL）；
5. `AclSandbox.init`：删掉 `restrictTokenIntegrity(...)` 调用；
6. 新增 `tokenIntegrityLevel(api, token)`（`--report-integrity` 用）；
7. 导出改成可读名字。

其余（限制 SID 列表、令牌默认 DACL、Job object、spawn、SID 派生、清理）**逐字未改**。

## 9. 已知限制

- **仅 win32**；`confine()` 对其他平台直接抛错。
- 依赖上游 **0.2.1-rc.2** 的内部实现（vendored）：升级 DSH 时要重新对 diff。
- `enforcement` 恒为 `partial`——这是事实，不是保守（缺口集合见 §2）。
- 不注册出厂的 `diagnose-windows-sandbox-acl` 技能（它随 `sandbox` 行一起被禁用）。
- 首次授权仍会做一次全树 **DACL** 传播（4 万文件量级约 20 s），之后走 exact-ACE skip；
  比出厂的「DACL + SACL」轻，但不是零。
- 能力 SID 的 Allow ACE / Deny 仍然常驻（与出厂同），换工作区路径会派生新 SID、旧 ACE 留下。
- 每会话 temp 只在 provider 存活期间有效；崩溃时目录 + 可撤销 ACE 会残留（与出厂同，靠 OS temp 清理）。

## 10. 与社区 workaround 的区别

- [`masknull/dsh-acl-sandbox-patch`](https://github.com/masknull/dsh-acl-sandbox-patch)：**hook `koffi` 的 FFI 调用**，
  在打不上标签的环境里降级成「DACL-only + 不降 IL」。它证明了同一条路可行，但依赖上游内部实现与
  加载顺序（还要 `NODE_OPTIONS=--require` 预加载）。
- [`lovezi0/dsh-workspace-acl-allow`](https://github.com/lovezi0/dsh-workspace-acl-allow)：**预授权 FullControl**，
  让写标签那一步能成功 —— 保留 MIC，只是把 `Win32 5` 绕过去。
- 本插件换的是**公开 seam**（`ctx.sandbox.confine()`），不 hook 任何内部调用；代价是 vendored 了一份
  上游实现（见 §8）。

## 11. 怎么查 / 怎么改

- 边界是否成立：`node verify.mjs`（临时树，自清；agentless 路径 13 项）。
- 每会话 temp / 服务注册：`node --test test/provider.test.mjs`（`@env external`，仅 win32）。
- 纯函数契约：`node --test test/confine.test.mjs`（`@env pure`，`pnpm test` 会跑）。
- 失败签名只在 `lib/protocol.mjs` 定义；`verify.mjs` 与单测都按**字面量** `nomiv-run: ` 断言。
- 改「不降 IL / 不写标签」：只在 `lib/acl-sandbox.mjs` 的 4 处 `NOMIC` 注释附近。
- 改 runner 契约：`lib/runner.mjs` 的 `parseArgs` 与 `lib/confine.mjs` 的 `buildConfinedArgv` 必须同步
  （退出码 126/127 的分工见 `lib/protocol.mjs`）。
