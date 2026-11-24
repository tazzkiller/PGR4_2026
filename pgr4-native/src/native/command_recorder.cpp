// pgr4-native PGR4 native renderer: the frame list's calls replayed on a thread of their own
// (local addition, 18/11/2026; command_recorder.h).
#include "command_recorder.h"

#include "clock.h"
#include "log.h"

#include <windows.h>

#include <cstring>
#include <system_error>

#pragma comment(lib, "Synchronization.lib")  // WaitOnAddress, WakeByAddressSingle

namespace pgr4::native {

namespace {

std::atomic<CommandRecorder*> g_recorder{nullptr};

// How long each side spins before it sleeps: the replay thread between two published batches
// (a batch every few draws), the recording thread on a drain (a batch's replay).
constexpr uint64_t kWorkerSpinNs = 10000;
constexpr uint64_t kDrainSpinNs = 50000;

// Commands' arguments. Those followed by an array are a multiple of 8 bytes, so the array is
// aligned as the driver reads it.
struct DrawIndexedArgs {
  UINT indices, instances, first_index;
  INT base_vertex;
  UINT first_instance, pad;
};
struct CopyBufferArgs {
  ID3D12Resource* dst;
  UINT64 dst_offset;
  ID3D12Resource* src;
  UINT64 src_offset, bytes;
};
struct CopyTextureArgs {
  D3D12_TEXTURE_COPY_LOCATION dst, src;
  UINT x, y, z;
  BOOL has_box;
  D3D12_BOX box;
};
struct CopyResourceArgs {
  ID3D12Resource *dst, *src;
};
struct CountArgs {  // then the array
  UINT n, pad;
};
struct BlendArgs {
  FLOAT factor[4];
  UINT has, pad;
};
struct TableArgs {
  UINT index, pad;
  D3D12_GPU_DESCRIPTOR_HANDLE table;
};
struct ConstantsArgs {  // then the values
  UINT index, n, offset, pad;
};
struct CbvArgs {
  UINT index, pad;
  D3D12_GPU_VIRTUAL_ADDRESS at;
};
struct IndexBufferArgs {
  D3D12_INDEX_BUFFER_VIEW view;
  UINT has, pad;
};
struct TargetsArgs {  // then the RTV handles
  UINT n, rtv_count;
  BOOL single_range, has_dsv;
  D3D12_CPU_DESCRIPTOR_HANDLE dsv;
};
struct ClearDsvArgs {  // then the rects
  D3D12_CPU_DESCRIPTOR_HANDLE dsv;
  D3D12_CLEAR_FLAGS flags;
  FLOAT depth;
  UINT stencil, n;
};
struct ClearRtvArgs {  // then the rects
  D3D12_CPU_DESCRIPTOR_HANDLE rtv;
  FLOAT color[4];
  UINT n, pad;
};
struct QueryArgs {
  ID3D12QueryHeap* heap;
  D3D12_QUERY_TYPE type;
  UINT index;
};
struct SubmitArgs {
  ID3D12CommandQueue* queue;
  ID3D12CommandAllocator* allocator;
};
struct ResolveQueryArgs {
  ID3D12QueryHeap* heap;
  D3D12_QUERY_TYPE type;
  UINT start, n, pad;
  ID3D12Resource* dst;
  UINT64 offset;
};
static_assert(sizeof(CountArgs) % 8 == 0 && sizeof(ConstantsArgs) % 8 == 0 &&
              sizeof(TargetsArgs) % 8 == 0 && sizeof(ClearDsvArgs) % 8 == 0 &&
              sizeof(ClearRtvArgs) % 8 == 0);

template <typename T>
T Read(const uint8_t* p) {
  T v;
  std::memcpy(&v, p, sizeof(T));
  return v;
}

void SpinPause() { YieldProcessor(); }

}  // namespace

CommandRecorder::~CommandRecorder() {
  if (g_recorder.load() == this) g_recorder.store(nullptr);
  if (!started_) return;
  Put(kStop, nullptr, 0);
  Publish();
  if (worker_.joinable()) worker_.join();
}

bool CommandRecorder::Start(ID3D12GraphicsCommandList* real) {
  if (started_) return true;
  real_ = real;
  if (FAILED(real->QueryInterface(IID_PPV_ARGS(&list9_)))) list9_.Reset();
  ring_ = std::make_unique<uint8_t[]>(kRingBytes);
  try {
    worker_ = std::thread([this] { Worker(); });
  } catch (const std::system_error& e) {
    REXLOG_ERROR("PGR4 native recorder: the replay thread failed to start: {}", e.what());
    ring_.reset();
    return false;
  }
  started_ = true;
  g_recorder.store(this);
  REXLOG_INFO("PGR4 native recorder: replay thread started ({} MB ring{})", kRingBytes >> 20,
              list9_ ? "" : ", no RSSetDepthBias");
  return true;
}

CommandRecorder* CommandRecorder::From(ID3D12GraphicsCommandList* list) {
  CommandRecorder* r = g_recorder.load(std::memory_order_relaxed);
  return r && list == static_cast<ID3D12GraphicsCommandList*>(r) ? r : nullptr;
}

void DrainCommandRecording() {
  if (CommandRecorder* r = g_recorder.load(std::memory_order_acquire)) r->Drain();
}

void CommandRecorder::Put(Op op, const void* a, uint32_t na, const void* b, uint32_t nb) {
  // Local change (24/11/2026, branch cpu_thread): on this thread's side, in the ring's format.
  if (Side* side = t_side_) [[unlikely]] {
    const uint64_t size = (8 + uint64_t(na) + nb + 7) & ~uint64_t(7);
    std::vector<uint8_t>& v = *side->bytes;
    const size_t at = v.size();
    v.resize(at + size);
    uint8_t* p = v.data() + at;
    const uint32_t header[2] = {uint32_t(op), uint32_t(size)};
    std::memcpy(p, header, sizeof(header));
    if (na) std::memcpy(p + 8, a, na);
    if (nb) std::memcpy(p + 8 + na, b, nb);
    return;
  }
  const DWORD tid = GetCurrentThreadId();
  if (producer_tid_.load(std::memory_order_relaxed) != tid)
    producer_tid_.store(tid, std::memory_order_relaxed);
  const uint64_t size = (8 + uint64_t(na) + nb + 7) & ~uint64_t(7);
  uint64_t pos = write_ & (kRingBytes - 1);
  const uint64_t skip = pos + size > kRingBytes ? kRingBytes - pos : 0;
  const uint64_t end = write_ + skip + size;
  if (end - known_tail_ > kRingBytes) {
    // The ring is full: the replay frees it.
    Publish();
    WaitReplayed(end - kRingBytes);
    known_tail_ = tail_.load(std::memory_order_acquire);
  }
  if (skip) {
    const uint32_t header[2] = {kWrap, uint32_t(skip)};
    std::memcpy(ring_.get() + pos, header, sizeof(header));
    write_ += skip;
    pos = 0;
  }
  uint8_t* p = ring_.get() + pos;
  const uint32_t header[2] = {uint32_t(op), uint32_t(size)};
  std::memcpy(p, header, sizeof(header));
  if (na) std::memcpy(p + 8, a, na);
  if (nb) std::memcpy(p + 8 + na, b, nb);
  write_ += size;
  stats_.bytes += size;
  if (write_ - published_ >= publish_bytes_ &&
      (op == kDrawInstanced || op == kDrawIndexed || op == kDispatch || op == kCopyTexture ||
       op == kCopyBuffer || op == kCopyResource))
    Publish();
}

void CommandRecorder::Publish() {
  if (write_ == published_) return;
  published_ = write_;
  head_.store(write_, std::memory_order_seq_cst);
  if (worker_sleeping_.load(std::memory_order_seq_cst)) WakeByAddressSingle(&head_);
}

void CommandRecorder::WaitReplayed(uint64_t position) {
  uint64_t t = tail_.load(std::memory_order_acquire);
  if (t >= position) return;
  const uint64_t start = Ticks();
  while (t < position && TicksToNs(Ticks() - start) < kDrainSpinNs) {
    for (int i = 0; i < 16; ++i) SpinPause();
    t = tail_.load(std::memory_order_acquire);
  }
  while (t < position) {
    producer_waiting_.store(1, std::memory_order_seq_cst);
    t = tail_.load(std::memory_order_seq_cst);
    if (t < position) WaitOnAddress(&tail_, &t, sizeof(t), INFINITE);
    t = tail_.load(std::memory_order_acquire);
  }
  producer_waiting_.store(0, std::memory_order_relaxed);
}

void CommandRecorder::Splice(const uint8_t* data, size_t bytes) {
  for (size_t i = 0; i + 8 <= bytes;) {
    uint32_t header[2];
    std::memcpy(header, data + i, sizeof(header));
    if (header[1] < 8 || i + header[1] > bytes) break;  // never: Put wrote it
    Put(Op(header[0]), data + i + 8, header[1] - 8);
    i += header[1];
  }
}

void CommandRecorder::Drain() {
  if (!started_) return;
  // Local change (24/11/2026, branch cpu_thread): recording on the side, the ring's producer's
  // packets before first, then this side's commands so far (Side::flush).
  if (Side* side = t_side_; side && side->flush) side->flush(side->context);
  if (GetCurrentThreadId() != producer_tid_.load(std::memory_order_relaxed)) {
    // Not the recording thread: what it published.
    WaitReplayed(head_.load(std::memory_order_acquire));
    return;
  }
  if (write_ == drained_) return;
  Publish();
  const uint64_t t0 = Ticks();
  WaitReplayed(write_);
  stats_.wait_ns += TicksToNs(Ticks() - t0);
  ++stats_.drains;
  drained_ = known_tail_ = write_;
}

CommandRecorder::Stats CommandRecorder::TakeStats() {
  Stats s = stats_;
  s.replay_ns = replay_ns_.exchange(0, std::memory_order_relaxed);
  stats_ = {};
  return s;
}

void CommandRecorder::Forwarded(const char* method) {
  Drain();
  ++stats_.forwarded;
  // Once per method (__func__ is one string per function).
  for (const char*& seen : forwarded_seen_) {
    if (seen == method) return;
    if (!seen) {
      seen = method;
      REXLOG_INFO("PGR4 native recorder: {} on the recording list: drained, then the real list's",
                  method);
      return;
    }
  }
}

HRESULT STDMETHODCALLTYPE CommandRecorder::QueryInterface(REFIID riid, void** object) {
  if (!object) return E_POINTER;
  if (riid == __uuidof(IUnknown) || riid == __uuidof(ID3D12Object) ||
      riid == __uuidof(ID3D12DeviceChild) || riid == __uuidof(ID3D12CommandList) ||
      riid == __uuidof(ID3D12GraphicsCommandList)) {
    *object = static_cast<ID3D12GraphicsCommandList*>(this);
    return S_OK;
  }
  // A later interface of the real list: what is recorded after it would land before what the
  // caller then records on it directly. Logged; the draw path has none (RSSetDepthBias here).
  Forwarded(__func__);
  return real_->QueryInterface(riid, object);
}

// ---------------------------------------------------------------------------------------------
// Recording.

void STDMETHODCALLTYPE CommandRecorder::DrawInstanced(UINT vertices, UINT instances,
                                                      UINT first_vertex, UINT first_instance) {
  const UINT a[4] = {vertices, instances, first_vertex, first_instance};
  Put(kDrawInstanced, a, sizeof(a));
}

void STDMETHODCALLTYPE CommandRecorder::DrawIndexedInstanced(UINT indices, UINT instances,
                                                             UINT first_index, INT base_vertex,
                                                             UINT first_instance) {
  const DrawIndexedArgs a = {indices, instances, first_index, base_vertex, first_instance, 0};
  Put(kDrawIndexed, &a, sizeof(a));
}

void STDMETHODCALLTYPE CommandRecorder::Dispatch(UINT x, UINT y, UINT z) {
  const UINT a[3] = {x, y, z};
  Put(kDispatch, a, sizeof(a));
}

void STDMETHODCALLTYPE CommandRecorder::CopyBufferRegion(ID3D12Resource* dst, UINT64 dst_offset,
                                                         ID3D12Resource* src, UINT64 src_offset,
                                                         UINT64 bytes) {
  const CopyBufferArgs a = {dst, dst_offset, src, src_offset, bytes};
  Put(kCopyBuffer, &a, sizeof(a));
}

void STDMETHODCALLTYPE CommandRecorder::CopyTextureRegion(const D3D12_TEXTURE_COPY_LOCATION* dst,
                                                          UINT x, UINT y, UINT z,
                                                          const D3D12_TEXTURE_COPY_LOCATION* src,
                                                          const D3D12_BOX* box) {
  CopyTextureArgs a = {*dst, *src, x, y, z, box != nullptr, {}};
  if (box) a.box = *box;
  Put(kCopyTexture, &a, sizeof(a));
}

void STDMETHODCALLTYPE CommandRecorder::CopyResource(ID3D12Resource* dst, ID3D12Resource* src) {
  const CopyResourceArgs a = {dst, src};
  Put(kCopyResource, &a, sizeof(a));
}

void STDMETHODCALLTYPE CommandRecorder::IASetPrimitiveTopology(D3D12_PRIMITIVE_TOPOLOGY topology) {
  Put(kTopology, &topology, sizeof(topology));
}

void STDMETHODCALLTYPE CommandRecorder::RSSetViewports(UINT n, const D3D12_VIEWPORT* viewports) {
  const CountArgs a = {n, 0};
  Put(kViewports, &a, sizeof(a), viewports, uint32_t(n * sizeof(D3D12_VIEWPORT)));
}

void STDMETHODCALLTYPE CommandRecorder::RSSetScissorRects(UINT n, const D3D12_RECT* rects) {
  const CountArgs a = {n, 0};
  Put(kScissors, &a, sizeof(a), rects, uint32_t(n * sizeof(D3D12_RECT)));
}

void STDMETHODCALLTYPE CommandRecorder::OMSetBlendFactor(const FLOAT factor[4]) {
  BlendArgs a = {{}, factor != nullptr, 0};
  if (factor) std::memcpy(a.factor, factor, sizeof(a.factor));
  Put(kBlendFactor, &a, sizeof(a));
}

void STDMETHODCALLTYPE CommandRecorder::OMSetStencilRef(UINT ref) {
  Put(kStencilRef, &ref, sizeof(ref));
}

void STDMETHODCALLTYPE CommandRecorder::SetPipelineState(ID3D12PipelineState* pso) {
  Put(kPipeline, &pso, sizeof(pso));
}

void STDMETHODCALLTYPE CommandRecorder::ResourceBarrier(UINT n,
                                                        const D3D12_RESOURCE_BARRIER* barriers) {
  const CountArgs a = {n, 0};
  Put(kBarriers, &a, sizeof(a), barriers, uint32_t(n * sizeof(D3D12_RESOURCE_BARRIER)));
}

void STDMETHODCALLTYPE CommandRecorder::SetDescriptorHeaps(UINT n,
                                                           ID3D12DescriptorHeap* const* heaps) {
  const CountArgs a = {n, 0};
  Put(kHeaps, &a, sizeof(a), heaps, uint32_t(n * sizeof(ID3D12DescriptorHeap*)));
}

void STDMETHODCALLTYPE CommandRecorder::SetComputeRootSignature(ID3D12RootSignature* root) {
  Put(kComputeRoot, &root, sizeof(root));
}

void STDMETHODCALLTYPE CommandRecorder::SetGraphicsRootSignature(ID3D12RootSignature* root) {
  Put(kGraphicsRoot, &root, sizeof(root));
}

void STDMETHODCALLTYPE CommandRecorder::SetComputeRootDescriptorTable(
    UINT index, D3D12_GPU_DESCRIPTOR_HANDLE table) {
  const TableArgs a = {index, 0, table};
  Put(kComputeTable, &a, sizeof(a));
}

void STDMETHODCALLTYPE CommandRecorder::SetGraphicsRootDescriptorTable(
    UINT index, D3D12_GPU_DESCRIPTOR_HANDLE table) {
  const TableArgs a = {index, 0, table};
  Put(kGraphicsTable, &a, sizeof(a));
}

void STDMETHODCALLTYPE CommandRecorder::SetComputeRoot32BitConstants(UINT index, UINT n,
                                                                     const void* data,
                                                                     UINT offset) {
  const ConstantsArgs a = {index, n, offset, 0};
  Put(kComputeConstants, &a, sizeof(a), data, n * 4);
}

void STDMETHODCALLTYPE CommandRecorder::SetGraphicsRoot32BitConstants(UINT index, UINT n,
                                                                      const void* data,
                                                                      UINT offset) {
  const ConstantsArgs a = {index, n, offset, 0};
  Put(kGraphicsConstants, &a, sizeof(a), data, n * 4);
}

void STDMETHODCALLTYPE CommandRecorder::SetGraphicsRootConstantBufferView(
    UINT index, D3D12_GPU_VIRTUAL_ADDRESS at) {
  const CbvArgs a = {index, 0, at};
  Put(kGraphicsCbv, &a, sizeof(a));
}

void STDMETHODCALLTYPE CommandRecorder::IASetIndexBuffer(const D3D12_INDEX_BUFFER_VIEW* view) {
  IndexBufferArgs a = {{}, view != nullptr, 0};
  if (view) a.view = *view;
  Put(kIndexBuffer, &a, sizeof(a));
}

void STDMETHODCALLTYPE CommandRecorder::OMSetRenderTargets(
    UINT n, const D3D12_CPU_DESCRIPTOR_HANDLE* rtvs, BOOL single_range,
    const D3D12_CPU_DESCRIPTOR_HANDLE* dsv) {
  // One handle for a single range (the first of n consecutive descriptors), else one each.
  const UINT count = !rtvs ? 0 : single_range ? (n ? 1 : 0) : n;
  TargetsArgs a = {n, count, single_range, dsv != nullptr, {}};
  if (dsv) a.dsv = *dsv;
  Put(kTargets, &a, sizeof(a), rtvs, uint32_t(count * sizeof(D3D12_CPU_DESCRIPTOR_HANDLE)));
}

void STDMETHODCALLTYPE CommandRecorder::ClearDepthStencilView(D3D12_CPU_DESCRIPTOR_HANDLE dsv,
                                                              D3D12_CLEAR_FLAGS flags,
                                                              FLOAT depth, UINT8 stencil, UINT n,
                                                              const D3D12_RECT* rects) {
  const UINT count = rects ? n : 0;
  const ClearDsvArgs a = {dsv, flags, depth, stencil, count};
  Put(kClearDsv, &a, sizeof(a), rects, uint32_t(count * sizeof(D3D12_RECT)));
}

void STDMETHODCALLTYPE CommandRecorder::ClearRenderTargetView(D3D12_CPU_DESCRIPTOR_HANDLE rtv,
                                                              const FLOAT color[4], UINT n,
                                                              const D3D12_RECT* rects) {
  const UINT count = rects ? n : 0;
  ClearRtvArgs a = {rtv, {}, count, 0};
  std::memcpy(a.color, color, sizeof(a.color));
  Put(kClearRtv, &a, sizeof(a), rects, uint32_t(count * sizeof(D3D12_RECT)));
}

void STDMETHODCALLTYPE CommandRecorder::BeginQuery(ID3D12QueryHeap* heap, D3D12_QUERY_TYPE type,
                                                   UINT index) {
  const QueryArgs a = {heap, type, index};
  Put(kBeginQuery, &a, sizeof(a));
}

void STDMETHODCALLTYPE CommandRecorder::EndQuery(ID3D12QueryHeap* heap, D3D12_QUERY_TYPE type,
                                                 UINT index) {
  const QueryArgs a = {heap, type, index};
  Put(kEndQuery, &a, sizeof(a));
}

void STDMETHODCALLTYPE CommandRecorder::ResolveQueryData(ID3D12QueryHeap* heap,
                                                         D3D12_QUERY_TYPE type, UINT start, UINT n,
                                                         ID3D12Resource* dst, UINT64 offset) {
  const ResolveQueryArgs a = {heap, type, start, n, 0, dst, offset};
  Put(kResolveQuery, &a, sizeof(a));
}

void CommandRecorder::SubmitList(ID3D12CommandQueue* queue, ID3D12CommandAllocator* allocator) {
  const SubmitArgs a = {queue, allocator};
  Put(kSubmitList, &a, sizeof(a));
  Publish();  // the GPU starts on it as soon as the replay reaches it
}

void CommandRecorder::RSSetDepthBias(FLOAT bias, FLOAT clamp, FLOAT slope) {
  if (!list9_) return;
  const FLOAT a[4] = {bias, clamp, slope, 0.0f};
  Put(kDepthBias, a, sizeof(a));
}

// ---------------------------------------------------------------------------------------------
// Replay.

void CommandRecorder::Worker() {
  SetThreadDescription(GetCurrentThread(), L"pgr4 list replay");
  uint64_t tail = 0;
  for (;;) {
    uint64_t head = head_.load(std::memory_order_acquire);
    if (head == tail) {
      const uint64_t start = Ticks();
      const uint64_t spin = worker_spin_ns_.load(std::memory_order_relaxed);
      while (head == tail && TicksToNs(Ticks() - start) < spin) {
        for (int i = 0; i < 16; ++i) SpinPause();
        head = head_.load(std::memory_order_acquire);
      }
      if (head == tail) {
        worker_sleeping_.store(1, std::memory_order_seq_cst);
        head = head_.load(std::memory_order_seq_cst);
        if (head == tail) WaitOnAddress(&head_, &tail, sizeof(tail), INFINITE);
        worker_sleeping_.store(0, std::memory_order_relaxed);
        continue;
      }
    }
    const uint64_t t0 = Ticks();
    bool stop = false;
    for (uint64_t at = tail; at < head;) {
      const uint8_t* p = ring_.get() + (at & (kRingBytes - 1));
      const uint32_t op = Read<uint32_t>(p), size = Read<uint32_t>(p + 4);
      at += size;
      if (op == kStop) {
        stop = true;
        break;
      }
      Replay(op, p + 8);
    }
    replay_ns_.fetch_add(TicksToNs(Ticks() - t0), std::memory_order_relaxed);
    tail = head;
    tail_.store(tail, std::memory_order_seq_cst);
    if (producer_waiting_.load(std::memory_order_seq_cst)) WakeByAddressAll(&tail_);
    if (stop) return;
  }
}

void CommandRecorder::Replay(uint32_t op, const uint8_t* a) {
  ID3D12GraphicsCommandList* const l = real_;
  switch (op) {
    case kWrap:
      break;
    case kDrawInstanced: {
      const UINT* d = reinterpret_cast<const UINT*>(a);
      l->DrawInstanced(d[0], d[1], d[2], d[3]);
      break;
    }
    case kDrawIndexed: {
      const auto d = Read<DrawIndexedArgs>(a);
      l->DrawIndexedInstanced(d.indices, d.instances, d.first_index, d.base_vertex,
                              d.first_instance);
      break;
    }
    case kDispatch: {
      const UINT* d = reinterpret_cast<const UINT*>(a);
      l->Dispatch(d[0], d[1], d[2]);
      break;
    }
    case kCopyBuffer: {
      const auto c = Read<CopyBufferArgs>(a);
      l->CopyBufferRegion(c.dst, c.dst_offset, c.src, c.src_offset, c.bytes);
      break;
    }
    case kCopyTexture: {
      const auto c = Read<CopyTextureArgs>(a);
      l->CopyTextureRegion(&c.dst, c.x, c.y, c.z, &c.src, c.has_box ? &c.box : nullptr);
      break;
    }
    case kCopyResource: {
      const auto c = Read<CopyResourceArgs>(a);
      l->CopyResource(c.dst, c.src);
      break;
    }
    case kTopology:
      l->IASetPrimitiveTopology(Read<D3D12_PRIMITIVE_TOPOLOGY>(a));
      break;
    case kViewports:
      l->RSSetViewports(Read<CountArgs>(a).n,
                        reinterpret_cast<const D3D12_VIEWPORT*>(a + sizeof(CountArgs)));
      break;
    case kScissors:
      l->RSSetScissorRects(Read<CountArgs>(a).n,
                           reinterpret_cast<const D3D12_RECT*>(a + sizeof(CountArgs)));
      break;
    case kBlendFactor: {
      const auto b = Read<BlendArgs>(a);
      l->OMSetBlendFactor(b.has ? b.factor : nullptr);
      break;
    }
    case kStencilRef:
      l->OMSetStencilRef(Read<UINT>(a));
      break;
    case kPipeline:
      l->SetPipelineState(Read<ID3D12PipelineState*>(a));
      break;
    case kBarriers:
      l->ResourceBarrier(Read<CountArgs>(a).n,
                         reinterpret_cast<const D3D12_RESOURCE_BARRIER*>(a + sizeof(CountArgs)));
      break;
    case kHeaps:
      l->SetDescriptorHeaps(Read<CountArgs>(a).n,
                            reinterpret_cast<ID3D12DescriptorHeap* const*>(a + sizeof(CountArgs)));
      break;
    case kComputeRoot:
      l->SetComputeRootSignature(Read<ID3D12RootSignature*>(a));
      break;
    case kGraphicsRoot:
      l->SetGraphicsRootSignature(Read<ID3D12RootSignature*>(a));
      break;
    case kComputeTable: {
      const auto t = Read<TableArgs>(a);
      l->SetComputeRootDescriptorTable(t.index, t.table);
      break;
    }
    case kGraphicsTable: {
      const auto t = Read<TableArgs>(a);
      l->SetGraphicsRootDescriptorTable(t.index, t.table);
      break;
    }
    case kComputeConstants: {
      const auto c = Read<ConstantsArgs>(a);
      l->SetComputeRoot32BitConstants(c.index, c.n, a + sizeof(ConstantsArgs), c.offset);
      break;
    }
    case kGraphicsConstants: {
      const auto c = Read<ConstantsArgs>(a);
      l->SetGraphicsRoot32BitConstants(c.index, c.n, a + sizeof(ConstantsArgs), c.offset);
      break;
    }
    case kGraphicsCbv: {
      const auto c = Read<CbvArgs>(a);
      l->SetGraphicsRootConstantBufferView(c.index, c.at);
      break;
    }
    case kIndexBuffer: {
      const auto v = Read<IndexBufferArgs>(a);
      l->IASetIndexBuffer(v.has ? &v.view : nullptr);
      break;
    }
    case kTargets: {
      const auto t = Read<TargetsArgs>(a);
      l->OMSetRenderTargets(
          t.n,
          t.rtv_count ? reinterpret_cast<const D3D12_CPU_DESCRIPTOR_HANDLE*>(a + sizeof(t))
                      : nullptr,
          t.single_range, t.has_dsv ? &t.dsv : nullptr);
      break;
    }
    case kClearDsv: {
      const auto c = Read<ClearDsvArgs>(a);
      l->ClearDepthStencilView(
          c.dsv, c.flags, c.depth, UINT8(c.stencil), c.n,
          c.n ? reinterpret_cast<const D3D12_RECT*>(a + sizeof(ClearDsvArgs)) : nullptr);
      break;
    }
    case kClearRtv: {
      const auto c = Read<ClearRtvArgs>(a);
      l->ClearRenderTargetView(
          c.rtv, c.color, c.n,
          c.n ? reinterpret_cast<const D3D12_RECT*>(a + sizeof(ClearRtvArgs)) : nullptr);
      break;
    }
    case kBeginQuery: {
      const auto q = Read<QueryArgs>(a);
      l->BeginQuery(q.heap, q.type, q.index);
      break;
    }
    case kEndQuery: {
      const auto q = Read<QueryArgs>(a);
      l->EndQuery(q.heap, q.type, q.index);
      break;
    }
    case kResolveQuery: {
      const auto q = Read<ResolveQueryArgs>(a);
      l->ResolveQueryData(q.heap, q.type, q.start, q.n, q.dst, q.offset);
      break;
    }
    case kDepthBias: {
      const FLOAT* f = reinterpret_cast<const FLOAT*>(a);
      list9_->RSSetDepthBias(f[0], f[1], f[2]);
      break;
    }
    case kSubmitList: {
      // D3D12Device::Submit's, on this thread: the list closed, submitted alone, reset on the
      // frame's allocator (a list may be reset while its submission runs).
      const auto s = Read<SubmitArgs>(a);
      if (FAILED(l->Close())) {
        failed_.store(true, std::memory_order_release);
        break;
      }
      ID3D12CommandList* const lists[] = {l};
      s.queue->ExecuteCommandLists(1, lists);
      if (FAILED(l->Reset(s.allocator, nullptr))) failed_.store(true, std::memory_order_release);
      break;
    }
    default:
      break;
  }
}

}  // namespace pgr4::native
