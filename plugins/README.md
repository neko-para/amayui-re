# plugins/ — DSH 插件的**源码落点**（位置自由）

## 1. 这个目录是什么

DSH 插件的源码放这里。**位置是自由的** —— 与技能不同，插件不靠固定路径被发现，
而是通过 DSH 的插件安装机制以**软链接**注册（环境级动作、要重装）。
选 `plugins/` 只是因为放这里清楚，不代表 DSH 要求它。

## 2. 本轮状态

**已重建第一个插件：`deploy`**（`plugins/deploy/`，DSH 宿主侧特权工具；设计/安全口径/装法见其 `README.md`）。
它属于**反面情形**那一类（§2 的判据："**需要的是文件，还是需要 DSH 的运行时？**"）——
它要的既不是台账文件、也不是网页视图，而是**宿主进程的权限**（沙箱拒绝的那几步：游戏目录硬链接 /
headless Chrome / 完整性标签）。

更早那版 DSH 插件（右栏「需求」面板 + 正文引用）**已删除** —— 结论是**那一版**不划算，
**不是"插件这条路整体不成立"**：

- 它做的是**只读的"看台账"**，而台账的核心数据是本地文件 ⇒ 网页视图（`apps/requirements/`）
  能在不依赖 DSH 的前提下给同样的总览 + 详情，还能用浏览器自己的 tab 并排看；
  做成 DSH 插件反而多出三样成本：profile 级安装（要动 `$DSH_HOME`）、改代码要重装/重启、
  以及拿不到领域模型而**抄第二份解析**（而 `tools/lib/requirements.mjs` 本来就提供它）。

旧仓 `plugins/`（实测 **5** 个：`amayui-emulator` / `amayui-inspector` / `htmlcard` / `ticket-board` / `uimap`）
**不迁移**：它们几乎都要重新适配 ⇒ 登记为 `corpus/assets.json` 的 `agent/plugins`（`role: rebuild`）。

★ **反面情形仍然存在**（上面那条结论只否定"只读看台账"这一版）：要和**正在运行的会话 / 模拟器 / 宿主进程**
打交道的东西本来就和 DSH 强耦合 —— 那种需求该做成插件，不该做成网页。判断依据是
**"它需要的是文件，还是需要 DSH 的运行时？"**（后者才进 `plugins/`）。

## 3. 注册步骤（环境级，**实测过的命令**）

```bash
pnpm install                                          # 插件的依赖是**普通 dependencies**（plugins/* 是 workspace 成员）
dsh plugin --profile web install "<插件的绝对路径>"     # 例：E:\Projects\amayui-re\plugins\deploy
dsh plugin --profile web uninstall "<包名>"            # 撤回（例：@amayui/dsh-plugin-deploy）
```

三个**实测踩过**的坑（别当成"命令打错了"）：

1. 插件包必须在 `package.json` 里声明 **`dsh.bundle.patch`**（指向自己的 `cordis.patch.yml`），
   否则 `install` 只把它当**普通依赖**装进来，并警告 *"declares no dsh.bundle — installed as a plain dependency,
   not a profile layer"*；此时**profile 的 `dsh.profile.bundles` 里没有它** ⇒ 不会被加载。
2. 已经作为普通依赖装过、之后才补上 `dsh.bundle` 的，`install` 会因为 *Already up to date* 而**跳过登记**
   ⇒ **`uninstall` 再 `install` 一次**（登记只在真的 add 时发生）。
3. 宿主插件**代码不热重载**：改完 `.mjs` 要重启 DSH（配置层的变化才走 HMR）。

★ 写 `$DSH_HOME` 是工作区之外的动作 ⇒ 受限沙箱里这一步**必然**要提权（与本仓"提权只发生在少数几步"同一口径）。
★ **插件的依赖**（`@deepseek-ai/dsh-tools` 等）由根 `pnpm install` 装进工作区 —— 但**这一步也要提权**：
Windows 上 pnpm 的目录符号链接要 `SeCreateSymbolicLinkPrivilege`，沙箱里被拒后会**静默降级成 junction**，
而 Node 解析不了 junction（传递依赖报 `ERR_MODULE_NOT_FOUND`）⇒ 判据与修法见 `AGENTS.md` §3 的 `storeDir` 那条。


## 4. 与技能的区别

| | 落点 | 注册 |
|---|---|---|
| DSH 插件 | **位置自由**（本目录只是源码落点） | 软链接注册，要重装 |
| 技能 | **固定** `.agents/skills/<名字>/SKILL.md` | 无需注册 |

★ **不要为了迎合注册方式去扭曲仓库结构**：插件的注册是环境级动作，技能才必须遵守固定路径。
