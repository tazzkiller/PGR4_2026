// Local tool (pgr4 project): decodes XCompress LZX chunks one after another from an offset of a
// file (HydraHazardLibraries' decoder, Unlicense): each chunk's frames (2-byte big-endian sizes,
// or 0xFF + uncompressed + compressed sizes) until a chunk's output reaches chunk_out bytes, the
// window kept across chunks. Writes the output and prints each chunk's input span.
//   hhdump <file> <offset> <max_in> <chunk_out> <out_file> [intel]
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include <hydrahazard-fmt/compression/lzx.h>

int main(int argc, char** argv) {
  if (argc < 6) return 2;
  FILE* f = fopen(argv[1], "rb");
  if (!f) return 3;
  long long off = strtoll(argv[2], 0, 0);
  size_t max_in = (size_t)strtoll(argv[3], 0, 0);
  size_t chunk_out = (size_t)strtoll(argv[4], 0, 0);
  int intel = argc > 6 ? atoi(argv[6]) : 1;
  uint8_t* in = malloc(max_in);
  _fseeki64(f, off, SEEK_SET);
  size_t len = fread(in, 1, max_in, f);
  fclose(f);
  size_t cap = 64u << 20;
  uint8_t* out = malloc(cap);
  size_t out_pos = 0, pos = 0;
  lzx_state_t* st = lzx_state_create(131072);
  int chunks = 0;
  while (pos < len && out_pos + chunk_out <= cap) {
    // This chunk's frames: sum their sizes up to chunk_out of output.
    size_t q = pos, produced = 0;
    while (q + 2 <= len && produced < chunk_out) {
      size_t c, u = 32768, h = 2;
      if (in[q] == 0xFF) {
        if (q + 5 > len) break;
        u = (in[q + 1] << 8) | in[q + 2];
        c = (in[q + 3] << 8) | in[q + 4];
        h = 5;
      } else {
        c = (in[q] << 8) | in[q + 1];
      }
      if (!c || !u || c > 0x9000 || q + h + c > len) break;
      q += h + c;
      produced += u;
    }
    if (q == pos) break;
    size_t written = 0;
    if (lzx_inflate_with_state(st, in + pos, q - pos, out + out_pos, produced, &written, intel) < 0 ||
        !written) {
      // Another resource's stream: a fresh decoder (window included), once.
      lzx_state_destroy(st);
      st = lzx_state_create(131072);
      written = 0;
      if (lzx_inflate_with_state(st, in + pos, q - pos, out + out_pos, produced, &written, intel) < 0 ||
          !written)
        break;
      printf("-- new stream at 0x%llx\n", off + (long long)pos);
    }
    printf("chunk %d: in 0x%llx-0x%llx, out %zu\n", chunks, off + (long long)pos,
           off + (long long)q, written);
    out_pos += written;
    pos = q;
    ++chunks;
  }
  FILE* o = fopen(argv[5], "wb");
  fwrite(out, 1, out_pos, o);
  fclose(o);
  printf("%d chunks, %zu bytes out, input used to 0x%llx\n", chunks, out_pos,
         off + (long long)pos);
  return 0;
}
