# T4 · interior 进内嵌数组元素：元素的访问器终于有类型（(k−off) % size == 0）

- id: REQ-01M6BR32V0AF7P5RJAW6BD8ENQ
- type: req
- status: done
- parent: REQ-01M6BPB07YQ73NYVMYNFPQ1X26
- order: 4
- verify: tools/test/shape-claims.assets.test.mjs#★ T4
- tags: [shape tooling]

## 一句话

`propagate` 原来只在 `interior(obj+k)` 的 `k` 恰好是 `count:1` 的内嵌子对象时换类。元素的访问器只能从
`lea ecx,[obj+K]` 进来，而 `K` 常常连成员偏移都不是（`0x69064 = 0x68EC0 + 15×0x1C`）⇒ 整族访问器没有载体
⇒ 那个类型的成员表永远是空的 —— 而 `claims` 全绿、`census --debt` 报"未声明 0"。

## 判据（做完的样子）

1. `interior(obj+k)`：`k` 落在 `{type, count:N>1}` 的内嵌数组窗口且 `(k − off) % size === 0` ⇒ **元素类型**；
   `k === 0` ⇒ 同类、内嵌子对象（`count:1`）⇒ 子类这两条**保持不变**。
2. 四条"不猜"同时钉住：越界 / 不对齐 / `kind:"ptr"` 的数组（`&arr[i]` 是**槽**地址）/ 元素 `size` 未知 ⇒ 不传播。
3. `obj` 不是 `this`（绝对地址的位移，实测 12 条）与 `unusable`（索引没解出来的 `lea`）⇒ 不传播。
4. 真语料：计时器访问器 `sub_453A60` / `sub_453AF0` / `sub_453BC0` / `sub_453BD0` 被推成 `T_FadeTimer`；
   `propagate` 的 `conflicts` 不新增；`next`（下一批候选）列出它们。
5. 守卫：`tools/test/shape-claims-guards.test.mjs#★㉝`（合成：六类命中 + 四条不猜）+
   `tools/test/shape-claims.assets.test.mjs#★ T4`（真语料）；变异：删"落在元素边界"那条判据 ⇒ 纯守卫红。

## 不做什么

- ⛔ 不按"偏移命中率"猜类（仍然只认调用点来路）。
- ⛔ 不放宽成"任意 interior 都换类"：必须落在已声明窗口内，否则只记"不传播"。
