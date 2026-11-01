# T2 · 未解析基址按「来路」分组：返回值 / 栈出参 / 表查询 / interior 内嵌数组 / 参数未声明

- id: REQ-01M6BR3B6M9X5XNZPRX2FFAEKG
- type: req
- status: done
- parent: REQ-01M6BPB07YQ73NYVMYNFPQ1X26
- order: 2
- verify: tools/test/shape-claims.assets.test.mjs#★ T2
- tags: [shape tooling]

## 一句话

`claims` 的守恒③ 里，**未解析基址**是一整档总数（现跑看数），只按**基址寄存器**分组（`eax×…` / `edi×…`）——
不可派活。本单把它按**来路形态**分组：`call` 返回值 / 栈出参 / 表查询结果 / `interior` 落进内嵌数组 /
参数未声明（另加间接调用目标未知），每档给样例 EA 与调用方。

## 判据（做完的样子）

1. `shape claims`（与 `shape access`）的未解析基址按上述五档报数，每档带样例 EA；五档之和 == 守恒③ 的
   `unresolvedBase`（不新增桶、不改名后重复计数）。
2. 分组是**纯机械**的"来路形态"，⛔ **不判类型**：仍不许按"偏移命中率"猜类。
3. 至少两个真实调用点能被正确归类（`call` 返回值那档与 `interior` 内嵌数组那档各一），并写进守卫。
4. 与 `shape coverage` 的"基址未解析"使用**同一份分档**（⛔ 两份实现必然差数）。
5. 守卫：`pure`（合成 lst + 注入 roots/observed）+ `assets`（真语料上五档之和 == 守恒③）。
   变异：把某一档并回"其它" ⇒ 守卫红。

## 不做什么

- ⛔ 不为了减少"未解析"而编 carrier / local 载体（那会把不可判变成假解释）。
- ⛔ 不覆盖栈上传参 ABI 与间接调用目标（那需要另一套证据，属别的单）。
