/**
 * 渲染请求边界（`data/spec/chapters/60-ops-render.md`）：
 * **平台接口**（`RenderHost`）+ **渲染状态**（当前目标 / 场景混合 / 字体对象）+ 一个**显式的无后端实现**。
 *
 * ★ 纯核心：本文件不 import `node:*` / `electron` / `pixi.js` / DOM。
 *   `tsconfig.core.json` 的 `types: []` 是这条的**真守卫**；本注释只是说明。
 * ★ 分工（spec 的「渲染请求边界」章）：
 *   - 本层规定**请求什么、参数是什么、写哪些引擎数据**；
 *   - 平台是否真的切换了目标、后缓冲是否可见、字形像素长什么样，都**不在本层**
 *     （spec 的 `RES-RENDER-TARGET-PLATFORM` / `RES-RENDER-TEXT-SURFACE` / `RES-RENDER-BLEND-PIXEL`）。
 *   ⇒ 这里定义的是"引擎向平台要什么"，不是"平台怎么做"。
 * ★ ⛔ 不伪造渲染：唯一自带的实现是 `HeadlessRenderHost`，它**没有后端**、不产生像素，
 *   只按契约回答"能 / 不能"并把收到的请求记账。
 */

/**
 * 平台渲染表面的**不透明句柄**：核心只把它原样传递（不解引用、不比较、不序列化）。
 * ★ 平台实现（Electron / Pixi）自己决定它装什么（`RenderTexture` / 原生 surface 指针…），
 *   造出来时用 `as unknown as SurfaceHandle` 标注；核心一个成员都不读。
 */
declare const SURFACE_HANDLE_BRAND: unique symbol;
export type SurfaceHandle = { readonly [SURFACE_HANDLE_BRAND]: 'amayui.render.surface' };

/**
 * `0x204` 的一次同步绘制请求（`host.drawText`）：
 * `textureId` = 目标纹理槽号；`x` / `y` = 操作数给的坐标；`text` = 按文本读出的字节串；
 * 其余字段全部是**字体对象当前的状态**（⛔ 不是操作数）：face name / 字号 / 字重 / 主色 / 副色 /
 * 绘制模式 / 对位模式（逐字锚见各字段注释与 `FontObjectState`）。
 *
 * ★ `text` 的字节编码是 **Shift-JIS**（已证，不是猜测）：`0x46F3D4` 起做 SJIS 前导字节判定、
 *   `0x46F4CD` 合成双字节、`0x8140`（全角空格）走特例。核心只搬运**字节串**（latin1 口径，
 *   与核心其它字符串一致），**解码 / 字形选择在平台侧**（`RES-RENDER-TEXT-ENCODING` 的编码部分已解，
 *   字形选择仍未解）。
 * ★ `font` / `color` 的 `null` 是**显式的"本层没有建立它"**（只有 face name 会缺：核心没有
 *   建字体对象的路径；主色的默认值是证过的 `0xFFFFFF`）。平台侧必须把 `null` 读成"未指定"，
 *   而不是"空字体名 / 透明黑"。
 */
export interface DrawTextRequest {
  textureId: number;
  x: number;
  y: number;
  /** 文本字节串：**Shift-JIS**（`0x46F3D4` / `0x46F4CD` / `0x8140` 逐字证）；核心不解码 */
  text: string;
  /** 字体对象当前的 face name（`0x1a5` 写 `Font+0x4EC`）；`null` = 本层未建立 */
  font: string | null;
  /** 字号（`0x75` 写 `Font+0x313D4`）；默认 24 */
  size: number;
  /** 字重（`0x2BD` 写 700 或 0）；默认 0（⛔ 初值未取证） */
  weight: number;
  /** 主色（`Font+0x550`）；默认 `0xFFFFFF`（`0x4655FB` 逐字证） */
  color: number | null;
  /** 副色 / 阴影色（`Font+0x554`）；默认 0（`0x465605`） */
  shadowColor: number;
  /** 绘制模式（`Font+0x55C`）；默认 1（`0x465611`） */
  drawMode: number;
  /**
   * 对位模式（`Font+0x313D0`；`0x45679E` 的 `cmp [Font+0x313D0], 1`）。
   * ★ 证据：**仅当它 == 1** 时 y 要加「缓存 ascent − 当前 ascent」；本层**没有**这两个 ascent
   *   ⇒ ⛔ 不伪造 y 修正，只把该格的记录值交给平台（平台也无法据此算出修正量）。
   *   默认 0 是**占位**（该格初值未取证）。
   */
  alignMode: number;
}

/** 纹理对象的种类（`0x1f8` 的第 4 个操作数：3 ⇒ 分割，其它 ⇒ 普通） */
export type TextureKind = 'normal' | 'divided';

/** `0x1f8` 的创建请求（尺寸是操作数给的，像素不上传） */
export interface TextureCreateRequest {
  textureId: number;
  width: number;
  height: number;
  kind: TextureKind;
}

/** `0x20b` 的矩形填充请求（颜色已按 `0xFF000000 | RGB` 打包）。 */
export interface FillTextureRequest {
  textureId: number;
  left: number;
  top: number;
  right: number;
  bottom: number;
  color: number;
  alpha: number;
}

/** `0x1f9` 的上传请求：解码后的 top-down RGBA8 + 归一化颜色键 */
export interface TextureUploadRequest {
  textureId: number;
  width: number;
  height: number;
  kind: TextureKind;
  rgba: Uint8Array;
  /**
   * 归一化颜色键 `0xFF000000 | RGB`；**`0` = 无键**（`0x1f9` 的负数操作数归一化成 0）。
   * ★ 平台比较的是 **32 位 ARGB 全值**（A 字节参与，见 `applyColorKey` 的引擎证据）：
   *   键 0 不会命中不透明像素；只有非负操作数（如 0 归一化出的 `0xFF000000`）才清不透明黑。
   */
  color: number;
}

/**
 * 渲染宿主：平台侧要实现的那一组方法（Electron / Pixi 与测试用 fake 都按这个映射）。
 *
 * ★ 全部是 `Promise`：核心的 IO / 外部请求一律可等待（核心能跑在浏览器 / worker 里）。
 * ★ 本层的**失败判据只有"null / false"**，没有异常通道：
 *   - `getRenderSurface` / `getBackBuffer` 取不到 ⇒ `null`；
 *   - `setRenderTarget` 失败 ⇒ `false`；
 *   - `createTexture` / `uploadTexture` 失败 ⇒ `false`。
 *   抛异常只表示"平台实现坏了"，不表示"引擎上报了失败"（`0x20d` 的失败路径写错误通道，不抛）。
 */
export interface RenderHost {
  /**
   * **一次只读询问**：这个纹理对象的渲染表面。
   *
   * ★ 同一个询问被两条指令共用（spec 明写）：
   *   - `0x20d` 用它**取表面**（取到后交给 `setRenderTarget`，再 `releaseSurface`）；
   *   - `0x204` 的**入口条件 ②** 只看它非空（"该纹理对象的一次只读询问返回非空"）。
   * ⇒ ⛔ 不要为这两条实现两个不同的判据：`null` 就是"取不到表面"。
   * ★ 槽里**有没有纹理对象**不在这里问：那是核心的 `scene.textureObjects`（入口条件 ①）。
   */
  getRenderSurface(textureId: number): Promise<SurfaceHandle | null>;

  /** 把一份表面设为当前渲染目标；`false` = 设置失败（失败路径由调用方写错误通道） */
  setRenderTarget(surface: SurfaceHandle): Promise<boolean>;

  /** 平台后缓冲（`0x20d` 的 ≥ 1000 那一支）；`null` = 取不到 */
  getBackBuffer(): Promise<SurfaceHandle | null>;

  /**
   * 归还一份表面引用。**成功与失败路径都归还**（spec 的「状态对应」）；
   * `0x20d` 的归还发生在写当前渲染目标**之前**（次序是合同）。
   */
  releaseSurface(surface: SurfaceHandle): Promise<void>;

  /**
   * `0x1f8`：创建 / 重置一张空白纹理（**不上传像素**）。
   * `false` = 平台创建失败；调用方把失败写错误通道，对象仍留在槽里。
   */
  createTexture(request: TextureCreateRequest): Promise<boolean>;

  /**
   * `0x1f9`：把解码后的 RGBA 上传到该纹理槽（**同步请求**；返回前完成）。
   * `false` = 上传失败；调用方按显示消息型异常处理。
   * ★ 颜色键的像素清零由平台实现（见请求字段）；核心只搬运归一化后的键。
   */
  uploadTexture(request: TextureUploadRequest): Promise<boolean>;

  /** `0x20b` 矩形填充；缺省 = 平台不支持该请求（调用方写错误通道）。 */
  fillTexture?(request: FillTextureRequest): Promise<boolean>;

  /**
   * `0x1ae` 把纹理当前画面捕捉成文件字节（BMP）；缺省 = 平台不支持。
   *
   * ⚠ **实现现状，与 `FMT-SAVE-STH` 相反**：合同要求 **core 自己组装 BMP 容器**
   * （14 字节文件头 + 40 字节信息头 + 按对齐行宽写满的像素块），平台只提供
   * 「像素 + 行宽 + 通道掩码」；本接口却让平台交出**整份文件字节**。
   * 分歧登记在 `RES-SAVE-STH-CAPTURE`（what ② / next ②）—— 收口时把容器搬回 core，
   * 本接口改成返回像素 + 行宽 + 通道掩码、且**不再**返回整份 BMP。
   */
  captureTexture?(textureId: number): Promise<Uint8Array | null>;

  /**
   * `0x1af` 把文件字节（BMP）上传进纹理槽；缺省 = 平台不支持。
   *
   * ⚠ 同上：合同要求 core 先按 `FMT-SAVE-STH` 解析容器、再把**像素**交给平台；
   * 本接口接受**整份文件字节**，是**实现现状**（`RES-SAVE-STH-CAPTURE`），不是合同。
   */
  uploadTextureBytes?(textureId: number, bytes: Uint8Array): Promise<boolean>;

  /** `0x1fa`：释放该纹理槽的平台纹理（幂等；空槽也要能调用）。 */
  releaseTexture(textureId: number): Promise<void>;

  /**
   * 把一段文本画进某个纹理槽的纹理（`0x204` 的唯一一次绘制请求）。
   *
   * 返回值：`true` = 请求被受理；`false` = **平台侧加锁 / 取矩形失败**
   * （逐字证据：`0x46F342 call sub_4A3890`（LockTexture）失败会写错误通道 `"LockTexture エラー"`）。
   * ★ spec 正文说本条"没有引擎自己上报的失败分支"——那句已被这条证据**推翻**，
   *   属**待 spec 修订**项（本层不改 `data/spec/**`）。
   */
  drawText(request: DrawTextRequest): Promise<boolean>;

  /**
   * 可选：把当前后缓冲提交到屏幕。
   * ★ 本层**不调用**它：切换目标与提交 / 呈现的先后关系没有取证
   *   （spec 的 `RES-RENDER-TARGET-ORDER`）⇒ 不替引擎安排这一趟。
   */
  present?(): Promise<void>;
}

/* ──────────────────────────── 渲染状态（引擎数据） ──────────────────────────── */

/**
 * 「无颜色」哨兵：与「场景 / 颜色族」共用同一个口径。
 * ★ 它的具体数值属**机器细节**（spec 明写不规定）；本层沿用场景族既有模型里的 `-1`，
 *   ⛔ 不要据此认为 `-1` 是取证过的值（合成结果因此不唯一，见报告里的 spec 不明确项）。
 */
export const NO_COLOR = -1;

/**
 * 「后缓冲」渲染目标哨兵：当前渲染目标格里表示"目标是平台后缓冲、不是纹理槽"。
 * ★ 数值属机器细节；选 `-1` 便于与合法纹理槽号（≥ 0）区分。
 */
export const RENDER_TARGET_BACK_BUFFER = -1;

/**
 * 字体对象状态：`0x204` 用到的那些格。
 * ★ 字体对象是引擎持有的那个接收者（`0x75` / `0x76` / `0x77` / `0x8b` / `0x197` / `0x1a4` /
 *   `0x1a5` / `0x2bd` / `0x2fe` 共用）。本版**只建模** `0x204` 读 / 写的那几格，
 *   其余字段（尺寸 / 描述 / 十个子对象…）不建模。
 */
export interface FontObjectState {
  /** face name（`0x1a5` 写 `Font+0x4EC`）；`null` = 本层未建立（核心没有建字体对象的路径） */
  font: string | null;
  /** 字号（`0x75` 写 `Font+0x313D4`）；默认 24 */
  size: number;
  /** 字重（`0x2BD` 写 700 或 0）；默认 0（⛔ 初值未取证） */
  weight: number;
  /** 主色（`Font+0x550`）；默认 `0xFFFFFF`（`0x4655FB` 逐字证） */
  color: number | null;
  /** 副色 / 阴影色（`Font+0x554`）；默认 0（`0x465605`） */
  shadowColor: number;
  /** 绘制模式（`Font+0x55C`）；默认 1（`0x465611`） */
  drawMode: number;
  /**
   * 对位模式（`Font+0x313D0`）：证据 `0x45679E cmp [Font+0x313D0], 1`；默认 0 是**占位**
   * （初值未取证）。== 1 时的 y 修正属平台侧、**本层未建模**（没有缓存 / 当前 ascent）。
   */
  alignMode: number;
  /**
   * 显示记录的 **`VirtualFullScreenType`**：**== 2 才 SelectObject**（判据已逐字核；
   * `RES-RENDER-TEXT-FONT-SELECT`）。★ 本层的 `RenderHost.drawText` 把"选字体"并进平台实现
   * ⇒ 这个格**被记录、但本层不据此分支**（接口表达不出的地方）。
   */
  renderState: number;
  /** `drawMode` 的**选择格**（取值 0 / 1 / 2 / 3，二选一绘制入口；`RES-RENDER-TEXT-DRAW-PATH`） */
  drawPath: number;
  /**
   * 两个内部布尔开关（身份与取值本版不规定 ⇒ `RES-RENDER-TEXT-FONT-SWITCHES`）。
   * ★ `switches[0]` 是请求第 1 步**清 0** 的那一个（spec 明写"把一个内部布尔开关清 0"）；
   *   `switches[1]` 的写入值未规定 ⇒ 本层**不猜**（保持原值，调用点记一条诊断）。
   */
  switches: [number, number];
  /**
   * 字形暂存区的**重建次数**：spec 说"字形位图长度与两份暂存缓冲被重建"，但布局属机器细节
   * ⇒ 只记"重建过几次"，⛔ 不建两份假缓冲。
   */
  glyphCacheRebuilds: number;
}

/** 渲染状态的默认值（场景建立路径本版不规定 ⇒ 见 `RES-RENDER-TARGET-INIT` / `RES-RENDER-BLEND-INIT`） */
export function initialFontObjectState(): FontObjectState {
  return {
    font: null,        // 核心没有建字体对象的路径 ⇒ face name 显式"未建立"
    size: 24,          // 0x75 的尺寸格；24 是已证的默认值
    weight: 0,         // 0x2BD 的二选一常量（700 / 0）；默认 0 —— ⛔ 初值未取证
    color: 0xffffff,   // Font+0x550 主色；0x4655FB 逐字证的默认值
    shadowColor: 0,    // Font+0x554 副色；0x465605 逐字证
    drawMode: 1,       // Font+0x55C；0x465611 逐字证
    alignMode: 0,      // Font+0x313D0 的占位（初值未取证；==1 的 y 修正本层未建模）
    // 两个选择格的初值没有取证；取 0 只是占位（⛔ 不是"观察到的初值"）
    renderState: 0,
    drawPath: 0,
    switches: [0, 0],
    glyphCacheRebuilds: 0,
  };
}

/** 本层建模的渲染状态（当前渲染目标 + 场景混合的两个格 + 字体对象） */
export interface RenderState {
  /**
   * 场景的**当前渲染目标**：一个纹理槽号，或 `RENDER_TARGET_BACK_BUFFER` 哨兵。
   * ★ spec 的「状态对应」说它落在"场景记录里的一个整数字段"；本层放在 `Machine.render`
   *   （单一真源，不再往 `scene` 里复制一份）。
   * ★ 初值 = 「后缓冲」哨兵（场景建立时写入；场景建立本版不规定）。
   */
  target: number;
  /** 场景混合的模式（效应对象上的一个整数格）；原样收下操作数，不 clamp */
  blendMode: number;
  /** 场景混合的颜色（同一对象上的颜色格）；低 24 位 = 颜色、高 8 位 = 透明度 */
  blendColor: number;
  /** 字体对象状态（`0x204` 的字体 / 颜色来源） */
  fontObject: FontObjectState;
}

/** 场景建立时的渲染状态初值（spec 给了模式 = 0、颜色 = 无颜色、目标 = 后缓冲哨兵） */
export function initialRenderState(): RenderState {
  return {
    target: RENDER_TARGET_BACK_BUFFER,
    blendMode: 0,
    blendColor: NO_COLOR,
    fontObject: initialFontObjectState(),
  };
}

/* ──────────────────────────── 显式的"无后端"实现 ──────────────────────────── */

/** headless 宿主记下的一条请求（**只是记账**：没有像素、没有平台资源） */
export type HeadlessRenderRequest =
  | { kind: 'get-render-surface'; textureId: number; ok: boolean }
  | { kind: 'get-back-buffer'; ok: boolean }
  | { kind: 'set-render-target'; surface: SurfaceHandle; ok: boolean }
  | { kind: 'release-surface'; surface: SurfaceHandle }
  | { kind: 'create-texture'; request: TextureCreateRequest; ok: boolean }
  | { kind: 'upload-texture'; request: TextureUploadRequest; ok: boolean }
  | { kind: 'release-texture'; textureId: number }
  | { kind: 'fill-texture'; request: FillTextureRequest; ok: boolean }
  | { kind: 'capture-texture'; textureId: number; ok: boolean }
  | { kind: 'upload-texture-bytes'; textureId: number; length: number; ok: boolean }
  | { kind: 'draw-text'; request: DrawTextRequest };

export interface HeadlessRenderOptions {
  /**
   * "该纹理槽的纹理对象能给出渲染表面吗"。
   * ★ 缺省 `() => true`：**这不是"有后端"的声称** —— 槽里有没有纹理对象已由核心先判
   *   （`0x20d` / `0x204` 的入口条件 ①），核心只会问**已存在**的纹理对象。
   *   要跑"取不到表面"的路径就传 `() => false`（或按 id 判）。
   */
  hasSurface?: (textureId: number) => boolean;
  /** 后缓冲可取到吗（缺省 `true`：给一个抽象句柄） */
  backBuffer?: boolean;
  /** `setRenderTarget` 会成功吗（缺省 `true`） */
  setRenderTargetSucceeds?: boolean;
  /** `createTexture` 会成功吗（缺省 `true`） */
  createTextureSucceeds?: boolean;
  /** `uploadTexture` 会成功吗（缺省 `true`） */
  uploadTextureSucceeds?: boolean;
  /**
   * `drawText` 返回 true 还是 false（缺省 `true`）。
   * ★ 只影响**返回值**（失败路径记账用），⛔ 不产生像素、不模拟加锁。
   */
  drawTextSucceeds?: boolean;
  /** `0x1ae` 的捕捉结果；缺省 = 捕捉不到（null）。 */
  captureBytes?: Uint8Array;
}

/**
 * **无后端**渲染宿主：给 CLI / 无头执行用。
 *
 * ★ 它**不是渲染器**：不建纹理资源、不发平台请求、不产生像素、不读纹理内容。
 *   - `getRenderSurface` / `getBackBuffer` 返回**抽象句柄**（一个带标记的纯对象），
 *   - `setRenderTarget` 直接成功，
 *   - `drawText` 只把请求记进 `requests`，并返回 `drawTextSucceeds`（缺省 `true`；同样只是记账）。
 * ★ 因此用它跑出来的"设了目标 / 画了文字"**只是请求记账**，⛔ 不能据此说渲染发生了，
 *   也不能拿它的输出当像素结果（spec 的 `RES-RENDER-TEXT-SURFACE` / `RES-RENDER-TARGET-PLATFORM`）。
 */
export class HeadlessRenderHost implements RenderHost {
  /** 显式标记：这个宿主**没有渲染后端**（平台层不要把它当渲染器用） */
  readonly backend = 'headless' as const;
  /** 收到的请求（按发生顺序；这是唯一的输出） */
  readonly requests: HeadlessRenderRequest[] = [];
  readonly hasSurface: (textureId: number) => boolean;
  readonly backBuffer: boolean;
  setRenderTargetSucceeds: boolean;
  createTextureSucceeds: boolean;
  uploadTextureSucceeds: boolean;
  drawTextSucceeds: boolean;
  captureBytes: Uint8Array | undefined;

  constructor(options: HeadlessRenderOptions = {}) {
    this.hasSurface = options.hasSurface ?? (() => true);
    this.backBuffer = options.backBuffer ?? true;
    this.setRenderTargetSucceeds = options.setRenderTargetSucceeds ?? true;
    this.createTextureSucceeds = options.createTextureSucceeds ?? true;
    this.uploadTextureSucceeds = options.uploadTextureSucceeds ?? true;
    this.drawTextSucceeds = options.drawTextSucceeds ?? true;
    this.captureBytes = options.captureBytes;
  }

  async getRenderSurface(textureId: number): Promise<SurfaceHandle | null> {
    const ok = this.hasSurface(textureId);
    this.requests.push({ kind: 'get-render-surface', textureId, ok });
    return ok ? headlessSurface(textureId) : null;
  }

  async setRenderTarget(surface: SurfaceHandle): Promise<boolean> {
    const ok = this.setRenderTargetSucceeds;
    this.requests.push({ kind: 'set-render-target', surface, ok });
    return ok;
  }

  async getBackBuffer(): Promise<SurfaceHandle | null> {
    this.requests.push({ kind: 'get-back-buffer', ok: this.backBuffer });
    return this.backBuffer ? headlessSurface(null) : null;
  }

  async releaseSurface(surface: SurfaceHandle): Promise<void> {
    this.requests.push({ kind: 'release-surface', surface });
  }

  async createTexture(request: TextureCreateRequest): Promise<boolean> {
    // ★ 只记账：不建 GPU 纹理、不分配像素
    this.requests.push({ kind: 'create-texture', request: { ...request }, ok: this.createTextureSucceeds });
    return this.createTextureSucceeds;
  }

  async uploadTexture(request: TextureUploadRequest): Promise<boolean> {
    // ★ 只记账：像素留在请求里，headless 不解释、不上传
    this.requests.push({ kind: 'upload-texture', request: { ...request }, ok: this.uploadTextureSucceeds });
    return this.uploadTextureSucceeds;
  }

  async releaseTexture(textureId: number): Promise<void> {
    this.requests.push({ kind: 'release-texture', textureId });
  }

  async fillTexture(request: FillTextureRequest): Promise<boolean> {
    this.requests.push({ kind: 'fill-texture', request: { ...request }, ok: true });
    return true;
  }

  async captureTexture(textureId: number): Promise<Uint8Array | null> {
    const bytes = this.captureBytes ?? null;
    this.requests.push({ kind: 'capture-texture', textureId, ok: bytes !== null });
    return bytes;
  }

  async uploadTextureBytes(textureId: number, bytes: Uint8Array): Promise<boolean> {
    this.requests.push({ kind: 'upload-texture-bytes', textureId, length: bytes.length, ok: true });
    return true;
  }

  async drawText(request: DrawTextRequest): Promise<boolean> {
    // ★ 只记账：不写字形、不碰像素、不改纹理（无后端）
    this.requests.push({ kind: 'draw-text', request: { ...request } });
    // "受理"是记账层面的：true ⛔ 不代表画出来了
    return this.drawTextSucceeds;
  }
}

/** 抽象表面：核心不读它，headless 自己也不读；存在只为让"有 / 没有表面"可区分且可记账 */
function headlessSurface(textureId: number | null): SurfaceHandle {
  return { kind: 'headless-surface', textureId } as unknown as SurfaceHandle;
}
