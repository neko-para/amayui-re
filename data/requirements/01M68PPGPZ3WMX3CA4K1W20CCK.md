# 池族缺口：字符串族初值 · 基址用途 · 存档 raw 地址 · 0x32f

- id: REQ-01M68PPGPZ3WMX3CA4K1W20CCK
- type: req
- status: open
- parent: REQ-01M674XHSX2HSFWSKDWZV2C63D
- tags: [spec residual]

## 目标

收敛池族在批 2 留下的缺口。

## 具体项

1. **池初值**：字符串族的构造 / 元素追加删除（`sub_433310` / `sub_42BF60`，2×150 行）；其余五族的初值清零已证（8 个 `memset 0` 站点）。对应残差 `RES-POOL-INIT-VALUES`（第 7 项保留的窄 `unknown`）。
2. **池基址槽的 23 个使用函数**（`POOL-BASE-USES-AUDIT`）：只做过形态普查（135 处全 `[reg+off]`、索引 / lea / ds 均 0），未逐条读；按 R3 另立。
3. **`0x32f` 的容器身份**（`sub_49A150`）：接收者是固定对象，不是池句柄容器；`CORE-CONTAINERS-ELEMENT` 的对应句已订正，但该对象的身份未定。
4. **存档把 int-ref 池的 raw 地址写进存档**（写 `0x40D9FA` / 读 `sub_410160`）：与 `65-save` 的"不持久化任何堆地址"**冲突**，跨章裁决。
5. **翻页表挂载**：`sub_41AA50` 的基址交换是否该挂进 `SPC-POOL-MEMFLIP`（跨规则）。
