// Local tool (pgr4 project): extracts the resources of a PGR4 .stream2 (Bizarre Creations'
// streamed world data) with libmspack's LZX decoder (from the rexglue SDK's third-party tree).
// Each resource is an XCompress stream after a header whose last four big-endian words are the
// uncompressed size, 0, the uncompressed size again and the compressed size: frames of 2-byte
// big-endian compressed sizes (or 0xFF + uncompressed + compressed sizes), whose payloads, put
// end to end, are one LZX stream with a 128 KB window (as Xenia's XEX loader decodes XCompress).
// Writes <out_dir>/<offset>_<size>.bin and <out_dir>/list.txt.
//   stream2ms <file.stream2> <out_dir>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mspack.h"
#include "lzx.h"

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

static uint32_t be32(const uint8_t* p) {
  return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

// The resource's frames from `start`, their payloads end to end into `payload`; its length, or
// 0 if the frames do not fill exactly `csize` bytes.
static size_t Frames(const uint8_t* d, size_t start, size_t csize, uint8_t* payload) {
  size_t q = start, end = start + csize, n = 0;
  while (q < end) {
    size_t c, h = 2;
    if (q + 2 > end) return 0;
    if (d[q] == 0xFF) {
      if (q + 5 > end) return 0;
      c = (d[q + 3] << 8) | d[q + 4];
      h = 5;
    } else {
      c = (d[q] << 8) | d[q + 1];
    }
    if (!c || q + h + c > end) return 0;
    memcpy(payload + n, d + q + h, c);
    n += c;
    q += h + c;
  }
  return n;
}

int main(int argc, char** argv) {
  if (argc < 3) return 2;
  FILE* f = fopen(argv[1], "rb");
  if (!f) return 3;
  _fseeki64(f, 0, SEEK_END);
  size_t len = (size_t)_ftelli64(f);
  _fseeki64(f, 0, SEEK_SET);
  uint8_t* d = malloc(len);
  if (!d) return 5;
  len = fread(d, 1, len, f);
  fclose(f);
  char path[1024];
  snprintf(path, sizeof(path), "%s/list.txt", argv[2]);
  FILE* list = fopen(path, "w");
  if (!list) return 4;
  struct mspack_system sys = {m_open, m_close, m_read, m_write, m_seek, m_tell, m_msg,
                              m_alloc, m_free, m_copy, NULL};
  uint8_t* payload = malloc(64u << 20);
  int n = 0, failed = 0;
  size_t total = 0;
  for (size_t i = 0; i + 32 <= len; ++i) {
    // The sizes: uncompressed, 0, uncompressed, compressed, then the frames.
    const uint8_t* s = d + i;
    if (s[4] | s[5] | s[6] | s[7]) continue;
    uint32_t usize = be32(s), usize2 = be32(s + 8), csize = be32(s + 12);
    if (usize != usize2 || usize < 64 || usize > (48u << 20) || csize < 8 || csize > usize + 4096)
      continue;
    size_t start = i + 16;
    if (start + csize > len) continue;
    size_t plen = Frames(d, start, csize, payload);
    if (!plen) continue;
    uint8_t* out = malloc(usize);
    memfile src = {payload, plen, 0}, dst = {out, usize, 0};
    struct lzxd_stream* z = lzxd_init(&sys, (struct mspack_file*)&src, (struct mspack_file*)&dst,
                                      17, 0, 0x8000, (off_t)usize, 0);
    int err = z ? lzxd_decompress(z, (off_t)usize) : -1;
    if (z) lzxd_free(z);
    fprintf(list, "%zx %u %u %s\n", i, usize, csize, err ? "FAILED" : "ok");
    if (!err && dst.pos == usize) {
      snprintf(path, sizeof(path), "%s/%010zx_%u.bin", argv[2], i, usize);
      FILE* o = fopen(path, "wb");
      fwrite(out, 1, usize, o);
      fclose(o);
      ++n;
      total += usize;
      i = start + csize - 1;
    } else {
      ++failed;
    }
    free(out);
  }
  fclose(list);
  printf("%d resources (%zu MB), %d failed\n", n, total >> 20, failed);
  return 0;
}
