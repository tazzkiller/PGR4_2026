// Local tool (pgr4 project): HydraHazardLibraries' XCompress LZX decoder (Unlicense) tried at each
// offset of a file range: prints the offsets where a chunk decodes, with its decoded size.
//   hhprobe <file> <from> <to> <chunk_max> [intel]
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include <hydrahazard-fmt/compression/lzx.h>

int main(int argc, char** argv) {
  if (argc < 5) return 2;
  FILE* f = fopen(argv[1], "rb");
  if (!f) return 3;
  long long from = strtoll(argv[2], 0, 0), to = strtoll(argv[3], 0, 0);
  size_t chunk = (size_t)strtoll(argv[4], 0, 0);
  int intel = argc > 5 ? atoi(argv[5]) : 1;
  size_t len = (size_t)(to - from) + chunk;
  uint8_t* in = malloc(len);
  _fseeki64(f, from, SEEK_SET);
  len = fread(in, 1, len, f);
  fclose(f);
  uint8_t* out = malloc(0x20000);
  for (long long p = 0; p < to - from && (size_t)p < len; ++p) {
    size_t written = 0;
    size_t avail = len - (size_t)p < chunk ? len - (size_t)p : chunk;
    if (lzx_mem_inflate(in + p, avail, out, 0x10000, &written, intel) == 0 && written >= 4096)
      printf("0x%llx decoded %zu\n", from + p, written);
  }
  return 0;
}
