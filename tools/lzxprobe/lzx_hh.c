// HydraHazardLibraries' LZX decoder (Unlicense, github.com/arthur-manske/HydraHazardLibraries),
// copied with a local change (pgr4 project): the position slots follow the window size.
#include <errno.h>
#include <hydrahazard-fmt/compression/lzx.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LZX_MIN_MATCH 2
#define LZX_MAX_MATCH 257
#define LZX_NUM_CHARS 256
#define LZX_NUM_PRIMARY_LENGTHS 7
#define LZX_NUM_SECONDARY_LENGTHS 249
#define LZX_BLOCKTYPE_VERBATIM 1
#define LZX_BLOCKTYPE_ALIGNED 2
#define LZX_BLOCKTYPE_UNCOMPRESSED 3
#define LZX_LEAF_MARKER 0x7FFFFFFF
#define LZX_MAX_NODES 4096

static const int extra_bits[52] = {0,  0,  0,  0,  1,  1,  2,  2,  3,  3,  4,  4,  5,  5,  6,  6,  7,  7,
                                   8,  8,  9,  9,  10, 10, 11, 11, 12, 12, 13, 13, 14, 14, 15, 15, 16, 16,
                                   17, 17, 17, 17, 17, 17, 17, 17, 17, 17, 17, 17, 17, 17, 17, 17};

static const uint32_t position_base[52] = {
	0,       1,       2,       3,       4,       6,       8,       12,     16,     24,      32,
	48,      64,      96,      128,     192,     256,     384,     512,    768,    1024,    1536,
	2048,    3072,    4096,    6144,    8192,    12288,   16384,   24576,  32768,  49152,   65536,
	98304,   131072,  196608,  262144,  393216,  524288,  655360,  786432, 917504, 1048576, 1179648,
	1310720, 1441792, 1572864, 1703936, 1835008, 1966080, 2097152, 2228224};

struct bit_reader {
	const uint8_t *in;
	size_t         sz;
	size_t         pos;
	uint32_t       buf;
	int            bl;
};

static inline unsigned int rb(struct bit_reader *b, int n)
{
	if (n == 0) return 0;
	while (b->bl < n) {
		if (b->pos + 1 < b->sz) {
			uint32_t w = b->in[b->pos] | (b->in[b->pos + 1] << 8);
			b->pos += 2;
			b->buf = (b->buf << 16) | w;
			b->bl += 16;
		} else if (b->pos < b->sz) {
			b->buf = (b->buf << 8) | b->in[b->pos++];
			b->bl += 8;
		} else
			break;
	}
	unsigned int v = (b->buf >> (b->bl - n)) & ((1u << n) - 1);
	b->bl -= n;
	return v;
}

struct hn {
	unsigned int c0;
	unsigned int c1;
};

static int build_tree(struct hn *tree, unsigned char *lens, int ns)
{
	if (!tree || !lens || ns <= 0) return -1;
	unsigned int c[17], s[17], code;
	int          nn = 1;
	memset(c, 0, sizeof(c));
	for (int i = 0; i < ns; i++)
		if (lens[i] > 0 && lens[i] <= 16) c[lens[i]]++;
	s[1] = 0;
	for (int i = 1; i < 16; i++) {
		s[i + 1] = (s[i] + c[i]) << 1;
	}
	tree[0].c0 = 0;
	tree[0].c1 = 0;
	for (int sym = 0; sym < ns; sym++) {
		int len = lens[sym];
		if (len > 0 && len <= 16) {
			code = s[len]++;
			if (code >= (1u << len)) return -1;
			unsigned int node = 0;
			for (int i = len - 1; i >= 0; i--) {
				if (node >= LZX_MAX_NODES) return -1;
				unsigned int  bit = (code >> i) & 1;
				unsigned int *ch  = bit ? &tree[node].c1 : &tree[node].c0;
				if (i == 0) {
					if (nn >= LZX_MAX_NODES) return -1;
					*ch          = (unsigned int)nn++;
					tree[*ch].c0 = (unsigned int)sym;
					tree[*ch].c1 = LZX_LEAF_MARKER;
				} else {
					if (*ch == 0) {
						if (nn >= LZX_MAX_NODES) return -1;
						*ch          = (unsigned int)nn++;
						tree[*ch].c0 = 0;
						tree[*ch].c1 = 0;
					}
					node = *ch;
				}
			}
		}
	}
	return nn;
}

static int hd(struct bit_reader *b, struct hn *tree)
{
	unsigned int node = 0;
	for (int depth = 0; depth < 32; depth++) {
		unsigned int bit = rb(b, 1);
		if (node >= LZX_MAX_NODES) return -1;
		unsigned int ch = bit ? tree[node].c1 : tree[node].c0;
		if (ch == 0 || ch >= LZX_MAX_NODES) return -1;
		if (tree[ch].c1 == LZX_LEAF_MARKER) return (int)tree[ch].c0;
		node = ch;
	}
	return -1;
}

static int rl(struct bit_reader *b, unsigned char *lens, int first, int last, struct hn *ptree)
{
	unsigned char pt[20];
	for (int i = 0; i < 20; i++) {
		pt[i] = (unsigned char)rb(b, 4);
	}
	if (build_tree(ptree, pt, 20) < 0) return -1;
	int x = first;
	while (x < last) {
		int z = hd(b, ptree);
		if (z < 0) return -1;
		if (z == 17) {
			unsigned int y = rb(b, 4) + 4;
			while (y-- && x < last) lens[x++] = 0;
		} else if (z == 18) {
			unsigned int y = rb(b, 5) + 20;
			while (y-- && x < last) lens[x++] = 0;
		} else if (z == 19) {
			unsigned int y     = rb(b, 1) + 4;
			int          delta = hd(b, ptree);
			if (delta < 0) return -1;
			int d = (int)lens[x] - delta;
			if (d < 0) d += 17;
			while (y-- && x < last) lens[x++] = (unsigned char)d;
		} else {
			int d = (int)lens[x] - z;
			if (d < 0) d += 17;
			lens[x++] = (unsigned char)d;
		}
	}
	return 0;
}

struct lzx_state {
	struct bit_reader br;
	struct hn     mt_tree[LZX_MAX_NODES], lt_tree[LZX_MAX_NODES], al_tree[LZX_MAX_NODES], pt_work[LZX_MAX_NODES];
	uint8_t      *out;
	size_t        out_sz;
	size_t        out_pos;
	unsigned int  win_sz, win_pos, frame, R0, R1, R2, block_rem, block_len;
	int           block_type, header_read;
	unsigned int  num_off;
	uint8_t      *window;
	uint32_t      intel_filesize;
	int           intel_active;
	unsigned char MT_len[720], LEN_len[320], AL_len[12];
	int           LEN_empty;
	unsigned int  wmask;
	size_t        intel_offset;
};

static void rst(struct lzx_state *st)
{
	st->R0 = st->R1 = st->R2 = 1;
	st->block_rem = st->block_type = st->block_len = 0;
	st->header_read                                = 0;
	st->win_pos = st->frame = st->out_pos = 0;
	st->intel_offset                      = 0;
	st->br.buf                            = 0;
	st->br.bl                             = 0;
	st->wmask                             = st->win_sz - 1;
	st->intel_filesize                    = 0x00FFFFFF;
	st->intel_active                      = 0;
	memset(st->MT_len, 0, sizeof(st->MT_len));
	memset(st->LEN_len, 0, sizeof(st->LEN_len));
	memset(st->AL_len, 0, sizeof(st->AL_len));
	memset(st->mt_tree, 0, sizeof(st->mt_tree));
	memset(st->lt_tree, 0, sizeof(st->lt_tree));
	memset(st->al_tree, 0, sizeof(st->al_tree));
	memset(st->pt_work, 0, sizeof(st->pt_work));
	if (st->window) memset(st->window, 0, st->win_sz);
}

static void apply_e8(uint8_t *data, size_t size, uint32_t offset, uint32_t filesize)
{
	if (size <= 10) return;
	size_t end = size - 10;
	for (size_t i = 0; i < end;) {
		if (data[i] == 0xE8) {
			uint32_t b0 = data[i + 1], b1 = data[i + 2], b2 = data[i + 3], b3 = data[i + 4];
			uint32_t abs_off = b0 | (b1 << 8) | (b2 << 16) | (b3 << 24);
			uint32_t rel_off;
			if (abs_off < filesize) {
				rel_off = abs_off - (offset + (uint32_t)i);
			} else if (abs_off >= 0x80000000 && abs_off < 0x80000000 + filesize) {
				rel_off = abs_off + filesize;
			} else {
				i++;
				continue;
			}
			data[i + 1] = (uint8_t)(rel_off & 0xFF);
			data[i + 2] = (uint8_t)((rel_off >> 8) & 0xFF);
			data[i + 3] = (uint8_t)((rel_off >> 16) & 0xFF);
			data[i + 4] = (uint8_t)((rel_off >> 24) & 0xFF);
			i += 5;
		} else {
			i++;
		}
	}
}

static int dec_frame(struct lzx_state *st, size_t uncomp_sz)
{
	struct bit_reader *b  = &st->br;
	unsigned int       R0 = st->R0, R1 = st->R1, R2 = st->R2, wp = st->win_pos, wm = st->wmask;
	size_t             frame_start_pos = st->out_pos;
	size_t             target_pos      = st->out_pos + uncomp_sz;
	if (target_pos > st->out_sz) target_pos = st->out_sz;

	if (st->header_read) {
		if (rb(b, 1)) {
			uint32_t hi        = rb(b, 16);
			uint32_t lo        = rb(b, 16);
			st->intel_filesize = (hi << 16) | lo;
			st->intel_active   = 1;
		}
		st->header_read = 0; // Only read once per context
	}

	while (st->out_pos < target_pos) {
		if (st->block_rem == 0) {
			// New LZX block header
			st->block_type = (int)rb(b, 3);
			st->block_rem = st->block_len = rb(b, 24);
			if (st->block_type < 1 || st->block_type > 3) return -1;

			switch (st->block_type) {
			case LZX_BLOCKTYPE_ALIGNED:
				for (int i = 0; i < 8; i++) st->AL_len[i] = (unsigned char)rb(b, 3);
				if (build_tree(st->al_tree, st->AL_len, 8) < 0) return -1;
				/* fallthrough */
			case LZX_BLOCKTYPE_VERBATIM:
				if (rl(b, st->MT_len, 0, 256, st->pt_work) < 0) return -1;
				if (rl(b, st->MT_len, 256, 256 + st->num_off * 8, st->pt_work) < 0) return -1;
				if (build_tree(st->mt_tree, st->MT_len, 256 + st->num_off * 8) < 0) return -1;
				if (rl(b, st->LEN_len, 0, LZX_NUM_SECONDARY_LENGTHS, st->pt_work) < 0) return -1;
				st->LEN_empty = (build_tree(st->lt_tree, st->LEN_len, LZX_NUM_SECONDARY_LENGTHS) < 0);
				if (st->block_type == LZX_BLOCKTYPE_VERBATIM) {
					memset(st->AL_len, 0, sizeof(st->AL_len));
					build_tree(st->al_tree, st->AL_len, 8);
				}
				break;
			case LZX_BLOCKTYPE_UNCOMPRESSED:
				if (b->bl & 15) rb(b, b->bl & 15);
				R0 = rb(b, 16);
				R0 |= (unsigned int)rb(b, 16) << 16;
				R1 = rb(b, 16);
				R1 |= (unsigned int)rb(b, 16) << 16;
				R2 = rb(b, 16);
				R2 |= (unsigned int)rb(b, 16) << 16;
				break;
			}
		}

		if (st->block_type == LZX_BLOCKTYPE_UNCOMPRESSED) {
			unsigned char c;
			if (b->bl == 16) {
				c = (unsigned char)(b->buf & 0xFF);
				b->buf >>= 8;
				b->bl -= 8;
			} else if (b->bl == 8) {
				c      = (unsigned char)(b->buf & 0xFF);
				b->buf = 0;
				b->bl  = 0;
			} else {
				c = b->in[b->pos++];
			}
			st->window[wp]         = c;
			wp                     = (wp + 1) & wm;
			st->out[st->out_pos++] = c;
			st->block_rem--;
			if (st->block_rem == 0) {
				if (b->bl & 15) {
					b->bl  = 0;
					b->buf = 0;
				}
				if (b->pos & 1) b->pos++; // Ensure 16-bit alignment in the stream
			}
		} else {
			int me = hd(b, st->mt_tree);
			if (me < 0) return -1;
			if (me < LZX_NUM_CHARS) {
				unsigned char c        = (unsigned char)me;
				st->window[wp]         = c;
				wp                     = (wp + 1) & wm;
				st->out[st->out_pos++] = c;
				st->block_rem--;
			} else {
				me -= LZX_NUM_CHARS;
				unsigned int ml = (unsigned int)me & 7;
				if (ml == 7) {
					if (st->LEN_empty) return -1;
					int ex = hd(b, st->lt_tree);
					if (ex < 0) return -1;
					ml += (unsigned int)ex;
				}
				ml += LZX_MIN_MATCH;
				unsigned int mo, slot = (unsigned int)me >> 3;
				if (slot == 0)
					mo = R0;
				else if (slot == 1) {
					mo = R1;
					R1 = R0;
					R0 = mo;
				} else if (slot == 2) {
					mo = R2;
					R2 = R0;
					R0 = mo;
				} else {
					int ex = slot >= 50 ? 17 : extra_bits[slot];
					mo     = position_base[slot] - 2;
					if (ex >= 3 && st->block_type == LZX_BLOCKTYPE_ALIGNED) {
						if (ex > 3) mo += rb(b, ex - 3) << 3;
						int as = hd(b, st->al_tree);
						if (as < 0) return -1;
						mo += (unsigned int)as;
					} else if (ex > 0)
						mo += rb(b, ex);
					R2 = R1;
					R1 = R0;
					R0 = mo;
				}
				while (ml-- && st->out_pos < target_pos) {
					unsigned char c        = st->window[(wp - mo) & wm];
					st->window[wp]         = c;
					wp                     = (wp + 1) & wm;
					st->out[st->out_pos++] = c;
					st->block_rem--;
				}
			}
		}
	}

	if (st->intel_active && st->out_pos > frame_start_pos) {
		apply_e8(st->out + frame_start_pos, st->out_pos - frame_start_pos, st->intel_offset,
		         st->intel_filesize);
	}
	st->intel_offset += st->out_pos - frame_start_pos;

	st->win_pos = wp;
	st->R0      = R0;
	st->R1      = R1;
	st->R2      = R2;
	return 0;
}

lzx_state_t *lzx_state_create(uint32_t window_size)
{
	struct lzx_state *st = calloc(1, sizeof(*st));
	if (!st) return NULL;
	st->win_sz = window_size;
	st->window = calloc(1, window_size);
	if (!st->window) {
		free(st);
		return NULL;
	}
	// Local change (pgr4 project): the position slots of the window (LZX: 2^15 30, 2^16 32,
	// 2^17 34, 2^18 36, 2^19 38, 2^20 42, 2^21 50), not always 34.
	unsigned int bits = 0;
	while ((1u << bits) < window_size) bits++;
	st->num_off = bits == 20 ? 42 : bits == 21 ? 50 : bits * 2;
	rst(st);
	return st;
}

void lzx_state_destroy(lzx_state_t *st)
{
	if (st) {
		if (st->window) free(st->window);
		free(st);
	}
}

int lzx_inflate_with_state(lzx_state_t *st, const uint8_t *in, size_t in_size, uint8_t *out, size_t out_size,
                           size_t *bytes_written, int has_intel_header)
{
	if (!st) return -1;

	st->out          = out;
	st->out_sz       = out_size;
	st->out_pos      = 0;
	st->header_read  = has_intel_header;
	st->intel_active = 0;
	st->intel_offset = 0; // Reset offset for new stream

	// Clear the Huffman lengths for a new chunk
	memset(st->MT_len, 0, sizeof(st->MT_len));
	memset(st->LEN_len, 0, sizeof(st->LEN_len));
	memset(st->AL_len, 0, sizeof(st->AL_len));

	st->R0 = 1;
	st->R1 = 1;
	st->R2 = 1;

	size_t in_pos = 0;

	// XCompress Frame loop
	while (in_pos < in_size && st->out_pos < out_size) {
		uint32_t comp_sz, uncomp_sz;

		// Parse XCompress frame header
		if (in[in_pos] == 0xFF) {
			if (in_pos + 5 > in_size) break;
			uncomp_sz = (in[in_pos + 1] << 8) | in[in_pos + 2];
			comp_sz   = (in[in_pos + 3] << 8) | in[in_pos + 4];
			in_pos += 5;
		} else {
			if (in_pos + 2 > in_size) break;
			uncomp_sz = 32768; // Default 32KB
			comp_sz   = (in[in_pos] << 8) | in[in_pos + 1];
			in_pos += 2;
		}

		if (comp_sz == 0 || in_pos + comp_sz > in_size) break;

		// Prepare bit reader for this specific frame
		st->br.in  = in;
		st->br.sz  = in_pos + comp_sz;
		st->br.pos = in_pos;
		st->br.bl  = 0;
		st->br.buf = 0;

		if (dec_frame(st, uncomp_sz) < 0) {
			return -1;
		}

		in_pos += comp_sz;
	}

	if (bytes_written) *bytes_written = st->out_pos;
	return 0;
}

void lzx_state_set_be(lzx_state_t *st, int be)
{
	(void)st;
	(void)be;
}

int lzx_mem_inflate(const uint8_t *in, size_t in_size, uint8_t *out, size_t out_size, size_t *bytes_written,
                    int has_intel_header)
{
	lzx_state_t *st = lzx_state_create(131072);
	if (!st) return -1;
	int ret = lzx_inflate_with_state(st, in, in_size, out, out_size, bytes_written, has_intel_header);
	lzx_state_destroy(st);
	return ret;
}

int lzx_mem_deflate(const uint8_t *in, size_t in_size, uint8_t *out, size_t *out_size, unsigned int window_bits)
{
	(void)in;
	(void)in_size;
	(void)out;
	(void)out_size;
	(void)window_bits;
	return -1;
}
