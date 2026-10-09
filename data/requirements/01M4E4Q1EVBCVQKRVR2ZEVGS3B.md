# codec key 的真实来源/取值未取证（0x6 的前置条件要求它非 0）

- id: REQ-01M4E4Q1EVBCVQKRVR2ZEVGS3B
- type: req
- status: open
- parent: REQ-01M4AGPP1T9A60HQYW3GX9W592

## 事实（本轮从逐字读出来的）

`0x6 load-frame` 的前置条件里有 `|| !*(_DWORD *)(this + 388236)` —— 而 `Engine+0x5EC8C`（= 388236）
就是**编解码 key**（另一条 `ROL(Engine[0x5EC90], 11) != key` 是它们之间的**自洽检查**：
`encZero = ENC(key,0) = ROL(key,21)` ⇒ `ROL(encZero,11) = key`，对**任何** key 恒成立）。
⇒ **引擎真的要求 key 非 0**（key = 0 时它抛 `Command_Exit_Exception`）。

## 与本层现状的冲突

`apps/emulator/src/host/environment.ts` 的 `ENV_DEFAULTS.codecKey = 0`，注释写着
"自由参数：它对脚本的可观测行为**没有影响**，只影响盘上位模式"。前半句**至少在这里不成立** ——
引擎会因为 key = 0 而**抛异常**。

## 要做什么

1. **取证**：引擎自己那份 key 是**启动时赋值**的（它的写入点/来源）—— 找到它，或者证明它对本层可观测行为**确实**无影响
   （那样就该把"自由参数"这句话的范围写窄：*除 `0x6` 的前置条件外*）。
2. **在此之前**：`0x6` 的做法是**记一笔不抛**（`machine.note('codec-key-placeholder', …)`）——
   ⛔ 不照抄成抛，否则前沿会停在"我的占位值不合法"上（那是**模型的问题**，不是脚本的问题）。

## 判据

一条守卫能证明"key 的取值改变 ⇒ 除盘上位模式外**没有**可观测差异"，
或者一条台账条目把 key 的真实来源钉在 EA 上。
