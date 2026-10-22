/**
 * Electron 的影片后端：pl_mpeg WASM 解码 → Pixi 纹理槽。
 *
 * ★ 0x20f 的 handler 是同步的；本类在 `open()` 里立刻返回句柄，异步加载字节 / WASM。
 *   `poll()` 在不是 ready 时返回**未结束**（不伪造结束），由核心的影片对象表服务等待。
 * ★ 音频忽略；视频帧按真实墙钟推进，并在有新帧时上传 + `presentNow()`。
 */
import type { MovieHandle, MovieHost, MovieOpenRequest, MoviePoll } from '../../../core/movie.ts';
import { PlMpegMovie } from './movie-decoder.ts';
import type { PixiRenderHost } from './pixi-host.ts';

interface LocalMovie {
  request: MovieOpenRequest;
  decoder: PlMpegMovie | null;
  ready: boolean;
  failed: boolean;
  playing: boolean;
  lastMs: number;
}

export class LocalMovieHost implements MovieHost {
  private readonly pixi: PixiRenderHost;
  private readonly loadBytes: (id: number) => Promise<Uint8Array | null>;
  private readonly movies = new Map<LocalMovie, MovieHandle>();
  private firstFrameLogged = false;

  constructor(pixi: PixiRenderHost, loadBytes: (id: number) => Promise<Uint8Array | null>) {
    this.pixi = pixi;
    this.loadBytes = loadBytes;
  }

  open(request: MovieOpenRequest): MovieHandle | null {
    const movie: LocalMovie = {
      request,
      decoder: null,
      ready: false,
      failed: false,
      playing: false,
      lastMs: 0,
    };
    const handle = movie as unknown as MovieHandle;
    this.movies.set(movie, handle);
    void this.init(movie);
    return handle;
  }

  private async init(movie: LocalMovie): Promise<void> {
    try {
      const bytes = await this.loadBytes(movie.request.resourceId);
      if (bytes === null) {
        movie.failed = true;
        return;
      }
      movie.decoder = await PlMpegMovie.open(bytes);
      movie.ready = true;
      if (movie.playing) this.startClock(movie);
    } catch {
      movie.failed = true;
    }
  }

  private movieOf(handle: MovieHandle): LocalMovie | null {
    return handle as unknown as LocalMovie;
  }

  private startClock(movie: LocalMovie): void {
    movie.lastMs = performance.now();
    // 立即解出第一帧；不等下一次 poll 才让纹理槽有内容
    this.decodeInto(movie, 0);
  }

  private decodeInto(movie: LocalMovie, dt: number): void {
    const decoder = movie.decoder;
    if (decoder === null) return;
    const frame = decoder.decode(dt);
    if (frame === null) return;
    const uploaded = this.pixi.uploadMovieFrame({
      textureId: movie.request.slot,
      width: frame.width,
      height: frame.height,
      rgba: frame.rgba,
    });
    if (uploaded) {
      if (!this.firstFrameLogged) {
        this.firstFrameLogged = true;
        this.pixi.movieNote(`movie(slot ${movie.request.slot})：首帧 ${frame.width}×${frame.height}（pl_mpeg WASM，音频忽略）`);
      }
      this.pixi.presentNow();
    }
  }

  play(handle: MovieHandle): void {
    const movie = this.movieOf(handle);
    if (movie === null) return;
    movie.playing = true;
    if (movie.ready) this.startClock(movie);
  }

  poll(handle: MovieHandle): MoviePoll | null {
    const movie = this.movieOf(handle);
    if (movie === null) return null;
    if (movie.failed) return { currentTime: 1, stopTime: 1, ended: true };
    if (!movie.ready || movie.decoder === null) {
      return { currentTime: 0, stopTime: 0, ended: false };
    }
    if (movie.playing) {
      const now = performance.now();
      const dt = Math.max(0, Math.min(0.1, (now - movie.lastMs) / 1000));
      movie.lastMs = now;
      this.decodeInto(movie, dt);
    }
    return {
      currentTime: movie.decoder.currentTime,
      stopTime: movie.decoder.duration,
      ended: movie.decoder.ended,
    };
  }

  rewind(handle: MovieHandle): void {
    const movie = this.movieOf(handle);
    if (movie === null || movie.decoder === null) return;
    movie.decoder.rewind();
    this.startClock(movie);
  }

  close(handle: MovieHandle): void {
    const movie = this.movieOf(handle);
    if (movie === null) return;
    this.movies.delete(movie);
    movie.decoder?.close();
    movie.decoder = null;
    movie.ready = false;
    movie.playing = false;
  }

  fillTexture(textureId: number): void {
    // 规范只规定“调用条件”；像素效果本版不规定（RES-MOVIE-FILL）。这里保留最后一帧。
    void textureId;
  }
}
