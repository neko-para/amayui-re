#include <stddef.h>
#include <stdint.h>
#define PLM_NO_STDIO
#define PL_MPEG_IMPLEMENTATION
#define PLM_MALLOC plm_malloc
#define PLM_FREE plm_free
#define PLM_REALLOC plm_realloc
int abs(int x);
void *plm_malloc(size_t n);
void plm_free(void *p);
void *plm_realloc(void *p, size_t n);
#include "pl_mpeg.h"

typedef unsigned long uintptr_t;
extern unsigned char __heap_base;
static uintptr_t heap = 0;
static uint8_t *frame_buf = 0;
static int frame_w = 0, frame_h = 0, frame_serial = 0;
static plm_t *movie = 0;

static void ensure_heap(void) {
  if (heap == 0) heap = (uintptr_t)&__heap_base;
}
static uintptr_t mem_top(void) { return (uintptr_t)__builtin_wasm_memory_size(0) * 65536; }
static void ensure_mem(uintptr_t need) {
  uintptr_t top = mem_top();
  if (need <= top) return;
  uintptr_t pages = (need - top + 65535) / 65536;
  if (__builtin_wasm_memory_grow(0, pages) == (size_t)-1) __builtin_trap();
}
void *plm_malloc(size_t n) {
  ensure_heap();
  n = (n + 15) & ~(size_t)15;
  uintptr_t p = heap;
  heap += n + 16;
  ensure_mem(heap);
  *(size_t *)p = n;
  return (void *)(p + 16);
}
void plm_free(void *p) { (void)p; }
void *plm_realloc(void *p, size_t n) {
  if (!p) return plm_malloc(n);
  size_t old = *(size_t *)((uintptr_t)p - 16);
  void *q = plm_malloc(n);
  memcpy(q, p, old < n ? old : n);
  return q;
}
void *memcpy(void *dst, const void *src, size_t n) {
  uint8_t *d = (uint8_t *)dst; const uint8_t *s = (const uint8_t *)src;
  for (size_t i = 0; i < n; i++) d[i] = s[i];
  return dst;
}
void *memmove(void *dst, const void *src, size_t n) {
  uint8_t *d = (uint8_t *)dst; const uint8_t *s = (const uint8_t *)src;
  if (d < s) for (size_t i = 0; i < n; i++) d[i] = s[i];
  else for (size_t i = n; i > 0; i--) d[i-1] = s[i-1];
  return dst;
}
void *memset(void *dst, int c, size_t n) {
  uint8_t *d = (uint8_t *)dst;
  for (size_t i = 0; i < n; i++) d[i] = (uint8_t)c;
  return dst;
}
int memcmp(const void *a, const void *b, size_t n) {
  const uint8_t *x = (const uint8_t *)a, *y = (const uint8_t *)b;
  for (size_t i = 0; i < n; i++) if (x[i] != y[i]) return x[i] - y[i];
  return 0;
}

static void on_video(plm_t *self, plm_frame_t *frame, void *user) {
  (void)self; (void)user;
  frame_w = (int)frame->width; frame_h = (int)frame->height;
  if (!frame_buf) frame_buf = (uint8_t *)plm_malloc((size_t)frame_w * frame_h * 4);
  plm_frame_to_rgba(frame, frame_buf, frame_w * 4);
  for (int i = 3; i < frame_w * frame_h * 4; i += 4) frame_buf[i] = 255;
  frame_serial++;
}

__attribute__((export_name("movie_alloc"))) void *movie_alloc(int n) { return plm_malloc((size_t)n); }
__attribute__((export_name("movie_open"))) int movie_open(uint8_t *data, int len) {
  if (movie) { plm_destroy(movie); movie = 0; }
  frame_serial = 0;
  frame_w = 0; frame_h = 0;
  movie = plm_create_with_memory(data, (size_t)len, 0);
  if (!movie) return 0;
  plm_set_audio_enabled(movie, 0);
  plm_set_video_decode_callback(movie, on_video, 0);
  frame_w = plm_get_width(movie); frame_h = plm_get_height(movie);
  return 1;
}
__attribute__((export_name("movie_close"))) void movie_close(void) { if (movie) { plm_destroy(movie); movie = 0; } }
__attribute__((export_name("movie_width"))) int movie_width(void) { return frame_w; }
__attribute__((export_name("movie_height"))) int movie_height(void) { return frame_h; }
__attribute__((export_name("movie_duration"))) double movie_duration(void) { return movie ? plm_get_duration(movie) : 0; }
__attribute__((export_name("movie_decode"))) int movie_decode(double dt) {
  if (!movie) return -1;
  plm_decode(movie, dt);
  return frame_serial;
}
__attribute__((export_name("movie_time"))) double movie_time(void) { return movie ? plm_get_time(movie) : 0; }
__attribute__((export_name("movie_ended"))) int movie_ended(void) { return movie ? plm_has_ended(movie) : 1; }
__attribute__((export_name("movie_rewind"))) void movie_rewind(void) { if (movie) plm_rewind(movie); }
__attribute__((export_name("movie_frame"))) uint8_t *movie_frame(void) { return frame_buf; }
__attribute__((export_name("movie_frame_width"))) int movie_frame_width(void) { return frame_w; }
__attribute__((export_name("movie_frame_height"))) int movie_frame_height(void) { return frame_h; }
__attribute__((export_name("movie_serial"))) int movie_serial(void) { return frame_serial; }
int abs(int x) { return x < 0 ? -x : x; }
