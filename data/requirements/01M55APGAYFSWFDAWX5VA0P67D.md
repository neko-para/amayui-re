# 批 S2d · 通用基类 BaseObject 与全局 RemoteDebug（[this+4] 那一族：26 个类共享的基类前缀 + RTTI 名齐）

- id: REQ-01M55APGAYFSWFDAWX5VA0P67D
- type: req
- status: done
- parent: REQ-01M54KEF7X560CMPMGABG2W3CW
- verify: tools/test/shape-claims-guards.test.mjs#★⑱ 适配器类型
- tags: [engine shape]

# S2d · 通用基类 `BaseObject` 与全局 `RemoteDebug`（`[this+4]` 那一族）

`sub_4034C0`/`sub_4034D0`/`sub_4034E0` 被 `T0` 与内嵌子对象 `T_1ee8`/`T_4ed10`/`T_a609c` 当 `this` 调用，
一度看来像"通用转发助手 / 冲突"。**真相是基类 + 全局调试输出对象** —— 而且这两者**都有 RTTI 名**。

## 证据（锚只有 EA）

| 事实 | EA | 逐字指令 |
|---|---|---|
| 基类 vtable | 0x4034a2 | `mov dword ptr [eax], offset ??_7BaseObject@@6B@` |
| `+4` = `&RemoteDebug` | 0x4034a8 | `mov dword ptr [eax+4], offset dword_55BF70` |
| 基类构造器 | 0x4034a0 | `sub_4034A0`（**26 个调用方**，全是各类构造器，含 0x415640 `sub_415640`=Command） |
| 转发 1（追加字符串） | 0x4034c3 / 0x4034c7 | `mov ecx,[ecx+4]` + `jmp sub_497620` |
| 转发 2（格式化） | 0x4034d3 / 0x4034d7 | `mov ecx,[ecx+4]` + `jmp sub_4976A0` |
| 转发 3（**虚调用**） | 0x4034e3..0x4034ec | `mov ecx,[ecx+4]` · `mov eax,[ecx]` · `mov eax,[eax+8]` · `jmp eax` |
| 全局对象 | `seg002:0055BF70` | `_DWORD dword_55BF70[266]`（0x428 字节） |
| **它的 RTTI 名** | 0x4975a3 | `mov dword ptr [esi], offset ??_7RemoteDebug@@6B@`（构造器 `sub_497570`） |
| 它的标志位 | 0x4975a9 | `mov [esi+420h], eax` |
| 静态初始化（MSVC 形态） | 0x51c140 / 0x51c145 / 0x51c14f | `mov ecx, offset dword_55BF70` + `call sub_497570` + `call _atexit` |
| `_atexit` 注册的析构器 | 0x51c2d0 | `mov ecx, offset dword_55BF70` + `jmp sub_4975D0` |
| `+4` 是文件句柄 | 0x438ca1 / 0x438ca7 | `call CreateFileA` + `mov [esi+4], eax`（文件名 `"Error.log"`） |
| 读回调/标志 | `sub_497620` | `[this+0x410]` 当回调、`[this+0x420]` 当标志 |

## 判据 / 处置

- `T_BaseObject`（`adapter:true` = **偏移 0 的基类前缀**）：`+0x0` vtable · `+0x4` ptr→`T_RemoteDebug`；
  三个转发器载体（c86/c87/c88）指向它。
- `T_RemoteDebug`（`size` = 0x428，来自全局数组 266 dword）：`+0x0`/`+0x4`/`+0x410` 三个 ptr + `+0x420` 标志；
  `sub_438CC0`/`sub_497620`/`sub_4976A0` 的载体（c89/c90/c91）指向它。
- 传播 **"声明 > 推断"**：被调方有载体时，推断出的另一类只记 note（"调用方类是它的子类"），不记冲突。
- 调用点对账多一档 🔗；适配器三条硬要求：不许 `size` · 不许 `complete` · 至少一个成员。

## 诚实项

`T_RemoteDebug+0x410`（回调）/`+0x420`（标志）的**语义**只有 `.c` 的推断，未逐字证明用途；`size=0x428`
来自数据段数组长度而不是分配点；vtable 槽 `+8` 的具体目标未解（间接调用，属闭合度缺口）。

## 复现

`pnpm tools shape claims data/shape/carriers.json`（`T_BaseObject`/`T_RemoteDebug`）· `pnpm tools shape calls`
（0 不一致 · 4 处 🔗）· `pnpm tools mutate check --match 适配器`。
