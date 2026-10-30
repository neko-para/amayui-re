# `data/routes/screens.json` — 界面路径登记表

这份表登记的是**宿主测试装置**的路径知识：**从 boot 出发、用脚本化输入时间线到达各个稳定界面**，并写清判据（到达哪个脚本）与一份可再校验的证据（实际跑过的命令 + 日期）。

⛔ **它不是引擎 spec**：这里只有"宿主喂了什么输入、判据是哪个脚本、当时看到什么"，没有一条引擎语义结论。引擎语义在 `data/spec/`（入口见 `pnpm tools spec describe`）。

字段语义 / 枚举 / 不变量 / 操作**只看工具自描述** —— 那是 schema 的唯一真源，本文件**不复述**（复述必然漂）：

```bash
pnpm tools routes describe
```

## 怎么查

```bash
pnpm tools routes describe                 # 字段 / 步骤语法 / 不变量 / 操作（schema 真源）
pnpm tools routes list                     # 一行一条：id / from / arrive / title
pnpm tools routes validate                 # 跑全部不变量；红 ⇒ 退出码 1
pnpm tools routes replay --id <id>         # 打印（不执行）该 route 的完整播放器命令行
pnpm tools routes emit --id <id>           # 打印该 route 的播放器输入脚本（缺省 stdout）
node tools/routes.mjs --list               # 脱离 pnpm 直接跑同一个工具
```

`replay` 打印的命令相对**仓库根**执行；`--install` 缺省取 `AMAYUI_INSTALL`，没有就用工具里登记的本机实测安装根（换机器请显式给 `--install <目录>`，⛔ 不要把机器路径写进数据）。

## 怎么改

1. 只改 `data/routes/screens.json`（它是**手写真源**）：
   - 新 route 的 `from` 必须指向已有 route 的 `id` 或 `boot`（不许成环）；
   - `steps` 是播放器输入时间线的行（`<毫秒> move x y` / `down N` / `up N` / `key N down|up` / `wheel D`），时间戳非递减；
   - `arrive` 是判据（`--expect-script`），`anchor` 是时间线 0 点。`script` 是真源，`bin` 只给人看，但**同一个 script 只许对应一个 bin**（反向也成立），工具用表内全部 route 自校验。
2. 跑 `pnpm tools routes validate`（红了先修数据，别绕过）。
3. 重生派生脚本（写后工具会回读复验）：

   ```bash
   pnpm tools routes emit --id <id> --out apps/emulator-cxx/tests/input/route-<id>.txt
   ```

   ⛔ **不要手改** `route-*.txt`：它们是 `emit` 的产物，守卫会逐字节比对（数据改了、脚本没重生 ⇒ 当场红）。
4. 跑一遍 `node --test tools/test/routes.test.mjs`（schema / from / 时间戳 / 逐字节 / 判据自洽）。
5. 想验证路径本身：照 `pnpm tools routes replay --id <id>` 打印的命令真跑一次，把**实际命令**与日期写回该 route 的 `evidence`。

## 相关落点

- 真源：`data/routes/screens.json`（本文件的同名数据）。
- 派生、入库：`apps/emulator-cxx/tests/input/route-<id>.txt`（播放器输入脚本，逐字节由 `emit` 决定）。
- 工具与守卫：`tools/routes.mjs`、`tools/test/routes.test.mjs`。
- 播放器口径：`apps/emulator-cxx/README.md` 的 `--input-script` / `--play-headless` / `--anchor-script` / `--expect-script` 一节（宿主测试装置，⛔ 不进 spec）。
