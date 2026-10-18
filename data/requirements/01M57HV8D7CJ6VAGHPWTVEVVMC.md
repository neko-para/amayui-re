# 批 S2ad · G4 拿类型相对偏移去查绝对地址（真 bug：判定器逼着模型退回顶层写法）

- id: REQ-01M57HV8D7CJ6VAGHPWTVEVVMC
- type: req
- status: done
- parent: REQ-01M54KEF7X560CMPMGABG2W3CW
- done_reason: G4 的 end 改为绝对地址：absBase = ABS.get(tid:offN)，endAbs = absBase + et.size*count，refs(endAbs)；ABS 不可得时退回相对偏移并记 note（明说是下界判断）。实测门禁：claims exit 0（8411 = 8411 + 0，守恒②③④ 不变）、test 409/409、mutation 119/119
- tags: [engine, shape]

# S2ad · G4 拿**类型相对偏移**去查绝对地址（真 bug）—— 已登记，待修

## 现象（batch-22 现场撞出来）

把 `T_Command+0xAAA18..0xAAB44` 写成内嵌子对象 `T_inihdr{size:0x12C}`（结构上更贴事实）后，当场 3 条红，其中**两条是假的**：

```
G4 T_inihdr+0x0：数组末尾 0x4 零引用 ⇒ count=… 无依据
G4 T_inihdr+0x4：数组末尾 0x8 零引用
```

`0x4`/`0x8` 是**类型内相对偏移**；语料里真正该被查到的是**绝对地址** `0xAAA1C`/`0xAAA20`（分别被引用 **2 / 19** 次）。
⇒ G4 的 `refs(end)` 应该用绝对地址，模块里已有现成的 `const ABS = new Map()`（类型 id → 该类型在 obj0 里的绝对基址，或至少 `T_Command` 用 `offN` 直接当绝对偏移的等价关系）。

## 代价

子代理因此**退回了顶层写法**（能过 G4 的那种），而不是更贴结构的子对象写法 —— 也就是说：**判定器的 bug 直接改变了模型长什么样**。这类"工具逼着模型说谎"的情形必须记下来（和"门禁全绿不能给判类背书"是同一条纪律）。

## 修法（两案）

1. **G4 用绝对地址**：`refs(endAbs)`，其中 `endAbs = ABS(tid) + m.offN + et.size * count`；`ABS` 缺失时退回现值并记 note（"相对偏移查引用，下界"）。
2. 或者让 `refs()` 同时接受"相对偏移"并**只在 `tid` 是 obj0 时**才等价（`T_Command` 就是 obj0，所以现状对 `T_Command` 的成员是对的，对**内嵌子类型**的成员是错的）。

## 守卫

合成用例：一个内嵌子对象类型 `TX{size:0x20}` 里放 `{off:0, type:T_el32, count:8}`，父类型 `T0` 在绝对 `0x1000` 处内嵌 `TX`，
语料里只让 `0x1000+0x20=0x1020` 出现（不出现 `0x20`）⇒ 现行代码会红，修好后必须**不红**。再加一条变异。

## 复现

`.tmp/shape/batch-22/cmdtail/report.md` §工具发现① · `logs/counterfactual-cur6.txt`（另一条反事实：把 0x5D800 做成 8 字节数组后 `--by-type` 仍是 1 处，只是残差搬家 + 假说法）。
