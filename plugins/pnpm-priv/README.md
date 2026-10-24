# plugins/pnpm-priv — 提权安装依赖 + 宿主侧转发固定任务（DSH 宿主侧特权工具）

## 1. 它解决什么问题

### 1.1 装依赖：要**真符号链接**

Node 的**原生类型剥离**（让 `.ts` / `.mts` 不用构建就能跑）有一条硬限制：

```
ERR_UNSUPPORTED_NODE_MODULES_TYPE_STRIPPING
  Stripping types is currently unsupported for files under node_modules
```

而"跨包用**包名**导入"必然把文件解析到 `node_modules/@amayui/<包>` 下面 ⇒ **包名导入**与**跨包 `.mts`**
在 Node 上**互斥**（上游议题：[nodejs/node#63853](https://github.com/nodejs/node/pull/63853)）。

★ **实测的绕过条件**（2026-10，本机三组对照）：

| 导入形态 | 结果 |
|---|---|
| 经**真符号链接**（`lstat.isSymbolicLink === true`，realpath 落在 `node_modules` **之外**） | ✅ 成功 |
| 经 **junction**（`isSymbolicLink === false`，realpath 仍在 `node_modules` 内） | ❌ 拒绝 |
| 纯真实路径 / `./node_modules/../../x.mts`（URL 里有、**解析路径**没有） | ✅ 成功 |

⇒ 判据是**解析后的路径含不含 `node_modules`**，**不是**"是不是包名导入"。

### 1.2 构建 / 起窗口：沙箱里 `spawn EPERM`

这两步本身不需要符号链接，但它们需要**正常完整性**：

* `vite build` 在 Windows 上会先 `spawn` 一个 helper 去查真实路径（Vite 的 `optimizeSafeRealPathSync`）
  ⇒ 在 Low 完整性的沙箱进程里直接 `[plugin externalize-deps] Error: spawn EPERM`；
* Electron 要 `CreateProcess` 一个 GUI 进程，同理起不来。

宿主进程（插件代码所在的那个）是**正常完整性** ⇒ 把这两步**转发**给宿主，是"不拆沙箱"的唯一解法。
⇒ 见 §3 的 `run` op：**只认固定任务名**，不是通用 exec。

## 2. 为什么"装依赖"这一步必须提权

Windows 上建**真**目录符号链接要 `SeCreateSymbolicLinkPrivilege`。
沙箱里的 `pnpm install` 拿不到它 ⇒ pnpm **静默降级成 junction**（装完不报错，链接形态却是错的）：

```
mklink /D <target>                     → You do not have sufficient privilege
fs.symlinkSync(target, dst, 'dir')     → EPERM
fs.symlinkSync(target, dst, 'junction') → 成功，但 isSymbolicLink === false
```

`link:` 协议**本身**是对的路子（pnpm 用它建符号链接；npm 2017 年把 `file:` 改成了拷贝）——
但在本机它最后仍然落到 junction，因为**卡点是特权、不是协议名**。
⇒ 唯一可靠的解法是**在宿主进程里跑 `pnpm install`**（宿主不受 ACL 沙箱约束）。

## 3. 工具接口（闭集合）

`pnpm_priv` 的 op：

| op | 做什么 | 需要 `write: true` |
|---|---|---|
| `status` | **先跑这个**：探针验证"本插件进程能否建真符号链接"（在系统临时目录里建、验完**立刻删**）+ pnpm 入口 + 各消费方的链接形态 | 否（只读 + 自清探针） |
| `check-links` | 逐个点名 `node_modules/@amayui/*` 里**不是真符号链接**的那些 | 否 |
| `install` | `pnpm install`（可选 `--frozen-lockfile` / `--force`） | **是** |
| `add` | `pnpm add [-D] [-w] <spec>` | **是** |
| `remove` | `pnpm remove [-w] <spec>` | **是** |
| `run` | 转发**一个固定任务**（`task` 是闭集合，见 §3.1） | **是**（构建写 `dist/`、启动起进程，都算有副作用） |

三个 pnpm 写类 op **装完自动复验链接形态**并把结果一起返回（省掉"装完了才发现还是 junction"）。

### 3.1 `run` 的三个任务（`task` → argv / cwd **全部写死在 `lib/ops.mjs`**）

| `task` | 宿主里实际跑什么 | 模式 | 前置 / 判据 |
|---|---|---|---|
| `build-renderer` | `pnpm --filter @amayui/emulator-new run build:renderer`（= 那个包自己的 `vite build`） | 等它退出 | 判据 = 退出 0 **且** `apps/emulator-new/dist/renderer/index.html` 的 mtime 真的变了 |
| `smoke` | `electron --no-sandbox apps/emulator-new --smoke`（起窗口 → 截图 → 退出） | 等它退出 | 前置 `dist/renderer/index.html`；判据 = 退出 0 **且** `.tmp/emulator-new-electron-smoke.png` 是这次写的 |
| `electron` | `electron --no-sandbox apps/emulator-new`（给 `installDir` 就追加 `--install <dir>`） | **起进程就返回** | 前置 `dist/renderer/index.html`；日志 `.tmp/emulator-new.log`（app 自己写的持久日志） |

★ `electron` 是**交互式**的（窗口要留给人玩）⇒ 它是 `detached` 起、**不随本回合结束而退出**，
返回 pid 与日志落点；`smoke` / `build-renderer` 是"跑完就要结论"的，所以等它退出并把输出尾部回给模型。
★ **产物判据不是多余的**：只看退出码会得到假绿（构建退出 0 但没写 `dist/renderer`、smoke 退出 0 但用的是上次的截图）。

## 4. 六条不许破的线

1. **闭接口**：op 只有上表那几个；pnpm 的 verb 只允许 `install` / `add` / `remove`；
   调用方**给不出新 flag**（开关都是布尔/枚举，在 `lib/ops.mjs` 里拼死）。
2. **必须落在本仓内**：`repoRoot` 必须是**绝对路径**、且**同时有 `package.json` 与 `pnpm-workspace.yaml`**
   （只看 `package.json` 会把任意 npm 包目录也放进来，在那里跑 `pnpm install` 会改别处的东西）。
   `allowedRoots` 为空 = 不额外限制；填了就按**路径段**比较（`<root>/dist` 不会把 `<root>` 放进来）。
3. **argv 数组不经 shell**：`pnpm` 用 `execFile` 跑（Windows 的 `.cmd` **不走 shell**：Node 从 v20 起
   拒绝 `execFile('x.cmd')`，所以先找它的 JS 入口 `node_modules/pnpm/bin/pnpm.cjs`；只有连 JS 入口都没有时
   才退到 `cmd.exe /d /s /c <shim>` —— 那是 shim **自己的**形态，不是我们给调用方的口子）。
   Electron 同理：直接定位 `<app>/node_modules/electron/dist/electron.exe`（Electron 自己的 `path.txt`）。
4. **写类 op 默认 dry-run + 每次留痕**：缺 `write:true` 时**一个字都不动**（`run` 也一样：
   只打印"本来会跑什么"）；无论成败都往 `data/privileged-audit.log` 追加一行（append-only）。
5. **`run` 没有"任意命令"**：`task` 是闭集合（三个），argv / cwd / 超时**全部来自 `lib/ops.mjs`
   的 `TASKS` 表** —— 调用方给不出命令、给不出 flag、也给不出 cwd。唯一能由调用方提供的 argv
   片段是 `installDir`（只有 `electron` 接受，且必须是**存在**的目录）。
6. **`repoRoot` 仍是唯一的工作目录来源**：任务的 cwd 一律是 `<repoRoot>` 或它下面的固定子目录。

★ `package` 参数只接受**包名形态**（`name` 或 `@scope/name`，可带 `@range` / `@workspace:*`）：
**不接受**路径 / `file:` / `link:` / shell 元字符 —— 那些要么越过闭接口，要么把"装依赖"变成"改别处的东西"。
★ `installDir` 同理只当**一个 argv 值**用（不经 shell、不拼字符串）；若部署方把 `allowedRoots` 收窄到仓库根，
游戏安装目录（在工作区之外）会一起被挡掉 —— 要放行就把它也列进 `allowedRoots`。

### 4.1 残余风险（说清楚，不藏着）

`run task=build-renderer` 会执行 `apps/emulator-new/package.json` 里的脚本（进而跑 `vite.config.ts`），
`electron` 会执行那个 app —— 它们都是**仓库里的代码**，在**宿主完整性**下跑。
这条通道**不比现有的 `install` 更宽**（`pnpm install` 本来就会跑包的 lifecycle 脚本），
所以它没有新增一类信任假设；但要清楚：本插件的边界是"**闭集合 + 默认 dry-run + 留痕**"，
用来防**误用手滑**与"模型自己拼命令"，**不是**对抗恶意代码的硬化沙箱。
⇒ 部署方（人）不同意时可以在 profile 的 config 里写 `allowRun: false`（总闸，模型改不了）。

## 5. 注册（环境级动作，与 `plugins/deploy` 同一套）

```bash
pnpm install                                              # 插件依赖是普通 dependencies（plugins/* 是 workspace 成员）
dsh plugin --profile web install "<本目录的绝对路径>"       # 例：E:\Projects\amayui-re\plugins\pnpm-priv
dsh plugin --profile web uninstall "@amayui/dsh-plugin-pnpm-priv"
```

★ 三个坑与 `plugins/README.md` §3 记的完全相同（`dsh.bundle.patch` 必须声明；
已作为普通依赖装过的要 `uninstall` 再 `install`；宿主插件代码**不热重载** ⇒ 改完要重启）。

★ 本插件的三个 Config 键（都在 profile 的 loader entry 里配，**模型改不了**）：

| 键 | 缺省 | 作用 |
|---|---|---|
| `allowedRoots` | `[]`（不额外限制） | 收窄"能在哪些根里动作"：`repoRoot` 与 `installDir` 都要落在里面 |
| `auditFile` | `data/privileged-audit.log` | 审计落点（相对仓库根） |
| `allowRun` | `true` | `false` ⇒ `run` op **一律拒**（其余 op 不受影响） |

## 6. 怎么用（典型一轮）

```text
pnpm_priv op=status                                 # ① 看特权与链接形态（不写任何东西）
pnpm_priv op=install repoRoot=<仓库根> write=true     # ② 宿主里跑 pnpm install（建真符号链接）
pnpm_priv op=check-links repoRoot=<仓库根>           # ③ 复验：非符号链接应为 0
pnpm_priv op=run task=build-renderer repoRoot=<仓库根> write=true   # ④ 宿主里 vite build（沙箱里 spawn EPERM）
pnpm_priv op=run task=smoke repoRoot=<仓库根> write=true            # ⑤ 起窗口 → 截图 → 退出（要 ④ 先过）
pnpm_priv op=run task=electron repoRoot=<仓库根> write=true         # ⑥ 起窗口就返回（可加 installDir=<游戏目录>）
```

★ **判据不是"pnpm 退出 0"，而是 `check-links` 里"不是真符号链接"为 0** ——
因为沙箱里那次降级**也**会退出 0。
★ `run` 同理：判据是**产物真的更新了**（`dist/renderer/index.html` / smoke 截图），
而不是"进程退出 0"；`electron` 的判据是"1.5 s 后它还活着"（`spawn` 的失败是异步的）。

## 7. 边界（不做什么）

* ❌ **不执行任意命令**：有 `run`，但它**只认三个任务名** —— 任务名 → argv / cwd / 超时全部写死在
  `lib/ops.mjs` 的 `TASKS` 表里。**没有** `exec` / `shell` / "把这条命令转发一下"这种口子
  （那等于把沙箱整体拆掉：confused deputy）。
* ❌ 不碰游戏目录的**内容**（`installDir` 只作为一个 argv 值传给 app）、不碰 DSH 配置（那是 `plugins/deploy` 的职责）。
* ❌ 不"顺手"改 `package.json` —— 只有 `add` / `remove` 会经由 pnpm 改它，且要显式 `write:true`。
* ❌ 不改 `nodeLinker` 之类的安装结构配置（那些写 `pnpm-workspace.yaml`，见 `AGENTS.md` §3）。
