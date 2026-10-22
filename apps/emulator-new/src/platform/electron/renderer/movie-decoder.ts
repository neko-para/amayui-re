/**
 * MPEG-PS / MPEG-1 影片解码器（pl_mpeg → WASM）。
 *
 * ★ 只做视频；音频在 `movie.c` 里关闭。
 * ★ WASM 是**派生资产**（MIT，见 `movie/README.md`）；本文件不碰 Node / Electron API。
 * ★ `plm_decode(dt)` 按真实时间推进；一帧可能跨多次调用，返回 `null` 表示本次没有新帧。
 */
import wasmUrl from './movie/movie.wasm?url';

interface MovieWasm {
  memory: WebAssembly.Memory;
  movie_alloc(n: number): number;
  movie_open(ptr: number, len: number): number;
  movie_close(): void;
  movie_width(): number;
  movie_height(): number;
  movie_duration(): number;
  movie_decode(dt: number): number;
  movie_time(): number;
  movie_ended(): number;
  movie_rewind(): void;
  movie_frame(): number;
  movie_frame_width(): number;
  movie_frame_height(): number;
  movie_serial(): number;
}

export interface DecodedMovieFrame {
  width: number;
  height: number;
  rgba: Uint8Array;
}

export class PlMpegMovie {
  private readonly wasm: MovieWasm;
  private readonly input: Uint8Array;
  private readonly inputPtr: number;
  private serial = 0;
  private closed = false;

  private constructor(wasm: MovieWasm, input: Uint8Array) {
    this.wasm = wasm;
    this.input = input;
    this.inputPtr = wasm.movie_alloc(input.length);
    this.view().set(input, this.inputPtr);
    if (wasm.movie_open(this.inputPtr, input.length) === 0) {
      throw new Error('movie-open-failed');
    }
    this.serial = wasm.movie_serial();
  }

  static async open(bytes: Uint8Array): Promise<PlMpegMovie> {
    const response = await fetch(wasmUrl);
    if (!response.ok) throw new Error(`movie-wasm-fetch-failed:${response.status}`);
    const { instance } = await WebAssembly.instantiate(await response.arrayBuffer(), {});
    return new PlMpegMovie(instance.exports as unknown as MovieWasm, new Uint8Array(bytes));
  }

  private view(): Uint8Array {
    return new Uint8Array(this.wasm.memory.buffer);
  }

  get width(): number { return this.wasm.movie_frame_width() || this.wasm.movie_width(); }
  get height(): number { return this.wasm.movie_frame_height() || this.wasm.movie_height(); }
  get duration(): number { return this.wasm.movie_duration(); }
  get currentTime(): number { return this.wasm.movie_time(); }
  get ended(): boolean { return this.wasm.movie_ended() !== 0; }

  /** 推进 dt 秒；有新帧则返回 RGBA 副本（top-down）。 */
  decode(dt: number): DecodedMovieFrame | null {
    if (this.closed) return null;
    const serial = this.wasm.movie_decode(dt);
    if (serial === this.serial) return null;
    this.serial = serial;
    const width = this.wasm.movie_frame_width();
    const height = this.wasm.movie_frame_height();
    const ptr = this.wasm.movie_frame();
    if (width <= 0 || height <= 0 || ptr === 0) return null;
    const size = width * height * 4;
    return { width, height, rgba: this.view().slice(ptr, ptr + size) };
  }

  rewind(): void {
    if (!this.closed) this.wasm.movie_rewind();
  }

  close(): void {
    if (this.closed) return;
    this.closed = true;
    this.wasm.movie_close();
  }
}
