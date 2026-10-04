# plugins/deploy —— 受信任的部署工具（DSH **宿主侧**插件）

> 一句话：把"沙箱拒绝的那几步"做成**一个闭接口工具** `deploy`。插件代码跑在 DSH **宿主进程**里，
> 而 ACL 沙箱只约束**工具执行器**spawn 出来的子进程 ⇒ 宿主侧的 `fs` / `child_process` 不受那套令牌限制。
> 接口 / 参数 / 不变量的真源是 `lib/ops.mjs` 的 `OPS` 表与工具自描述；本文件写"为什么 + 口径 + 怎么用"。

## 1. 它解决什么（三件事沙箱里做不了）

| 件 | 为什么沙箱做不了 |
|---|---|
| 对**游戏安装目录**建硬链接（`install-tree` 的 ALF） | 建硬链接会**改到源文件的链接数** ⇒ 不是纯读操作（实测 `EPERM`） |
| `ui-bake build`（headless Chrome） | Chrome 的 mojo IPC 要开**命名管道** ⇒ 受限沙箱里 `FATAL:mojo … 拒绝访问` |
| 改**完整性标签**（`relabel-medium`） | 要写主 ACL / `WRITE_OWNER` ⇒ 受限令牌下被拒 |

而"每次都要人点同意"（`sandbox_permissions` 升级 + 审批）不能自动化 —— 所以需要**人预先授权的、窄的**那一件事。

## 2. 为什么这条路成立（机制 + 怎么自己再验一遍）

* 沙箱是**执行器**的属性，不是宿主的属性：`dsh-pwsh-sandbox` / `dsh-bash-sandbox` 才隔离，
  而 `dsh-bash-local` 的 README 明说"命令以 **harness 进程自身的权限**运行：本执行器不做任何隔离"。
* 插件由 cordis 的 **host runner 在宿主里**加载 ⇒ 它的 `node:fs` / `child_process` 不受沙箱令牌限制。
* ★ **自己验（别信文档）**：调 `deploy op=status` —— 它报告"**宿主起的子进程是什么完整性**"。
  实测：`Mandatory Label\Medium Mandatory Level S-1-16-8192` 且**管道读得到** `whoami` 的输出；
  而同一个沙箱会话里是 `Low S-1-16-4096` + 管道 `EPERM`。
* 三个探针 op（`status` / `probe-link` / `probe-write`）就是为"**可证伪**"准备的：做完立刻撤销，不留痕迹。

## 3. 接口（**只有这些**；没有"执行任意命令"这种 op）

| op | 做什么 | 要 `write:true` | 落点 |
|---|---|---|---|
| `status` | 报告宿主进程事实（含子进程完整性、DSH 包来源） | 否（只读） | —— |
| `probe-link` | 对游戏安装目录的 ALF 建硬链接、核对 inode 后**立刻删** | 否（自清） | `dir`（须与游戏同卷） |
| `probe-write` | 在 `dir` 写一个临时件、回读后**立刻删** | 否（自清） | `dir` |
| `install-tree` | `release install`（ALF 硬链接 / 覆盖件 / 可选 LE 启动器） | **是** | `out` ∈ `allowedRoots` |
| `bake-ui` | `ui-bake build`（headless Chrome） | **是** | `out` ∈ `allowedRoots` |
| `relabel-medium` | `icacls <树> /setintegritylevel Medium /T /C` | **是** | `out` ∈ `allowedRoots`（须已存在） |

* **授权写在配置里（人改，模型改不了）**：profile 里那条 loader entry 的 `config.allowedRoots`
  （缺省 `['dist']`，相对仓库根；要写到工作区外就加绝对路径，例如 `E:\Projects`）。
* 参数**受校验**：`out` 必须绝对且在 `allowedRoots` 内、不许是盘符根/用户主目录；
  `alf` 只能是枚举值；`leProfile` 必须是 GUID；`baked`/`leCmd` 必须存在。
* argv 一律**数组、不经 shell**，形状在 `lib/ops.mjs` 里拼死 ⇒ 塞不进新 flag。
* ★ `probe-*` **故意**允许落在 `allowedRoots` 之外 —— 否则证不了"能不能写工作区外"；
  代价是它们只动**元数据**（硬链接）或**一个临时件**，且 `finally` 里必删。

## 4. 安全口径（**这是信任边界，别松**）

1. **插件代码由人写/审/装**。❌ 不要让 agent 自己改这个插件、再自己装 —— 那等于把沙箱整体拆掉
   （`dsh plugin install` 是本插件唯一的"人做的"一步）。
2. **接口只增不改宽**：新增能力 = 在 `lib/ops.mjs` 的 `OPS` 里加一个 op + 更新本文件。
   ❌ 永远不加 `op:'run'` / `cmd` 这类"接受任意命令"的形态。
3. **每次调用留痕**：`data/privileged-audit.log`（append-only、一行一次、**被拒绝的调用也记**）。
   行格式的真源是 `lib/ops.mjs` 的 `auditLine()`。
4. **写类 op 缺省 dry-run**：不给 `write:true` 就只出工具自己的计划。
5. **提示注入的边界**：模型能读网页与文件 ⇒ "能驱动这个工具"就等于"能驱动这几件特权操作"。
   所以 op 越少越好、`allowedRoots` 配得越窄越好。
6. 本插件**不**提升为管理员：它要的只是"**不在受限令牌里**"，不是 Administrator。

## 5. 装（环境级，实测过的命令）

```bash
# ① 装（写 $DSH_HOME ⇒ 受限沙箱里要提权；就是这一条是人做的）
dsh plugin --profile web install "E:\Projects\amayui-re\plugins\deploy"

# ② 自检（可选，仓库内）：插件能不能被 DSH 的 API 吃下 —— 这条也是 `pnpm test` 的一部分
pnpm test -- plugins/deploy          # 或在插件目录：pnpm --filter @amayui/dsh-plugin-deploy test

# ③ 真正的验证（**宿主侧执行**，只有它能回答"不受沙箱约束"）
#    注册后由一个会话调工具： deploy op=status
```

* 包**必须**声明 `dsh.bundle.patch`（见 `package.json` 的 `dsh` 字段 + 本目录的 `cordis.patch.yml`）：
  否则 `dsh plugin install` 会警告 *"declares no dsh.bundle — installed as a plain dependency, not a profile layer"*。
  已在 `dependencies` 里但没登记成层时，**`uninstall` → `install` 一次**即可（install 只在真的 add 时才登记）。
* 宿主插件**不热重载代码**：改完 `index.mjs` / `lib/**` 要**重启 DSH**（配置层的变化才走 HMR）。
* 撤回：`dsh plugin --profile web uninstall "@amayui/dsh-plugin-deploy"`。
* **依赖（普通 `dependencies`，与旧仓那几个插件同形）**：`@deepseek-ai/dsh-tools`（`defineTool`）与
  `@deepseek-ai/schemastery`（`Config` 的 schema），`@deepseek-ai/cordis` 声明为 peer（宿主提供）。
  `plugins/*` 是**本仓 workspace 成员** ⇒ 由根 `pnpm install` 装（落在工作区内的 `.pnpm-store`）。
  ★ **版本要与正在运行的 DSH 同步**（当前 `0.2.0-rc.2`）：升 DSH 时同步升 `package.json` 里那两行。
  ★★ **装依赖这一步要提权（Windows）**：pnpm 建的是**目录符号链接**，受限沙箱里被拒后它会**静默降级成
  junction**；而 Node **不能 realpath junction** ⇒ **传递依赖解析不到**（实测症状：`schemastery` 报
  `Cannot find package '@deepseek-ai/cosmokit'`）。判据：`fs.lstatSync('<包>').isSymbolicLink === true`；
  修法：删掉 `plugins/deploy/node_modules` 后**提权重跑 `pnpm install`**（口径见 `AGENTS.md` §3 `storeDir` 那条）。

## 6. 目录

```text
plugins/deploy/
  index.mjs            # DSH 插件入口：name / inject / Config / apply → 注册工具 `deploy`
  cordis.patch.yml     # bundle patch：一条 loader entry（部署方按 id 覆盖 config）
  lib/ops.mjs          # ★ 闭接口：OPS 表 + 参数校验 + argv 组装 + 审计行（纯函数，可测）
  lib/probe.mjs        # 特权探针：status / probe-link / probe-write（宿主侧 fs，做完自清）
  lib/run.mjs          # 跑本仓 CLI（argv 数组、超时、中断、输出裁剪）
  lib/audit.mjs        # append-only 审计（data/privileged-audit.log）
  test/ops.test.mjs    # 闭接口 / 校验能红 / argv 形状 / 审计一行 / 默认 io 是真 fs
  test/plugin.test.mjs # 宿主侧加载：defineTool 吃下 spec、apply 注册出工具、status 能跑
```

## 7. 怎么查 / 怎么改

* **查它做过什么**：`data/privileged-audit.log`（append-only；`git log -p` 也能看历史）。
* **查它现在能不能**：`deploy op=status`（宿主视角，唯一算数的那个）；`pnpm --filter @amayui/dsh-plugin-deploy test`（加载契约）。
* **改授权面**：改 profile 里那条 entry 的 `config.allowedRoots`（**人**改，别让模型改）。
* **加一个新 op**：`lib/ops.mjs` 的 `OPS` + `planOp` 的一个分支 + `test/ops.test.mjs` 的判据 + 本文件 §3 的表。
* **不要做**：`op:'run'`、把参数拼进 shell、放开"任意输出路径"、把 `write:true` 变成缺省。
