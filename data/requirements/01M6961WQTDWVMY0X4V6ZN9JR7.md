# 残差可达面基建：routes 动态/静态足迹 dump + spec residuals --footprint join（路径内 / 路径外 / 判不了）

- id: REQ-01M6961WQTDWVMY0X4V6ZN9JR7
- type: req
- status: doing
- parent: REQ-01M5851Z51V1MTSWH414M7WN5Q
- tags: [spec residual, tooling]

## 背景（为什么需要）

残差目前按**通道**（vm/state/render/text…）统计，但通道只是报表维度，**不能当执行顺序**；按"缺陷"排序也不行（从表现反推是欠定的，容易把错误实现当参照）。缺一条机械判据来回答：**某条残差是否会被本次里程碑（boot → TITLE → OPTION → SN0000）的输入触发**。

已有可复用的两半：`data/routes/screens.json`（boot 起的输入时间线 + `arrive.script`，`pnpm tools routes`）+ `data/spec` 的规则 / 残差（`pnpm tools spec residuals`）。缺的是"**足迹**"这一层：把一次 route 实际执行到的 opcode / 服务过的位 / 读写的格 dump 出来，再和规则 join。

## 目标（三件，可独立验收）

1. **动态足迹 dump**：`apps/emulator-cxx` 加一个**只读诊断口**（env / flag，缺省关），跑一条 route 时输出 JSONL：
   - `op`：执行到的 opcode + 次数；
   - `gate`：服务链上被服务过的位号 + 次数；
   - `cell`：读 / 写的引擎格（以 `storage` 指针或偏移为键，⛔ 不写死中文名）+ 方向 + 次数。
   要求**确定性**（同 route 同输出，时间戳等易变项不进文件）与**不改行为**（log-only）。
2. **静态足迹**：对 route 的 `arrive.script` 及它装载到的脚本做解码，输出"出现过的 opcode 集合"。
   ★ 它是**上界**（可能出现但没执行），动态足迹是**下界**（一定执行了）；两者都要，判据见下。
3. **join（`pnpm tools spec residuals --footprint <file>`）**：把每条残差按**触发者**分三桶，输出清单 + 计数（`--json` 可机读）：
   - **路径内**：触发者在动态足迹里，或"触发者已知 + 其写者闭包落在足迹内"；
   - **路径外**：触发者已定位、但不在动态足迹、且静态足迹里也没有它（或它的写者闭包为空）；
   - **判不了**：连"谁写这个格 / 这个位由谁置起"都还没取证（⇒ 先做写者闭包，再重判）。

## 判据（触发者怎么定，机械可重算）

- **opcode 类残差**：规则 `covers.ops`（没有就取标题里的 `0x…`）∩ 动态足迹。命中 ⇒ 路径内；不命中但静态足迹命中 ⇒ **判不了**（数据依赖分支）；两者都不命中 ⇒ 路径外。
- **位 / 格类残差**：残差的 `what` / `next` 里点名的位号 / 偏移 → 在动态足迹的 `gate` / `cell` 里查；查不到则做**写者闭包**（`disasm-fn callers` / 语料写点）：闭包里没有任何足迹内 opcode ⇒ 路径外。
- **外部 / 资产类残差**（真机、IDA 导出、语料外模块）⇒ 一律"路径外"，不参与排序。

★ **"路径外" 不落库**：它是**派生分类**（每次按当前足迹重算），⛔ 不改残差的 `kind`、⛔ 不新增 schema 字段；足迹文件本身是生成物（落 `.cache/` 或 `.tmp/`，不入库）。

## 非目标

- 不做像素 oracle；不追求"全引擎覆盖"；⛔ 不把足迹当规范（足迹只是"这次跑到了什么"，不是"引擎是什么"）。
- 不因为某条残差"路径外"就 `discarded`（我们没证明它不可达，只是本次不涉及）。
- 不引入新台账 / 新数据格式真源；足迹是派生视图。

## 验收判据

1. `emulator-cxx --dump-footprint <out.jsonl> --route <id>`（或 env 口）跑 TITLE / OPTION / SN0000 三条 route 各产出一份足迹；**同 route 两次运行逐字节相同**。
2. `pnpm tools spec residuals --footprint <out.jsonl>` 对每条残差给出**三桶之一**（不允许"未分类"），并给计数；`--json` 结构与 `--channel` 输出同族。
3. 守卫（`@env pure`）：
   - 合成足迹 → 三桶判定的判别力（把某 opcode 从足迹象里删掉 ⇒ 该残差从"路径内"变"路径外"；写者闭包为空 ⇒ 路径外）；
   - 每条残差都必须落桶（遍历全库 ⇒ 未分类 = 0）；
   - dump 的确定性（同一输入两次输出相同）；
   - `mutate-check` 一条变异（例如把"静态足迹命中也算路径外"写反 ⇒ 守卫红）。
4. `pnpm test` / `spec validate` / `requirements validate` 全绿；不触碰 `data/spec` 的 schema。

## 分期

- **P1（先做）**：动态 dump（opcode + gate + cell）+ opcode 类 join + 三桶输出 + 守卫。够回答"render/text 的 97 条 A 里哪些真的在路径上"。
- **P2**：位 / 格类 join（写者闭包）；静态足迹解码（用 `packages/age-format` 的指令表，按脚本字节机械枚举）。
- **P3**：把三桶结果接进 `spec residuals --channel` 的输出（多一列 `scope=path|offpath|unknown`），以及 route 扩展后的重算说明。

## 怎么查 / 怎么改

- 路由真源：`pnpm tools routes describe` / `list`；足迹文件是生成物。
- spec 侧的规则 / 残差：`pnpm tools spec describe` / `show` / `residuals`。
- 本单不新增数据真源：`data/routes/screens.json` 与 `data/spec/*` 各自仍是唯一真源，足迹只做 join。
