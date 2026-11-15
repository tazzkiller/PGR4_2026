// Local tool (pgr4 project): tries libmspack's LZX decoder on a slice of a file, to find how the
// game's streamed data is compressed.
//   lzxprobe <file> <offset> <length> <window_bits> <out_len> [out_file]
// Prints how many bytes decoded before an error (or all of out_len).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mspack.h"
#include "lzx.h"

typedef struct {
  struct mspack_system sys;
} memsys;
typedef struct {
  unsigned char* buf;
  size_t size, pos;
} memfile;

static struct mspack_file* m_open(struct mspack_system* s, const char* f, int m) { return NULL; }
static void m_close(struct mspack_file* f) {}
static int m_read(struct mspack_file* f, void* b, int n) {
  memfile* m = (memfile*)f;
  size_t left = m->size - m->pos;
  if ((size_t)n > left) n = (int)left;
  memcpy(b, m->buf + m->pos, n);
  m->pos += n;
  return n;
}
static int m_write(struct mspack_file* f, void* b, int n) {
  memfile* m = (memfile*)f;
  size_t left = m->size - m->pos;
  if ((size_t)n > left) n = (int)left;
  memcpy(m->buf + m->pos, b, n);
  m->pos += n;
  return n;
}
static int m_seek(struct mspack_file* f, off_t o, int mode) { return -1; }
static off_t m_tell(struct mspack_file* f) { return ((memfile*)f)->pos; }
static void m_msg(struct mspack_file* f, const char* fmt, ...) {}
static void* m_alloc(struct mspack_system* s, size_t n) { return calloc(n, 1); }
static void m_free(void* p) { free(p); }
static void m_copy(void* s, void* d, size_t n) { memmove(d, s, n); }

int main(int argc, char** argv) {
  if (argc < 6) return 2;
  FILE* f = fopen(argv[1], "rb");
  if (!f) return 3;
  long long off = atoll(argv[2]);
  size_t len = (size_t)atoll(argv[3]);
  int wbits = atoi(argv[4]);
  size_t out_len = (size_t)atoll(argv[5]);
  unsigned char* in = malloc(len);
  _fseeki64(f, off, SEEK_SET);
  len = fread(in, 1, len, f);
  fclose(f);
  struct mspack_system sys = {m_open, m_close, m_read, m_write, m_seek, m_tell, m_msg,
                              m_alloc, m_free, m_copy, NULL};
  memfile src = {in, len, 0};
  memfile dst = {calloc(out_len, 1), out_len, 0};
  struct lzxd_stream* s = lzxd_init(&sys, (struct mspack_file*)&src, (struct mspack_file*)&dst,
                                    wbits, 0, 0x8000, (off_t)out_len, 0);
  if (!s) {
    printf("init failed\n");
    return 4;
  }
  // Decode in 32 KB steps to see how far it gets.
  size_t done = 0;
  int err = 0;
  while (done < out_len) {
    size_t step = out_len - done < 0x8000 ? out_len - done : 0x8000;
    err = lzxd_decompress(s, (off_t)step);
    if (err) break;
    done += step;
  }
  printf("decoded %zu of %zu, err %d, read %zu of %zu input\n", done, out_len, err, src.pos, len);
  if (argc > 6 && done) {
    FILE* o = fopen(argv[6], "wb");
    fwrite(dst.buf, 1, done, o);
    fclose(o);
  }
  lzxd_free(s);
  return 0;
}
