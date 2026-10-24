# 残差第十一批：存档槽目录 / 范围 · 0x1a1 方向纠错 · STH 头

- id: REQ-01M5N79QF7TNSNX12KJCYYWM8S
- type: req
- status: done
- parent: REQ-01M5BK8QMQYK7XQG707YVMQ202
- verify: tools/test/emulator-new-save-1a1.test.mjs#★ `0x1a1`：文件存在且头合法 ⇒ 成功路径**不写** op1（保持原值）、无写调用、有还原诊断
- tags: [spec residual]

# 残差第十一批：存档槽目录 / 范围 · 0x1a1 方向纠错 · STH 头

## 一句话

闭合存档槽三条残差（槽号范围 / 目录 / 半槽）与 STH / 存档头四条格式残差；并纠正 `0x1a1` 的**方向**：它是读档，不是写档。STH 的 VERSION2 / TRAILER 收窄与正文修正留待下一批。

## 纠错

- `CORE-OPS-SAVE-WRITE`：`0x1a1` 以**只读 + 只打开已存在**方式打开 `.DAT`，读头 + 读负载；写只有 `0x19e`。打不开 ⇒ op1 ← 1（**唯一**写它的支路）；成功路径**不写** op1；读完后整池重编码。`RES-OPS-SAVE-WRITE-RECODE` 的预言方向改成"读同一个槽之后"。

## 闭合 / 收窄

- `RES-SAVE-SLOT-RANGE` / `-DIR` 关闭 → 新 `RES-SAVE-SLOT-SPACE`（本作用了哪些槽号，属数据）。
- `RES-SAVE-STH-BFSIZE` 关闭（`+40` 三处写、读侧零读者；"不要按它截断"升格为断言）。
- `RES-SAVE-MAGIC-FAMILY` / `-HEADER-VERSION-STRING` / `-HEADER-RESERVED` 关闭（族标识 = 初始化文件头两字节；版本串 = EXE 版本资源去第二字符取前四；保留区写侧清零、读侧不判）。
- `RES-SAVE-STH-SHORTREAD` 收窄（像素 / 调色板清零分配、两段头不清零）。

## 合同补充

- 槽号任意 32 位整数、不做范围检查；`%2.2d`；不会跑出存档目录；唯一边界是 256 字节路径截断。
- 存档目录 = 根 [+分隔符] + `set:SavePath`；根 = `UseAppDataFolder` 恰为 1 ? 用户本地应用数据 : 空串；`RegFilePath` 与槽无关；无搜索顺序；目录按需创建（读也建）；缺键默认 `0 / "SAVE" / ""`。
- `.DAT` 与 `.STH` 同号；`SAVEVERSION` 是一个整数（主 = trunc(v/100)、次 = 余数）；半槽来自独立写 + 先截断后写 + 非事务删拷，读侧无一致性判据。
- STH 头：BFSIZE 少 14 无读者；族标识 / 版本串 / 保留区的取值与"读侧不比"已写。

## emulator-new 落地

- `saveWrite1a1` → `saveRead1a1`：只读打开；打不开 ⇒ op1=1；头不合法 ⇒ 诊断且不写 op1；成功 ⇒ **不写** op1 + `save-restore-unmodeled` / `save-recode-unmodeled`（负载还原 / 整池重编码未建模）。
- 守卫：新增 `emulator-new-save-1a1`（3 条）；删旧"0x1a1 写文件"断言；变异 +1（326 条）。

## 仍开着

`RES-SAVE-STH-SHORTREAD`（可观察值需真机）· `RES-SAVE-STH-VERSION2` / `-TRAILER`（收窄待合并）· `RES-SAVE-SLOT-SPACE`（数据）· 命令档真实绘制 S2+。

## 判据

`pnpm test` 989/0 · `pnpm test:assets` 201/0（1 skip）· `pnpm test:mutation` 326/326 · `pnpm tools spec validate` 13/13 · `pnpm tools requirements validate` 6/6 · `pnpm typecheck` 六工程绿。

## 怎么查

`pnpm tools spec show CORE-OPS-SAVE-WRITE` · `docs/02-engine/spec-audit-2026-10.md` §28 · `tools/test/emulator-new-save-1a1.test.mjs`。
