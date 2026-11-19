// pgr4-native PGR4 native renderer: the frame list's D3D12 calls recorded on the render thread
// and replayed into the real list on a thread of their own (local addition, 18/11/2026).
//
// The render thread spent a good part of each draw inside the driver (SetPipelineState, the root
// constant buffers, the render targets, the draw call itself). Here those calls are written into
// a ring of commands, a few bytes each, and a replay thread makes them on the real list while the
// render thread translates the next draws. The same method as PGR4-ReGPU's RecordedList (MIT,
// github.com/zer0ducksgiven/PGR4-ReGPU, thirdparty/ReGPU/src/gpu/recorded_list.h), written again
// for this renderer: a ring the render thread publishes into every few draws instead of chunks
// under a mutex.
//
// The recorder is an ID3D12GraphicsCommandList, so the draw path and everything it calls (render
// targets, textures, occlusion, scene copies, the upscaler's motion target) use it unchanged:
// - the calls the renderer makes per draw are recorded (the list below), in order;
// - every other method first waits until everything recorded is replayed (Drain), then calls the
//   real list (logged once per method, so a new hot call shows up in the log);
// - D3D12Device::command_list() drains and returns the real list: the code outside the draw path
//   (resolves, the upscalers, frame generation, the overlay, Streamline, the submissions) always
//   gets the real list with everything before it in it.
// RTV and DSV descriptors are read when OMSetRenderTargets and the clears are made on the real
// list: their writes drain first (DrainCommandRecording, render_targets.cpp, upscaler.cpp).
// Resources and pipelines named in the commands live at least until the frame's fence (every
// release here waits for it), so past the replay, which ends before the frame is submitted.
//
// --pgr4_native_record_list (d3d12_device.cpp) turns it off: the draw path then calls the real
// list, as before; read at each frame's start.
//
// Threading: Put, Publish and Drain on the thread that records (the render thread, or the guest
// thread without it); the replay on the recorder's own thread. Drain from another thread only
// waits for what was published.
#pragma once

#include <d3d12.h>
#include <wrl/client.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <thread>

namespace pgr4::native {

class CommandRecorder final : public ID3D12GraphicsCommandList {
 public:
  // Per frame (TakeStats): the recording thread's waits for the replay, how many drains waited,
  // the replay thread's time on the commands, the bytes recorded, the calls forwarded.
  struct Stats {
    uint64_t wait_ns = 0, drains = 0, replay_ns = 0, bytes = 0, forwarded = 0;
  };

  CommandRecorder() = default;
  ~CommandRecorder();
  CommandRecorder(const CommandRecorder&) = delete;
  CommandRecorder& operator=(const CommandRecorder&) = delete;

  // Starts the replay thread on `real` (the frame list, which lives as long as the recorder).
  // False (logged) on failure.
  bool Start(ID3D12GraphicsCommandList* real);
  bool started() const { return started_; }
  // Everything recorded so far is in the real list.
  void Drain();
  ID3D12GraphicsCommandList* real() const { return real_; }
  // `list` as the recorder, or nullptr when it is another list.
  static CommandRecorder* From(ID3D12GraphicsCommandList* list);
  // ID3D12GraphicsCommandList9::RSSetDepthBias, recorded; false when the real list has none.
  bool has_depth_bias() const { return list9_ != nullptr; }
  void RSSetDepthBias(FLOAT bias, FLOAT clamp, FLOAT slope);
  Stats TakeStats();
  // Local change (18/11/2026, D3D12Device::Submit with pgr4_native_record_submit): recorded, the
  // real list's Close, ExecuteCommandLists of it alone on `queue` and Reset on `allocator`, made by
  // the replay thread in order (the render thread no longer waits for the driver's submission).
  // failed(): one of them failed (the frame fails at its end, D3D12Device::EndFrame).
  void SubmitList(ID3D12CommandQueue* queue, ID3D12CommandAllocator* allocator);
  bool failed() const { return failed_.load(std::memory_order_acquire); }
  // Local change (18/11/2026), development: the bytes recorded between two publishes (kPublishBytes
  // by default) and the replay thread's spin before it sleeps (kWorkerSpinNs), live.
  void SetTuning(uint32_t publish_bytes, uint32_t worker_spin_ns) {
    publish_bytes_ = publish_bytes ? publish_bytes : uint32_t(kPublishBytes);
    worker_spin_ns_.store(worker_spin_ns ? worker_spin_ns : 10000, std::memory_order_relaxed);
  }

  // IUnknown, ID3D12Object, ID3D12DeviceChild, ID3D12CommandList.
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) override;
  ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
  ULONG STDMETHODCALLTYPE Release() override { return 1; }
  HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID guid, UINT* size, void* data) override {
    return real_->GetPrivateData(guid, size, data);
  }
  HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID guid, UINT size, const void* data) override {
    return real_->SetPrivateData(guid, size, data);
  }
  HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID guid, const IUnknown* data) override {
    return real_->SetPrivateDataInterface(guid, data);
  }
  HRESULT STDMETHODCALLTYPE SetName(LPCWSTR name) override { return real_->SetName(name); }
  HRESULT STDMETHODCALLTYPE GetDevice(REFIID riid, void** device) override {
    return real_->GetDevice(riid, device);
  }
  D3D12_COMMAND_LIST_TYPE STDMETHODCALLTYPE GetType() override { return real_->GetType(); }

  // Recorded.
  void STDMETHODCALLTYPE DrawInstanced(UINT vertices, UINT instances, UINT first_vertex,
                                       UINT first_instance) override;
  void STDMETHODCALLTYPE DrawIndexedInstanced(UINT indices, UINT instances, UINT first_index,
                                              INT base_vertex, UINT first_instance) override;
  void STDMETHODCALLTYPE Dispatch(UINT x, UINT y, UINT z) override;
  void STDMETHODCALLTYPE CopyBufferRegion(ID3D12Resource* dst, UINT64 dst_offset,
                                          ID3D12Resource* src, UINT64 src_offset,
                                          UINT64 bytes) override;
  void STDMETHODCALLTYPE CopyTextureRegion(const D3D12_TEXTURE_COPY_LOCATION* dst, UINT x, UINT y,
                                           UINT z, const D3D12_TEXTURE_COPY_LOCATION* src,
                                           const D3D12_BOX* box) override;
  void STDMETHODCALLTYPE CopyResource(ID3D12Resource* dst, ID3D12Resource* src) override;
  void STDMETHODCALLTYPE IASetPrimitiveTopology(D3D12_PRIMITIVE_TOPOLOGY topology) override;
  void STDMETHODCALLTYPE RSSetViewports(UINT n, const D3D12_VIEWPORT* viewports) override;
  void STDMETHODCALLTYPE RSSetScissorRects(UINT n, const D3D12_RECT* rects) override;
  void STDMETHODCALLTYPE OMSetBlendFactor(const FLOAT factor[4]) override;
  void STDMETHODCALLTYPE OMSetStencilRef(UINT ref) override;
  void STDMETHODCALLTYPE SetPipelineState(ID3D12PipelineState* pso) override;
  void STDMETHODCALLTYPE ResourceBarrier(UINT n, const D3D12_RESOURCE_BARRIER* barriers) override;
  void STDMETHODCALLTYPE SetDescriptorHeaps(UINT n, ID3D12DescriptorHeap* const* heaps) override;
  void STDMETHODCALLTYPE SetComputeRootSignature(ID3D12RootSignature* root) override;
  void STDMETHODCALLTYPE SetGraphicsRootSignature(ID3D12RootSignature* root) override;
  void STDMETHODCALLTYPE SetComputeRootDescriptorTable(UINT index,
                                                       D3D12_GPU_DESCRIPTOR_HANDLE table) override;
  void STDMETHODCALLTYPE SetGraphicsRootDescriptorTable(UINT index,
                                                        D3D12_GPU_DESCRIPTOR_HANDLE table) override;
  void STDMETHODCALLTYPE SetComputeRoot32BitConstants(UINT index, UINT n, const void* data,
                                                      UINT offset) override;
  void STDMETHODCALLTYPE SetGraphicsRoot32BitConstants(UINT index, UINT n, const void* data,
                                                       UINT offset) override;
  void STDMETHODCALLTYPE SetGraphicsRootConstantBufferView(UINT index,
                                                           D3D12_GPU_VIRTUAL_ADDRESS at) override;
  void STDMETHODCALLTYPE IASetIndexBuffer(const D3D12_INDEX_BUFFER_VIEW* view) override;
  void STDMETHODCALLTYPE OMSetRenderTargets(UINT n, const D3D12_CPU_DESCRIPTOR_HANDLE* rtvs,
                                            BOOL single_range,
                                            const D3D12_CPU_DESCRIPTOR_HANDLE* dsv) override;
  void STDMETHODCALLTYPE ClearDepthStencilView(D3D12_CPU_DESCRIPTOR_HANDLE dsv,
                                               D3D12_CLEAR_FLAGS flags, FLOAT depth,
                                               UINT8 stencil, UINT n,
                                               const D3D12_RECT* rects) override;
  void STDMETHODCALLTYPE ClearRenderTargetView(D3D12_CPU_DESCRIPTOR_HANDLE rtv,
                                               const FLOAT color[4], UINT n,
                                               const D3D12_RECT* rects) override;
  void STDMETHODCALLTYPE BeginQuery(ID3D12QueryHeap* heap, D3D12_QUERY_TYPE type,
                                    UINT index) override;
  void STDMETHODCALLTYPE EndQuery(ID3D12QueryHeap* heap, D3D12_QUERY_TYPE type,
                                  UINT index) override;
  void STDMETHODCALLTYPE ResolveQueryData(ID3D12QueryHeap* heap, D3D12_QUERY_TYPE type,
                                          UINT start, UINT n, ID3D12Resource* dst,
                                          UINT64 offset) override;

  // Forwarded: drained, then the real list's (the code generated from d3d12.h of the Windows SDK
  // 10.0.26100, every other method of ID3D12GraphicsCommandList).
  HRESULT STDMETHODCALLTYPE Close() override {
    Forwarded(__func__);
    return real_->Close();
  }
  HRESULT STDMETHODCALLTYPE Reset(ID3D12CommandAllocator *pAllocator, ID3D12PipelineState *pInitialState) override {
    Forwarded(__func__);
    return real_->Reset(pAllocator, pInitialState);
  }
  void STDMETHODCALLTYPE ClearState(ID3D12PipelineState *pPipelineState) override {
    Forwarded(__func__);
    real_->ClearState(pPipelineState);
  }
  void STDMETHODCALLTYPE CopyTiles(ID3D12Resource *pTiledResource, const D3D12_TILED_RESOURCE_COORDINATE *pTileRegionStartCoordinate, const D3D12_TILE_REGION_SIZE *pTileRegionSize, ID3D12Resource *pBuffer, UINT64 BufferStartOffsetInBytes, D3D12_TILE_COPY_FLAGS Flags) override {
    Forwarded(__func__);
    real_->CopyTiles(pTiledResource, pTileRegionStartCoordinate, pTileRegionSize, pBuffer, BufferStartOffsetInBytes, Flags);
  }
  void STDMETHODCALLTYPE ResolveSubresource(ID3D12Resource *pDstResource, UINT DstSubresource, ID3D12Resource *pSrcResource, UINT SrcSubresource, DXGI_FORMAT Format) override {
    Forwarded(__func__);
    real_->ResolveSubresource(pDstResource, DstSubresource, pSrcResource, SrcSubresource, Format);
  }
  void STDMETHODCALLTYPE ExecuteBundle(ID3D12GraphicsCommandList *pCommandList) override {
    Forwarded(__func__);
    real_->ExecuteBundle(pCommandList);
  }
  void STDMETHODCALLTYPE SetComputeRoot32BitConstant(UINT RootParameterIndex, UINT SrcData, UINT DestOffsetIn32BitValues) override {
    Forwarded(__func__);
    real_->SetComputeRoot32BitConstant(RootParameterIndex, SrcData, DestOffsetIn32BitValues);
  }
  void STDMETHODCALLTYPE SetGraphicsRoot32BitConstant(UINT RootParameterIndex, UINT SrcData, UINT DestOffsetIn32BitValues) override {
    Forwarded(__func__);
    real_->SetGraphicsRoot32BitConstant(RootParameterIndex, SrcData, DestOffsetIn32BitValues);
  }
  void STDMETHODCALLTYPE SetComputeRootConstantBufferView(UINT RootParameterIndex, D3D12_GPU_VIRTUAL_ADDRESS BufferLocation) override {
    Forwarded(__func__);
    real_->SetComputeRootConstantBufferView(RootParameterIndex, BufferLocation);
  }
  void STDMETHODCALLTYPE SetComputeRootShaderResourceView(UINT RootParameterIndex, D3D12_GPU_VIRTUAL_ADDRESS BufferLocation) override {
    Forwarded(__func__);
    real_->SetComputeRootShaderResourceView(RootParameterIndex, BufferLocation);
  }
  void STDMETHODCALLTYPE SetGraphicsRootShaderResourceView(UINT RootParameterIndex, D3D12_GPU_VIRTUAL_ADDRESS BufferLocation) override {
    Forwarded(__func__);
    real_->SetGraphicsRootShaderResourceView(RootParameterIndex, BufferLocation);
  }
  void STDMETHODCALLTYPE SetComputeRootUnorderedAccessView(UINT RootParameterIndex, D3D12_GPU_VIRTUAL_ADDRESS BufferLocation) override {
    Forwarded(__func__);
    real_->SetComputeRootUnorderedAccessView(RootParameterIndex, BufferLocation);
  }
  void STDMETHODCALLTYPE SetGraphicsRootUnorderedAccessView(UINT RootParameterIndex, D3D12_GPU_VIRTUAL_ADDRESS BufferLocation) override {
    Forwarded(__func__);
    real_->SetGraphicsRootUnorderedAccessView(RootParameterIndex, BufferLocation);
  }
  void STDMETHODCALLTYPE IASetVertexBuffers(UINT StartSlot, UINT NumViews, const D3D12_VERTEX_BUFFER_VIEW *pViews) override {
    Forwarded(__func__);
    real_->IASetVertexBuffers(StartSlot, NumViews, pViews);
  }
  void STDMETHODCALLTYPE SOSetTargets(UINT StartSlot, UINT NumViews, const D3D12_STREAM_OUTPUT_BUFFER_VIEW *pViews) override {
    Forwarded(__func__);
    real_->SOSetTargets(StartSlot, NumViews, pViews);
  }
  void STDMETHODCALLTYPE ClearUnorderedAccessViewUint(D3D12_GPU_DESCRIPTOR_HANDLE ViewGPUHandleInCurrentHeap, D3D12_CPU_DESCRIPTOR_HANDLE ViewCPUHandle, ID3D12Resource *pResource, const UINT Values[ 4 ], UINT NumRects, const D3D12_RECT *pRects) override {
    Forwarded(__func__);
    real_->ClearUnorderedAccessViewUint(ViewGPUHandleInCurrentHeap, ViewCPUHandle, pResource, Values, NumRects, pRects);
  }
  void STDMETHODCALLTYPE ClearUnorderedAccessViewFloat(D3D12_GPU_DESCRIPTOR_HANDLE ViewGPUHandleInCurrentHeap, D3D12_CPU_DESCRIPTOR_HANDLE ViewCPUHandle, ID3D12Resource *pResource, const FLOAT Values[ 4 ], UINT NumRects, const D3D12_RECT *pRects) override {
    Forwarded(__func__);
    real_->ClearUnorderedAccessViewFloat(ViewGPUHandleInCurrentHeap, ViewCPUHandle, pResource, Values, NumRects, pRects);
  }
  void STDMETHODCALLTYPE DiscardResource(ID3D12Resource *pResource, const D3D12_DISCARD_REGION *pRegion) override {
    Forwarded(__func__);
    real_->DiscardResource(pResource, pRegion);
  }
  void STDMETHODCALLTYPE SetPredication(ID3D12Resource *pBuffer, UINT64 AlignedBufferOffset, D3D12_PREDICATION_OP Operation) override {
    Forwarded(__func__);
    real_->SetPredication(pBuffer, AlignedBufferOffset, Operation);
  }
  void STDMETHODCALLTYPE SetMarker(UINT Metadata, const void *pData, UINT Size) override {
    Forwarded(__func__);
    real_->SetMarker(Metadata, pData, Size);
  }
  void STDMETHODCALLTYPE BeginEvent(UINT Metadata, const void *pData, UINT Size) override {
    Forwarded(__func__);
    real_->BeginEvent(Metadata, pData, Size);
  }
  void STDMETHODCALLTYPE EndEvent() override {
    Forwarded(__func__);
    real_->EndEvent();
  }
  void STDMETHODCALLTYPE ExecuteIndirect(ID3D12CommandSignature *pCommandSignature, UINT MaxCommandCount, ID3D12Resource *pArgumentBuffer, UINT64 ArgumentBufferOffset, ID3D12Resource *pCountBuffer, UINT64 CountBufferOffset) override {
    Forwarded(__func__);
    real_->ExecuteIndirect(pCommandSignature, MaxCommandCount, pArgumentBuffer, ArgumentBufferOffset, pCountBuffer, CountBufferOffset);
  }

 private:
  enum Op : uint32_t {
    kWrap,  // the rest of the ring is unused: go on at its start
    kStop,
    kDrawInstanced,
    kDrawIndexed,
    kDispatch,
    kCopyBuffer,
    kCopyTexture,
    kCopyResource,
    kTopology,
    kViewports,
    kScissors,
    kBlendFactor,
    kStencilRef,
    kPipeline,
    kBarriers,
    kHeaps,
    kComputeRoot,
    kGraphicsRoot,
    kComputeTable,
    kGraphicsTable,
    kComputeConstants,
    kGraphicsConstants,
    kGraphicsCbv,
    kIndexBuffer,
    kTargets,
    kClearDsv,
    kClearRtv,
    kBeginQuery,
    kEndQuery,
    kResolveQuery,
    kDepthBias,
    kSubmitList,
  };
  // A command: its header, then `a` and `b` (8-byte aligned). Publishes every kPublishBytes,
  // after a draw, dispatch or copy, so the replay follows a few draws behind.
  void Put(Op op, const void* a, uint32_t na, const void* b = nullptr, uint32_t nb = 0);
  // Makes what was recorded visible to the replay thread.
  void Publish();
  // Waits until the replay reached `position`.
  void WaitReplayed(uint64_t position);
  void Forwarded(const char* method);
  void Worker();
  // Makes one command (`a` its arguments) on the real list.
  void Replay(uint32_t op, const uint8_t* a);

  static constexpr uint64_t kRingBytes = 8ull << 20;
  static constexpr uint64_t kPublishBytes = 4096;

  ID3D12GraphicsCommandList* real_ = nullptr;
  Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList9> list9_;
  std::unique_ptr<uint8_t[]> ring_;
  bool started_ = false;
  std::thread worker_;
  // The thread that records (its last Put).
  std::atomic<uint32_t> producer_tid_{0};
  // The recording thread's: bytes written, published, replayed by the last drain, and replayed
  // as last read.
  uint64_t write_ = 0, published_ = 0, drained_ = 0, known_tail_ = 0;
  const char* forwarded_seen_[64] = {};  // the methods forwarded so far (logged once each)
  // Published (written by the recording thread) and replayed (by the replay thread) positions.
  alignas(64) std::atomic<uint64_t> head_{0};
  alignas(64) std::atomic<uint64_t> tail_{0};
  alignas(64) std::atomic<uint32_t> worker_sleeping_{0};
  std::atomic<uint32_t> producer_waiting_{0};
  std::atomic<bool> failed_{false};
  uint32_t publish_bytes_ = uint32_t(kPublishBytes);  // the recording thread's
  std::atomic<uint32_t> worker_spin_ns_{10000};
  // Stats: the recording thread's, then the replay thread's.
  alignas(64) Stats stats_;
  std::atomic<uint64_t> replay_ns_{0};
};

// The process's recorder drained (render_targets.cpp, upscaler.cpp: before an RTV or DSV
// descriptor is written); nothing without one.
void DrainCommandRecording();

}  // namespace pgr4::native
