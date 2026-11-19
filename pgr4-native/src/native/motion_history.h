// pgr4-native PGR4 native renderer: the previous frame's constants of each draw, for the motion
// vector pass of the upscaler's scene (DxbcShaderTranslator's motion_vectors).
//
// A draw is matched with the previous frame's by its identity (its vertex shader, index buffer
// and index count: the draws of a frame keep these across frames, while per-frame vertex streams
// such as instance transforms move) and, among draws of the same identity, by its order. 2520 of
// the 2522 scene draws of a race frame match (td-265 draw logs). An unmatched draw reads its own
// constants and gets no motion of its own.
//
// Flat open-addressing tables with generation stamps instead of clearing. The constants stay in
// the upload memory they were committed to, which the constant pool keeps for one more frame
// (UploadPool::set_keep_previous_frame).
//
// Threading: the thread that records the draws.
#pragma once

#include <d3d12.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

namespace pgr4::native {

class MotionHistory {
 public:
  struct Constants {
    const void* vs = nullptr;  // the shader the float constants are packed for
    D3D12_GPU_VIRTUAL_ADDRESS float_constants = 0, fetch_constants = 0;
    // Local change (pgr4 project, 16/11/2026): the draw's position (c0-c3's translation), for the
    // nearest match of a moving draw (BindPreviousConstants).
    float pos[3] = {};
    // Local change (16/11/2026): its motion from the previous frame's draw it was matched with
    // (world units a frame; 0 for a still or new one), so the next frame looks for it where it
    // went (BindPreviousConstants).
    float vel[3] = {};
    // Local change (pgr4 project, 17/11/2026): its whole transform (the placement's rows c0-c2,
    // [R | t]) and its change from the previous frame's draw it was matched with (world space;
    // the identity for a still or new one), so a moving draw is looked for where its whole
    // transform went (Predict, Distance), not where its origin went. A part that turns about
    // another point than its origin (a wheel about its axle, its origin at the car's centre)
    // carries its origin round a circle as wide as a wheel base: by its origin, the wheels took
    // each other's previous transforms. `has_xf`: the draw has a placement (not the characters').
    float xf[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
    float delta[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
    bool has_xf = false;
    // Its rank among this frame's draws of the same identity and placement (the passes of one
    // object, drawn with the same transform: a blurred rim's four slices), so each pass keeps
    // its own.
    uint32_t group = 0;
    // Local change (18/11/2026): a part spinning fast about its own x axis (a wheel's tyre, rim,
    // hubcap, disc): the axle's point in its model's y-z plane, averaged over the frames
    // (BindPreviousConstants), and its packed float constants kept for the next frame (StoreBlob;
    // UINT32_MAX for none), whose transform is then given without the spin.
    float axle[2] = {};
    bool has_axle = false;
    uint32_t blob = UINT32_MAX, blob_size = 0;
  };

  // Local change (18/11/2026): bytes kept for one frame (a spinning part's constants).
  uint32_t StoreBlob(const void* data, uint32_t size) {
    if (blobs_[current_blobs_].size() + size > kMaxBlobBytes) return UINT32_MAX;
    const uint32_t offset = uint32_t(blobs_[current_blobs_].size());
    const auto* p = static_cast<const uint8_t*>(data);
    blobs_[current_blobs_].insert(blobs_[current_blobs_].end(), p, p + size);
    return offset;
  }
  // The previous frame's bytes at `offset` (Previous's blob), or nullptr.
  const uint8_t* PreviousBlob(uint32_t offset, uint32_t size) const {
    const auto& b = blobs_[current_blobs_ ^ 1];
    if (offset == UINT32_MAX || size_t(offset) + size > b.size()) return nullptr;
    return b.data() + offset;
  }

  // Local change (pgr4 project, 17/11/2026): 3x4 transforms by rows, [R | t] (p' = R p + t).
  // a after b.
  static void Compose(const float a[12], const float b[12], float out[12]) {
    for (int r = 0; r < 3; ++r) {
      for (int c = 0; c < 3; ++c)
        out[4 * r + c] = a[4 * r] * b[c] + a[4 * r + 1] * b[4 + c] + a[4 * r + 2] * b[8 + c];
      out[4 * r + 3] =
          a[4 * r] * b[3] + a[4 * r + 1] * b[7] + a[4 * r + 2] * b[11] + a[4 * r + 3];
    }
  }
  // The inverse of m; false when m is singular.
  static bool Inverse(const float m[12], float out[12]) {
    const float a = m[0], b = m[1], c = m[2], d = m[4], e = m[5], f = m[6], g = m[8], h = m[9],
                i = m[10];
    const float A = e * i - f * h, B = f * g - d * i, C = d * h - e * g;
    const float det = a * A + b * B + c * C;
    if (!(std::abs(det) > 1e-12f)) return false;
    const float k = 1.0f / det;
    const float r[9] = {A * k, (c * h - b * i) * k, (b * f - c * e) * k,
                        B * k, (a * i - c * g) * k, (c * d - a * f) * k,
                        C * k, (b * g - a * h) * k, (a * e - b * d) * k};
    for (int row = 0; row < 3; ++row) {
      for (int col = 0; col < 3; ++col) out[4 * row + col] = r[3 * row + col];
      out[4 * row + 3] =
          -(r[3 * row] * m[3] + r[3 * row + 1] * m[7] + r[3 * row + 2] * m[11]);
    }
    return true;
  }
  // Where c's transform goes in this frame: its own, moved as it moved in the last.
  static void Predict(const Constants& c, float out[12]) { Compose(c.delta, c.xf, out); }
  // How far apart two transforms put things: the squared distances between the images of the
  // object's origin and of the points a metre along its axes.
  static float Distance(const float a[12], const float b[12]) {
    float d = 0.0f;
    float t[3];
    for (int r = 0; r < 3; ++r) t[r] = a[4 * r + 3] - b[4 * r + 3];
    d += t[0] * t[0] + t[1] * t[1] + t[2] * t[2];
    for (int axis = 0; axis < 3; ++axis) {
      float e = 0.0f;
      for (int r = 0; r < 3; ++r) {
        const float x = a[4 * r + axis] - b[4 * r + axis] + t[r];
        e += x * x;
      }
      d += e;
    }
    return d;
  }

  // Frame `frame`'s first draw: the last frame's draws become the previous frame's, if that was
  // frame - 1 (the constant pool keeps one frame's memory, drawn or not).
  // Local change (18/11/2026): `prefetch`, the next draws' slots read ahead (PrefetchNext).
  void BeginFrame(uint64_t frame, bool prefetch = false, bool prefetch_whole = false) {
    prefetch_ = prefetch;
    prefetch_whole_ = prefetch_whole;
    homes_current_ ^= 1;
    homes_[homes_current_].clear();
    const bool consecutive = frame == frame_ + 1;
    stamp_ += consecutive ? 1 : 2;
    frame_ = frame;
    std::swap(current_, previous_);
    current_used_ = counts_used_ = 0;
    current_blobs_ ^= 1;
    blobs_[current_blobs_].clear();
    if (!consecutive) blobs_[current_blobs_ ^ 1].clear();
  }

  // Local change (18/11/2026, the render thread's load): the frame's draws come in the same
  // order as the last frame's, and each lookup's first read of its slot (one of 65536 slots of
  // 200 bytes, out of the CPU's caches by the next frame) cost about 80 ns, five a draw with
  // motion (xperf, pont de New York 18/11/2026: MotionHistory::Find, 11 % of the render thread).
  // LogHomes keeps the slots a draw's lookups start at, in draw order; PrefetchNext, at the
  // start of a draw, has the CPU fetch the ones the draw kAhead places later touched in the last
  // frame. A prefetch reads nothing into the tables' state: every answer stays the same.
  void LogHomes(uint64_t identity, uint64_t placed_identity, uint64_t key, uint64_t placed_key) {
    if (!prefetch_) return;
    homes_[homes_current_].push_back({uint32_t(Mix(identity)) & (kSlots - 1),
                                      uint32_t(Mix(placed_identity)) & (kSlots - 1),
                                      uint32_t(key) & (kSlots - 1),
                                      uint32_t(placed_key) & (kSlots - 1)});
  }
  void PrefetchNext() {
    if (!prefetch_) return;
    const std::vector<Homes>& last = homes_[homes_current_ ^ 1];
    const size_t k = homes_[homes_current_].size() + kAhead;
    if (k >= last.size()) return;
    const Homes& h = last[k];
    // Key: the identities' counts. Record: this frame's slots, written. Local change
    // (18/11/2026, pgr4_native_motion_prefetch_whole): their whole slots (four lines: Insert
    // writes all of a new one, Key's count is at its end, Record copies the constants).
    const size_t write_bytes = prefetch_whole_ ? sizeof(Slot) : 1;
    for (const char* w : {reinterpret_cast<const char*>(&(*counts_)[h.identity]),
                          reinterpret_cast<const char*>(&(*counts_)[h.placed_identity]),
                          reinterpret_cast<const char*>(&(*current_)[h.key]),
                          reinterpret_cast<const char*>(&(*current_)[h.placed_key])})
      for (size_t offset = 0; offset < write_bytes; offset += 64) __builtin_prefetch(w + offset, 1);
    // Previous: the last frame's slot of the placed key, and its constants (four lines).
    const auto* p = reinterpret_cast<const char*>(&(*previous_)[h.placed_key]);
    for (size_t offset = 0; offset < sizeof(Slot); offset += 64) __builtin_prefetch(p + offset, 0);
  }

  // The key of this frame's next draw with `identity`; *occurrence: its rank among them.
  uint64_t Key(uint64_t identity, uint32_t* occurrence_out = nullptr) {
    const uint64_t id = Mix(identity);
    uint32_t occurrence = 0;
    if (Slot* s = Insert(*counts_, id, counts_used_)) occurrence = s->count++;
    if (occurrence_out) *occurrence_out = occurrence;
    return Mix(id + occurrence);
  }
  // The key of the draw of rank `occurrence` with `identity` (Key's, in any frame).
  static uint64_t KeyAt(uint64_t identity, uint32_t occurrence) {
    return Mix(Mix(identity) + occurrence);
  }

  // The previous frame's constants of the draw with `key`, or nullptr.
  const Constants* Previous(uint64_t key) const {
    const Slot& s = (*previous_)[Find(*previous_, key, stamp_ - 1)];
    return s.stamp == stamp_ - 1 ? &s.constants : nullptr;
  }

  void Record(uint64_t key, const Constants& c) {
    if (Slot* s = Insert(*current_, key, current_used_)) s->constants = c;
  }

  // Local change (pgr4 project, 16/11/2026): the draws of a mesh by where they stand (the crowd,
  // the animated characters: dozens share a mesh, their order changes from frame to frame and
  // their placement with their animation). RecordCell files this frame's draw under its ground
  // cell (x, z, kCell metres; two draws a cell); NearestInCells returns the previous frame's draw
  // of `mesh` and `vs` nearest `pos` in its cell and the 8 around, or nullptr.
  static constexpr float kCell = 0.5f;
  void RecordCell(uint64_t mesh, const float pos[3], const Constants& c) {
    const int32_t cx = int32_t(std::floor(pos[0] / kCell)), cz = int32_t(std::floor(pos[2] / kCell));
    for (uint32_t slot = 0; slot < 2; ++slot) {
      const uint64_t key = CellKey(mesh, cx, cz, slot);
      Slot& s = (*current_)[Find(*current_, key, stamp_)];
      if (s.stamp == stamp_) continue;  // taken by another draw of the mesh in this cell
      if (current_used_ == kMaxUsed) return;
      ++current_used_;
      s = {key, stamp_, c, 0};
      return;
    }
  }
  const Constants* NearestInCells(uint64_t mesh, const float pos[3], const void* vs) const {
    const int32_t cx = int32_t(std::floor(pos[0] / kCell)), cz = int32_t(std::floor(pos[2] / kCell));
    const Constants* best = nullptr;
    float best_d = 0.0f;
    for (int32_t dz = -1; dz <= 1; ++dz)
      for (int32_t dx = -1; dx <= 1; ++dx)
        for (uint32_t slot = 0; slot < 2; ++slot) {
          const Constants* p = Previous(CellKey(mesh, cx + dx, cz + dz, slot));
          if (!p) break;
          if (p->vs != vs) continue;
          const float ex = p->pos[0] - pos[0], ey = p->pos[1] - pos[1], ez = p->pos[2] - pos[2];
          const float d = ex * ex + ey * ey + ez * ez;
          if (!best || d < best_d) {
            best = p;
            best_d = d;
          }
        }
    return best;
  }

 private:
  // A power of two, three times a frame's draws' keys (two each: BindPreviousConstants).
  // Local change (31/10/2026): 65536 (16384 before). Las Vegas draws up to 4 400 a frame, 8 800
  // keys, past the 8 192 the table held: the draws after it were not recorded, and some of the
  // player's car's parts took another draw's previous transform (100-140 px of motion in turns).
  static constexpr uint32_t kSlots = 65536;
  static constexpr uint32_t kMaxUsed = kSlots / 2;
  struct Slot {
    uint64_t key = 0, stamp = 0;
    Constants constants;
    uint32_t count = 0;  // the identity's draws so far (counts_)
  };
  using Table = std::array<Slot, kSlots>;

  static uint64_t CellKey(uint64_t mesh, int32_t cx, int32_t cz, uint32_t slot) {
    return Mix(Mix(mesh ^ 0xC3A5C85C97CB3127ull) +
               ((uint64_t(uint32_t(cx)) << 32 | uint32_t(cz)) * 2 + slot));
  }
  static uint64_t Mix(uint64_t x) {  // the murmur3 finalizer
    x ^= x >> 33;
    x *= 0xFF51AFD7ED558CCDull;
    x ^= x >> 33;
    x *= 0xC4CEB9FE1A85EC53ull;
    return x ^ (x >> 33);
  }
  // The index of `key` among the slots of generation `stamp`, or of the free slot where it goes
  // (linear probing; at most kMaxUsed slots are live, so a free one is near).
  static uint32_t Find(const Table& t, uint64_t key, uint64_t stamp) {
    uint32_t i = uint32_t(key) & (kSlots - 1);
    while (t[i].stamp == stamp && t[i].key != key) i = (i + 1) & (kSlots - 1);
    return i;
  }
  // `key`'s slot of this frame, made if new; nullptr when the table is full.
  Slot* Insert(Table& t, uint64_t key, uint32_t& used) {
    Slot& s = t[Find(t, key, stamp_)];
    if (s.stamp != stamp_) {
      if (used == kMaxUsed) return nullptr;
      ++used;
      s = {key, stamp_, {}, 0};
    }
    return &s;
  }

  // Local change (18/11/2026): LogHomes' slots, this frame's and the last's (homes_current_ ^ 1).
  struct Homes {
    uint32_t identity, placed_identity, key, placed_key;
  };
  static constexpr size_t kAhead = 4;
  std::vector<Homes> homes_[2];
  uint32_t homes_current_ = 0;
  bool prefetch_ = false, prefetch_whole_ = false;

  uint64_t stamp_ = 2;  // the tables start at generation 0: empty
  uint64_t frame_ = UINT64_MAX - 1;
  std::unique_ptr<Table> tables_[3] = {std::make_unique<Table>(), std::make_unique<Table>(),
                                       std::make_unique<Table>()};
  Table* current_ = tables_[0].get();
  Table* previous_ = tables_[1].get();
  Table* counts_ = tables_[2].get();
  uint32_t current_used_ = 0, counts_used_ = 0;
  // Local change (18/11/2026): StoreBlob's two frames (a few dozen spinning parts' constants).
  static constexpr size_t kMaxBlobBytes = 1 << 20;
  std::vector<uint8_t> blobs_[2];
  uint32_t current_blobs_ = 0;
};

}  // namespace pgr4::native
