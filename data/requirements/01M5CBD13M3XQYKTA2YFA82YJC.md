# shape claims 的 assets 守卫曾 5 条红：类型改名只改一半 / 守恒② 少 pointeeAmbiguous 桶 / 下钻夹具落在索引站点 / 全局载体没入桶 / T_Font 两条多余视图

- id: REQ-01M5CBD13M3XQYKTA2YFA82YJC
- type: bug
- status: done
- parent: REQ-01M52F3ASMGWQXX64JSA18MP2N
- order: 58
- verify: tools/test/shape-claims.assets.test.mjs#★ 入库的 shape 模型（data/shape/）逐族全绿 + 守恒闭合
- repro: tools/test/shape-claims.assets.test.mjs#★ 二级对象下钻：三档 + 不透明单列（⛔ 不许静默通过）
- severity: S2
- tags: [engine, shape, guard]

## 现象（HEAD 上 `pnpm test:assets` 的 5 条红）

`tools/test/shape-claims.assets.test.mjs` 6 个用例里 5 条红（不是语料问题：同一台机器上其余 shape assets 全绿）：

1. `★ 改坏一处 ⇒ 红，而且**红的理由要对**` —— `TypeError: Cannot read properties of undefined (reading 'members')`
2. `★ 二级对象下钻：三档 + 不透明单列` —— 同一个 `TypeError`
3. `★ 层级 ≥3 的访问必须被单列报出` —— `实测 0`（函数里明明有二级访问）
4. `★ 第二批函数（6 个）：残差如实报出` —— 6 条 `类型 T_Command* 未定义`
5. `★ 入库的 shape 模型（data/shape/）逐族全绿 + 守恒闭合` —— 守恒② `1370 !== 1139`

## 根因（四处，全部是"判定器改了、夹具/断言没跟上"，不是 shape 数据错）

1. **样本类型改名只改了一半**：`88f90c3` 把 `data/shape/types.json` 的 `T0` 改成 `T_Command`（同时把测试里的夹具引用也改成 `T_Command`），
   但 `tools/lib/shape-claims.mjs` 的 `sampleModel()` 仍返回 `T0` ⇒ 夹具引用了不存在的类型。
   ★ 反方向（把 `sampleModel()` 改名成 `T_Command`）**不行**：G14 命名棘轮会要求 `note` 把名字绑到 `??_7Command@@6B@` 的 vtable 写入点。
   ⇒ 夹具保留**无名占位** `T0`（与 G14 注释里"`T1`/`T2` 这类无名占位不冒充 RTTI"同一口径）。
2. **守恒② 少一桶**：`88f90c3` 起判定器把 `pointeeAmbiguous`（"基址/索引分不清"）计入守恒②，但测试断言没加这一项
   （`carriers.json` 实测 `1370 = 1139 + 231` 恰好差这一桶）。
3. **下钻夹具的长在索引站点上**：夹具用 `sub_42AEA0` 的 `0x42af25 mov eax,[eax+edx*4]`，`edx` 是运行期算出来的 ⇒
   判定器**正确地**把它归进"基址/索引分不清"档 ⇒ 四档夹具永远碰不到 `G5'`（假绿风险：改坏判定器也不会红）。
   ⇒ 夹具换到 `sub_403200` 的 `0x403208 mov eax,[eax+18h]`（常量二级偏移 0x18）。
4. **全局载体没入任何桶**：`carriers.json` 的 `g01`（`kind:"global"`，`ds:dword_55E1BC → T_Command*`）在 G13 里判、**不进 `perCarrier`**；
   测试的"每个载体都要被检查到"原先只排除 `local` ⇒ 把"在 G13 里已被检查"误读成"静默漏"。
5. **`T_Font` 两条 `views{elementDelta:1}` 是多余声明**：`T_el32[4096]`（元素 4 字节）的窗口已经罩住 `+0x18f2c` / `+0x3561c`
   —— G9 判它们 `redundant`（"范围已被 delta 0 覆盖"）⇒ 按测试的判据永远无法"被观察到"。
   ⇒ **删声明**（不是补观察）：它们不提供新信息；`+0x18f2c` 就是本数组的元素 1。

## 复现 / 收口判据

```bash
node --test --test-isolation=none tools/test/shape-claims.assets.test.mjs   # 6/6 绿
pnpm tools shape claims data/shape/carriers.json                           # 仍「全部义务通过」
node tools/mutate-check.mjs --match shape                                   # 49/49 条仍按预期变红
```

★ 同机 `pnpm test:assets` 仍有 **2 条先前就红的环境项**（corpus 的两条本机写入口按 Windows 盘符根解析）
—— 与本单无关，见交接单 `REQ-01M5BK8QMQYK7XQG707YVMQ202`。spec-isa 不再要求 `_extracted`（历史汉化临时解压，不是安装数据）。

## 为什么值得登记（而不是"顺手修了"）

两个 `status=done` 的节点拿这条 assets 守卫当 `verify`（S1d / S2f）—— 守卫红了**两天**而 `pnpm test`（pure 档）全绿，
没有任何东西会响 ⇒ "done 节点的 verify 名副其实"在 assets 档上当时**没有防线**。
