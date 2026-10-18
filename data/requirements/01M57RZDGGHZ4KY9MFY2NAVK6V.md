# 批 S2ah · rep movs*/stos* 之后 esi/edi 未失效（实测 795 处假解释，改动已回滚待验证）

- id: REQ-01M57RZDGGHZ4KY9MFY2NAVK6V
- type: req
- status: done
- parent: REQ-01M54KEF7X560CMPMGABG2W3CW
- done_reason: 串指令失效已落盘（分层：movs*/rep ⇒ esi+edi+ecx+eax；stos* ⇒ 仅 edi）；守卫 ★access-05/★access-06；变异 122→123；守恒差额逐类可解释（解释 8439→7644 = −795 假解释，未解析基址 5834→6634 = +800 = 795+5，总数 15919 不变）；5 处 rep movsd 抽样均无 esi/edi 当对象基址
- tags: [engine, shape]

# S2ah · `rep movs*` / `rep stos*` 之后 `esi/edi` 没失效（**会造成假解释**，已实测 795 处）

## 现象（探针 `.tmp/shape/probe-rep.mjs`）

```
.text:00402700 mov  esi, ecx
.text:00402702 rep  movsd
.text:00402704 mov  edx, [esi+400h]    ⇒ acc=r abs=0x400 prob=[]   ← 被当成 obj0 相对位移
```

`rep movsd` 会把 `esi` 前进 `4·n`，之后的 `[esi+disp]` **不再是对象相对位移**；
现在的判定器落到 `default: return`，于是这些站点被算成"解释"，**凭空生成假解释**。

## 实测代价（本轮做完又回滚了，所以这里给的是实测数字）

给 `rep`/`repe`/`repne` 加 `fr.set('esi'|'edi', UNK)` 之后：

| | 改前 | 改后 |
|---|---|---|
| 守恒① | `8439 = 8439 + 0` | **`7644 = 7644 + 0`** |
| 未解析基址 | 5834 | **6634** |

⇒ **795 处"解释"其实是假解释**（它们依赖"`rep` 之后 `esi` 仍指向原对象"这个错误前提）。

## 为什么本轮回滚

795 处是**模型级的大位移**，而我没有时间逐类验证其中有多少是"真该丢掉"、有多少是
"`rep` 之后 `esi` 被重新装载（`mov esi, …`）所以仍应解释"。仓规：**没有守卫的大改动不许留在树里**。
所以：**改动已回滚**，`claims --by-type` 输出恢复到 `24C367DD1B90823F`、`test 413/413`、`mutation 122/122`。

## 要做的事（下一轮起点）

1. `shape-access` 的传移函数里加串指令档：`rep/repe/repne` + `movs*/stos*` ⇒ 显式 `fr.set('esi'|'edi', UNK)`
   （⚠ `clobberCall()` **不够**：它只清调用者保存寄存器，`esi/edi` 是粘性寄存器）；
   `movs*` 影响 `esi+edi`，`stos*` 只影响 `edi`（分开写，别一刀切）。
2. **验证 795 处的性质**：抽查 ≥5 个"改前被解释、改后落未解析"的站点，确认 `rep` 之后 `esi` 没有被重新装载
   （若被重新装载，说明我的失效过宽，需要区分 `rep movs` 与 `movs`）。
3. 守卫：`tools/test/shape-access.test.mjs` 加 `★access-05`（`rep movsd` 后 `[esi+400h]` 必须 `absDisp === null` 或 `problems` 含 `base`）；
   变异：把失效去掉必须红。
4. 只有 1~3 都做完，才把这次改动落盘；验收里要写清守恒① 从 8439 降到多少、差额逐类解释。

## 复现

```powershell
node .tmp/shape/probe-rep.mjs        # 现状：absDisp=0x400（错）
pnpm tools shape claims data/shape/carriers.json | rg "守恒①"
```

## 抽样验证（3/5，本轮做的）

| `rep movsd` 位置 | 之后紧跟的指令 | 判断 |
|---|---|---|
| `0x40B227` | `lea ecx,[ebx+0A609Ch]` / `call`（用的是 **ebx**，不是 esi/edi） | **失效是正确的**：此后没有任何 `[esi+disp]`/`[edi+disp]` 当对象相对位移 |
| `0x40BE6E` | `test [ebp+var_2E8],4` … `lea ecx,[ebp+var_4]` | 同上：全是栈/其它寄存器 |
| `0x40C598` | `mov [ebp+var_4],0FFFFFFFFh` … `pop ecx/edi`（SEH 收尾） | 同上 |

⇒ 三个样本都**没有**"`rep` 之后把 `esi/edi` 当对象基址"的用法 ⇒ 失效不会误伤合法解释，
795 处更可能是**真·假解释**（此前它们被当成 obj0 相对位移，其实 `esi` 已前进）。
仍欠：再抽 2 个样本 + 写 `★access-05` 守卫 + 变异（下一轮）。
