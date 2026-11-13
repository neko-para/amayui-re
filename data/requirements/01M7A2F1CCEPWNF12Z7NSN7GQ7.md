# emulator-lift · GetBackBuffer 每次新建对象（真机是同一对象 + AddRef）：接口保真，低优先

- id: REQ-01M7A2F1CCEPWNF12Z7NSN7GQ7
- type: req
- status: open
- parent: REQ-01M6H6G6DSRG1ZRD810Z0HTH8V
- tags: [emulator-lift, d3d9, fidelity]

## 缺口

`IDirect3DDevice9::GetBackBuffer` 今天**每次调用都新建一个 surface 对象**；真机的语义是"返回同一个后缓冲 surface 并 AddRef"
（连续两次调用：同一对象 / refcount += 1）。

## 影响

- 引擎若拿两次 `GetBackBuffer` 的返回值做**同一目标**判定（或按 EA 认对象），我们会给它两个不同对象；
- 今天的后果被 arena 规范化旁路吸收（`SetRenderTarget` 按"像素块 == 本设备后缓冲 arena"识别后缓冲），所以**没有用户可见缺陷**；
- 但它属于"D3D9 接口保真"缺口：口径是"除明确拒绝外都补"。

## 判据

- 连续两次 `GetBackBuffer(0,0,D3DBACKBUFFER_TYPE_MONO, &p)` ⇒ 两个 `IDirect3DSurface9*` **指针相同**、`Release` 两次后对象才销毁（refcount = 2）；
- 混用 `SetRenderTarget` 后仍成立；`Release` 到 0 后 arena 回收行为不变（`REQ-01M6X4N7C0PAF6XT7EAC172V85` 一族）。
- 守卫：`d3d9/get-backbuffer-identity`（`tests/d3d9.cpp`，Mock 设备即可）。
- 变异：让 `GetBackBuffer` 每次新建 ⇒ 该用例红。

## 非目标

- 不改 `GetBackBuffer` 的尺寸/格式参数校验；不实现多后缓冲。

## 优先级

低：无用户可见后果，属接口保真。排在渲染/音频主线之后。
