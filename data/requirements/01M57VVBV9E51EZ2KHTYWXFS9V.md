# 批 S1o · calls 的覆盖扩展：栈上传参 ABI / 间接调用 / local 载体当实参（每条先证明约定）

- id: REQ-01M57VVBV9E51EZ2KHTYWXFS9V
- type: req
- status: open
- parent: REQ-01M52F3ASMGWQXX64JSA18MP2N
- order: 56
- tags: [engine shape]

## 目标

`pnpm tools shape calls` 现在覆盖的是"**寄存器**传对象"（`ecx`/`esi`/`edi` 等）。三种真实存在的情形它
**明确不覆盖**，本单把它们逐条做出来，且每一条都要**先证明约定**（⛔ 不许猜 ABI）。

## 三条（各自独立，可分开收口）

| # | 情形 | 为什么难 | 收口要多什么 |
|---|---|---|---|
| 1 | **栈上传参**（`push` 的顺序 ↔ 参数位） | 参数位与 `push` 顺序的关系是**调用约定**，`__cdecl`/`__stdcall`/`__thiscall` 不同 | 一份**约定证据**（`.c` 侧签名 + 至少两个调用点的逐字对照），再让 `calls` 按参数位对账 |
| 2 | **间接调用**（`call eax` / `call [mem]`） | 目标未知（L0 里本来就挡着，见 S1j） | 先有"目标集"才谈得上对账 ⇒ 依赖 S1l 的产出或 vtable 证据 |
| 3 | **`local` 载体当实参来源** | 调用点上实参来自一个"由局部类型载体断言过"的寄存器 | 现在 `calls` 只解析 `this` / 内指向量 / 指针路径 ⇒ 把 `kind:"local"` 也纳入来源解析 |

## 判据（每条）

* 合成 `.lst`：同一份调用在"覆盖前"判 `⚪ 给不出`、"覆盖后"判 `same`；**再改一处**（换个参数位 / 换个载体类型）
  ⇒ 必须翻成 `mismatch`。⛔ 只会判 `same` 的实现在这一条上过不去。
* 真语料：跑 `pnpm tools shape calls`，新覆盖的那一类**不许凭空造出 `mismatch`**
  （出现就当场查：是约定错还是声明错）。
* 变异清单各一条（把新覆盖拆掉 ⇒ 对应守卫当场红）。

## 反模式

* ⛔ 用"`push` 的先后"直接当参数位（`__stdcall` 是**反向**压栈，`__fastcall` 前两个在寄存器里）；
* ⛔ 把间接调用当"目标未知所以不用对账"（那样它永远进不了 `calls`，也就永远没人做）；
* ⛔ 给 `local` 载体造一个"看起来像类型"的名字（那是假类型，见 S1m 的口径）。

## 怎么查

```bash
pnpm tools shape calls          # 现在是哪三档、哪些落在"给不出"
pnpm tools shape regions <sym> <reg>   # local 载体要逐字引用它的活窗口
pnpm tools shape describe       # `calls` 的三步判据与"不覆盖"清单（真源）
```
