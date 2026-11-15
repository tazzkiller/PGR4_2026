// Local tool (pgr4 project): extracts the resources of a PGR4 .stream2 (Bizarre Creations'
// streamed world data). Each resource is an XCompress LZX stream (HydraHazardLibraries' decoder,
// Unlicense) after a fixed header whose last four big-endian words are the uncompressed size, 0,
// the uncompressed size again and the compressed size; its frames (2-byte big-endian sizes, or
// 0xFF + uncompressed + compressed sizes) follow. Writes <out_dir>/<offset>_<size>.bin and a list.
//   stream2x <file.stream2> <out_dir> [window]
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <hydrahazard-fmt/compression/lzx.h>

static const uint8_t kHeader[] = {0x01, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00,
                                  0x02, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00,
                                  0x02, 0x00, 0x00, 0x00, 0x00, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00,
                                  0x00, 0x01};

static uint32_t be32(const uint8_t* p) {
  return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

int main(int argc, char** argv) {
  if (argc < 3) return 2;
  // The LZX window (XCompress default 128 KB).
  const uint32_t window = argc > 3 ? (uint32_t)strtoul(argv[3], 0, 0) : 131072;
  FILE* f = fopen(argv[1], "rb");
  if (!f) return 3;
  _fseeki64(f, 0, SEEK_END);
  size_t len = (size_t)_ftelli64(f);
  _fseeki64(f, 0, SEEK_SET);
  uint8_t* d = malloc(len);
  if (!d) return 5;
  len = fread(d, 1, len, f);
  fclose(f);
  printf("%zu bytes read\n", len);
  char path[1024];
  snprintf(path, sizeof(path), "%s/list.txt", argv[2]);
  FILE* list = fopen(path, "w");
  if (!list) return 4;
  int n = 0, failed = 0;
  size_t total_out = 0;
  for (size_t i = 0; i + 32 <= len; ++i) {
    // The sizes: uncompressed, 0, uncompressed, compressed, then a plausible first frame.
    const uint8_t* s = d + i;
    if (s[4] | s[5] | s[6] | s[7]) continue;
    uint32_t usize = be32(s), usize2 = be32(s + 8), csize = be32(s + 12);
    if (usize != usize2 || usize < 64 || usize > (64u << 20) || csize < 8 || csize > usize + 4096)
      continue;
    size_t start = i + 16;
    if (start + csize > len) continue;
    uint32_t first = s[16] == 0xFF ? ((uint32_t)s[19] << 8 | s[20]) : ((uint32_t)s[16] << 8 | s[17]);
    if (!first || first > 0x9000 || first > csize) continue;
    uint8_t* out = malloc(usize);
    lzx_state_t* st = lzx_state_create(window);
    // Frame by frame, the decoder's state kept across them.
    size_t q = start, end = start + csize, produced = 0;
    int ok = 1;
    while (q + 2 <= end && produced < usize) {
      size_t c, u = 32768, h = 2;
      if (d[q] == 0xFF) {
        u = (d[q + 1] << 8) | d[q + 2];
        c = (d[q + 3] << 8) | d[q + 4];
        h = 5;
      } else {
        c = (d[q] << 8) | d[q + 1];
      }
      if (!c || q + h + c > end) {
        ok = 0;
        break;
      }
      if (u > usize - produced) u = usize - produced;
      size_t written = 0;
      if (lzx_inflate_with_state(st, d + q, h + c, out + produced, u, &written, 1) < 0 || !written) {
        ok = 0;
        break;
      }
      produced += written;
      q += h + c;
    }
    lzx_state_destroy(st);
    fprintf(list, "%zx %u %u %zu %s\n", i, usize, csize, produced, ok ? "ok" : "FAILED");
    if (ok && produced == usize) {
      snprintf(path, sizeof(path), "%s/%010zx_%u.bin", argv[2], i, usize);
      FILE* o = fopen(path, "wb");
      fwrite(out, 1, usize, o);
      fclose(o);
      ++n;
      total_out += usize;
      i = start + csize - 1;
    } else {
      ++failed;
    }
    free(out);
  }
  fclose(list);
  printf("%d resources (%zu MB), %d failed\n", n, total_out >> 20, failed);
  return 0;
}
