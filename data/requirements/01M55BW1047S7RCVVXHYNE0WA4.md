# 批 S2g · 类型命名棘轮：T_<RTTI 名> 必须带可解析的 vtable 证据（名字是可证伪断言）

- id: REQ-01M55BW1047S7RCVVXHYNE0WA4
- type: req
- status: done
- parent: REQ-01M54KEF7X560CMPMGABG2W3CW
- verify: tools/test/shape-claims-guards.test.mjs#★㉕
- tags: [engine shape]

# S2g · 类型命名棘轮（G14）：`T_<RTTI 名>` 必须带**可解析**的 vtable 证据 —— 已落

## 立起来的判据（`shape claims` 的 G14）

1. 把 id 的 `T_` 之后那段（`Scene`）拿去语料找 `??_7Scene@@6B@`：
   **找不到** ⇒ 这是**结构名**（`T_Queue_int` / `T_Scene_460` / `T_FileDB_3028`），不要求 RTTI 证据；
   **找得到** ⇒ 这是**RTTI 声明** ⇒ 必须有一条 `note` 把名字绑到那次 vtable 写入上。
2. `note` 里的 `??_7<Name>@@` 必须与 id 对得上（⛔ 不许 `T_Scene` 绑 `??_7Font@@`）。
3. `note` 里**至少一个 EA** 必须落在**写 vtable 的那条指令**上 —— 该行要逐字含 ``offset ??_7<Name>@@``。

★ 为什么第 3 条要求"至少一个"而不是"第一个"：note 里常同时给 ctor / dtor / `_atexit` 的 EA，
要求第一个会逼人把有用的地址删掉。

## 一立就抓到的真缺陷

`T_RemoteDebug` 的 note 给的是 `0x497570` —— 那一行是 `; Attributes: bp-based frame`（**函数起始**），
真正的 vtable 写入是 `0x4975A3`（`mov dword ptr [esi], offset ??_7RemoteDebug@@6B@`；dtor 路径另写 `0x4975F6`）。
⇒ note 已改成"vtable 写入 0x4975a3；ctor 0x497570 · dtor 另写 0x4975f6 · _atexit 0x51c14f"。
其余 20 个 RTTI 类名一次通过；另有 17 个结构名按规则不要求 RTTI 证据。

## 落点

- `tools/lib/shape-claims.mjs`：G14 块（在 G13 之后、适配器检查之前）
- `tools/test/shape-claims-guards.test.mjs`：`★㉕`（4 个反例：过期 EA / 无 note / 绑错名字 / 结构名放行）
- `tools/mutate-check.mjs`：`G14 命名棘轮整条失效`（112/112 全红）
- `docs/02-engine/shape-census.md`：「命名棘轮（G14）：`T_<Name>` 是一个可证伪的断言」
- `data/shape/types.json`：`T_RemoteDebug` 的 note 修到真实写入点

## 复现

`pnpm tools shape claims data/shape/carriers.json` ⇒ 提示行
`G14 命名棘轮：21 个 RTTI 类名已绑到 vtable 写入点（另有 17 个结构名不要求 RTTI 证据）`。
