# plugins/pnpm-priv — 提权安装依赖（DSH 宿主侧特权工具）

## 1. 它解决什么问题

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

## 2. 为什么这一步必须提权

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

三个写类 op **装完自动复验链接形态**并把结果一起返回（省掉"装完了才发现还是 junction"）。

## 4. 四条不许破的线

1. **闭接口**：op 只有上表那几个；pnpm 的 verb 只允许 `install` / `add` / `remove`；
   调用方**给不出新 flag**（开关都是布尔/枚举，在 `lib/ops.mjs` 里拼死）。
2. **必须落在本仓内**：`repoRoot` 必须是**绝对路径**、且**同时有 `package.json` 与 `pnpm-workspace.yaml`**
   （只看 `package.json` 会把任意 npm 包目录也放进来，在那里跑 `pnpm install` 会改别处的东西）。
   `allowedRoots` 为空 = 不额外限制；填了就按**路径段**比较（`<root>/dist` 不会把 `<root>` 放进来）。
3. **argv 数组不经 shell**：`pnpm` 用 `execFile` 跑（Windows 的 `.cmd` 不走 shell，见 `lib/run.mjs`）。
4. **写类 op 默认 dry-run + 每次留痕**：缺 `write:true` 时**一个字都不动**；无论成败都往
   `data/privileged-audit.log` 追加一行（append-only）。

★ `package` 参数只接受**包名形态**（`name` 或 `@scope/name`，可带 `@range` / `@workspace:*`）：
**不接受**路径 / `file:` / `link:` / shell 元字符 —— 那些要么越过闭接口，要么把"装依赖"变成"改别处的东西"。

## 5. 注册（环境级动作，与 `plugins/deploy` 同一套）

```bash
pnpm install                                              # 插件依赖是普通 dependencies（plugins/* 是 workspace 成员）
dsh plugin --profile web install "<本目录的绝对路径>"       # 例：E:\Projects\amayui-re\plugins\pnpm-priv
dsh plugin --profile web uninstall "@amayui/dsh-plugin-pnpm-priv"
```

★ 三个坑与 `plugins/README.md` §3 记的完全相同（`dsh.bundle.patch` 必须声明；
已作为普通依赖装过的要 `uninstall` 再 `install`；宿主插件代码**不热重载** ⇒ 改完要重启）。

## 6. 怎么用（典型一轮）

```text
pnpm_priv op=status                       # ① 看特权与链接形态（不写任何东西）
pnpm_priv op=install repoRoot=<仓库根> write=true   # ② 宿主里跑 pnpm install（建真符号链接）
pnpm_priv op=check-links repoRoot=<仓库根>          # ③ 复验：非符号链接应为 0
```

★ **判据不是"pnpm 退出 0"，而是 `check-links` 里"不是真符号链接"为 0** ——
因为沙箱里那次降级**也**会退出 0。

## 7. 边界（不做什么）

* ❌ **不执行任意命令**（没有 `run` / `exec` 这种 op）—— 那等于把沙箱整体拆掉（confused deputy）。
* ❌ 不碰游戏目录、不碰 DSH 配置（那是 `plugins/deploy` 的职责）。
* ❌ 不"顺手"改 `package.json` —— 只有 `add` / `remove` 会经由 pnpm 改它，且要显式 `write:true`。
* ❌ 不改 `nodeLinker` 之类的安装结构配置（那些写 `pnpm-workspace.yaml`，见 `AGENTS.md` §3）。
