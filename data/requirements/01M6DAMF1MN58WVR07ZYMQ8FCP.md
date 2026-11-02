# SN0000 足迹残差的逐条 triage 与收口（122 条路径内）

- id: REQ-01M6DAMF1MN58WVR07ZYMQ8FCP
- type: req
- status: done
- parent: REQ-01M674XHSX2HSFWSKDWZV2C63D
- done_reason: 122 条路径内残差已全部 triage 并逐条裁决；8 批（颜色/场景/命名/投影/audio/render-text/vm-timing/b-f）已写进 spec/audit；其余 narrowed / real-gap 均在 audit 带具体 next，由 audit 跟踪；无未裁决项，门禁全绿。
- tags: [spec, residual, sn0000]

## 范围

`route sn0000` 的足迹（`apps/emulator-cxx` 跑到 SN0000.BIN id 116；`--settle-steps 2000000` 覆盖进入后的夕阳稳定期）。
路径内 **122 条**残差 / 57 条规则。9 个只读 subagent 按规则簇逐条 triage；报告原文在
`docs/02-engine/sn0000-triage/`，审计记录在 `docs/02-engine/spec-audit-2026-11.md` §九。

## 已落（首批）

- 删 7 条残差（正文已有同一义务的答案 / 闭集已证）：`RES-OPS-195-COMPARE-SUBROUTINE` ·
  `RES-TEXT-CELL-REVEAL-SWITCH-CELL` · `RES-TEXT-CELL-REVEAL-REACH` · `RES-SCENE3D-DIRTY-CONSUMERS` ·
  `RES-SCENE3D-XFORM-DIRTY2` · `RES-LEA-ALIAS-LIFETIME` · `RES-ADV-TEXT-GATE-INIT`。
- 第 8 条 `RES-OPS-STATUS-CELL-ROLE` 首批删了，复审指出 E 的写者/前置关系无人管 ⇒ 已恢复为窄残差（所以净删 7）。
- `62-adv-text.md` 门条补成合同（初值 0 / 只有 `0x1bb` 两支写 / 无复位路径 / 第三类读者按最高位判）。
- 路径内 122 → 116；`pnpm tools spec validate` 绿。

## 第二批（2026-11-02，颜色字节序 + state 三处）

- **颜色字节序已裁决**：脚本侧 `0xRRGGBB`；`0x76`/`0x77` 字节倒序写进字体对象，存储侧 = COLORREF BGR（低字节红）；`0x82` 同反转、`0x204` 直接作为 SetTextColor 的 COLORREF。`60-ops-render.md:108` 的「低字节为蓝」已改成「低字节红」；`60-ops-text.md:102` 加交叉引用；`RES-RENDER-TEXT-SURFACE-FORMAT` 只留 32 位表面通道归属。颜色子单已 done。
- **state 三处**：`0x1ad` = 当前帧号副本（层号定为帧号别名）；`0x1cf`/`0x1bf` 与 `0x24e` 不是同一格；布局 1 的第三对 = 返回点表（版本 1=2/3 恢复重建返回栈、表项 +3；布局 1 不重建）。
- 删 `RES-OPS-1BF-MODE-WORD-ALIAS` · `RES-LAYER-FRAME-REL`；`RES-OPS-SAVE-VERSION-AE-LAYOUT1` ④ 收口；`RES-OPS-LAYER-CELL-ROLE` 收窄。
- 路径内 116 → **114**。

## 第三批（2026-11-02，场景三处）

- `0x220`：G1/G2 从「两个整数属性」改成**平移窗延迟 / 持续时间**（G0 窗起点、G3 重算标志；持续时间 ≤ 0 整段不插值）；删 `RES-SCENE3D-XFORM-INTFIELDS`。
- `0x224`：容器就是**场景命令表**；正文明确五条命令记录指令 + `0x224` 是唯一由脚本发起的整表清空、不释放纹理 / 影片；删 `RES-SCENE-CONTAINER-224-FILLER`；`:66` 的「没有任何脚本指令清命令表」改掉。
- `0x32a`：对象表改成「与 `0x32b` 同一张 1000 槽表」；创建者 `0x329`；残差 `RES-SCENE-EFFECT-OBJECT` 收窄（雨 / 雪 / 雾是旧说）；`0x329` 的建对象语义进 `GAP-OPS-0300-03FF` 的 `RES-GAP-329-MESH-OBJECT`。
- `0x203` 顺带：该格随 740 字节记录整块进存档；残差 `RES-SCENE-UNKNOWN30` 收窄到「直接遍历结点 / 平台资源接口」。
- 路径内 114 → **113**（删 2 条 + 新增 1 条 `RES-SCENE3D-XFORM-WINDOW-VALUES`；场景子单已 done）。

## 第四批（2026-11-02，跨规则命名）

- 统一名 = **「强制完成」位**（独立的一格，不是状态字里的某一位；旧称「中止位」只作别名）。
- 三处规则说法对齐：`CORE-OPS-ENGINE-CELL-238` / `CORE-TIMING-WINDOW` / `CORE-OPS-SCENE-CLEAR`；`wait` / `list` / `texture` / `render` / `bindings` 的「档 / 中止位」一并统一。
- `RES-ENGINE-CELL-238-READER` 收窄为「A 格写者相位未枚举；读者已定」。
- 路径内仍 **113**（无残差增减）；命名子单已 done。

## 第五批（2026-11-02，投影偏移借用期）

- 借用期事实已定：**另一条应用路径**借用字体对象的两个投影偏移（借用 / 归还点已定位），借用期内 **5 个直接绘制入口调用点**读到样式值。
- `0x204` 正文改成「其它路径在另一条应用路径的借用期读到样式值」；残差 `RES-RENDER-TEXT-PROJECTION` 改写为「本条读 (1,1) 已定；其它路径确实读到样式值；未定：是否参与字形格/推进量、归还是否覆盖所有出口」。
- **不新立规则**（无独立合同 / 守卫，避免空规则）；投影子单已 done；路径内仍 **113**。
- **规则面独立复审**已做（只读 `data/spec/`）：抓出 7 条（门条的两章冲突 / 漏 `0x6f` / 按键重画的触发与效果 / E 的写者义务 / 初值可证伪 / 两处残差措辞），已全部修复并复验；见审计记录 §九。

## 第七批（2026-11-02，render/text 合同）

- `0x204`：新增**回退字体**合同（字形码 < 0x100 ⇒ 建临时字体 / 选入 / 状态 1；≥ 0x100 ⇒ 只释放）；**特殊字形**补「F043 一次 / F042 两次；矩形 = 字框 + 投影横偏移 + 推进量」；**取尺寸失败**与加锁失败同一出口、取矩形失败未定位；步骤 1 标明「按算出的格宽推进」开关。
- `0x1d2`：补**脏槽写者 = 行记录收尾**（置 1；下一条同下标追加时清 0）；删 `RES-TEXT-RECORD-REGISTER-DIRTY`。
- 5 条残差收窄：`FONT-FALLBACK` / `SPECIAL` / `FONT-SHARED-METRICS` / `LOCK` / `ADVANCE`。
- 路径内 110 → **109**。

## 第八批（2026-11-02，vm/timing）

- `60-ops-wait.md`：待办清除**两处**（等待期按延时 / 跳过·取消立即；前者先于谓词 B）、缺省构造 0、待办参数 6 ⇒ 挡门；第 2 位**只被等待谓词读**、分离实验静态不可构造。
- `70-vm.md`：谓词 B 生效参数 = 待办 ? 待办参数 : 最近一次真正启动的选项字；第 14 位前置补「主循环模式字第 0 位为 0」；第 21 位 = 窗口重画抑制位（不在门字服务链，泵收场清 0）；退出型失败三格不可观察。
- 残差：`PENDING` / `BIT2` / `FLAGBITS` / `NEST` 收窄；删 `RES-VM-PUMP-FAIL-CLEAN`。
- 路径内 109 → **108**。

## 收口边界

其余 narrowed / real-gap 残差均已在 `data/spec/audit.json` 带具体 `next`，由 audit 跟踪；本单不再承载。

## 收口判据

- 每条路径内残差都有裁决：删（正文已答）/ 改写（窄 `next`）/ 保持（`real-gap` + 具体 `next`）/ 转通道；
- 每批改动过 `pnpm tools spec validate` + 独立复审（规则面）；
- `node tools/spec.mjs --residuals --footprint .cache/footprint/sn0000.jsonl --scope path` 的清单与审计记录一一对应；
- 除 `RES-RENDER-TEXT-ALIGN-REFERENCE` 外，不把"真机看看"当 `next`。
