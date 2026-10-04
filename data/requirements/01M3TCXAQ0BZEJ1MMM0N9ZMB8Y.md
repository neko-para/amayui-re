# 批 M6 · agent 基建重建（技能 + DSH 插件）

- id: REQ-01M3TCXAQ0BZEJ1MMM0N9ZMB8Y
- type: req
- status: doing
- parent: REQ-01M3TC9660ZG1YMB7KAKE4W94B
- order: 80

## 范围
agent 基建按新结构重建：**技能**（路径固定）与 **DSH 插件**（位置自由、环境级注册）。
口径见 `AGENTS.md` §7；本节点只记"到什么程度了"。

## 已落点
* **技能**：`.agents/skills/amayui-translate/`（路径固定、无需注册，DSH 按固定路径发现）。
* **插件**：`plugins/deploy/`（**本仓第一个插件**）—— DSH **宿主侧特权工具**：
  把沙箱拒绝的那几步（对游戏安装目录建硬链接 / headless Chrome / 改完整性标签）做成**闭接口工具** `deploy`。
  设计 / 安全口径 / 装法 / 目录见其 `README.md`。
  * 注册**实测命令**（已补进 `AGENTS.md` §7）：`dsh plugin --profile web install "<插件绝对路径>"`；
    包必须声明 `dsh.bundle.patch`，否则只当普通依赖装进来、**不会**被组合；
    已装成普通依赖的，`uninstall` → `install` 一次才登记成层；宿主插件代码**不热重载**（改完重启）。
  * 依赖：运行时按 `file://` 从**正在运行的这一份 DSH**取 `defineTool` / `z` ⇒ 不联网、不装第二份、不漂版本。
  * 留痕：`data/privileged-audit.log`（append-only，一行一次，**被拒绝的调用也记**）。

## 判据
* **技能**：`pnpm tools requirements validate` 与技能目录约定（`.agents/skills/<名字>/SKILL.md`）本身即约束。
* **插件**：`plugins/*/test/**` 进 `pnpm test`（闭接口 / 参数校验能红 / argv 形状 / 审计一行 / **宿主能加载并注册出工具**）；
  另外两个**必须由人做**的动作各留一条可复核的痕迹：
  * **注册**：`dsh plugin --profile web install`（写 `$DSH_HOME`，工作区外 ⇒ 要提权）；
  * **特权是否真的成立**：调一次 `deploy op=status` —— 它报"宿主起的子进程完整性"，
    正常应是 `Medium S-1-16-8192`（受限沙箱里是 `Low S-1-16-4096`，且管道读不到）。

## 剩下的事（本批不只这一件）
* 旧仓 8 个技能里其余几个是否重建、以及**是否值得**重建，逐个判（判据：它需要的是文件，还是 DSH 的运行时）。
* 插件只有 `deploy` 一个；以后"操纵模拟器"那类强耦合需求才该加插件（见 `plugins/README.md` §2 的判据）。
