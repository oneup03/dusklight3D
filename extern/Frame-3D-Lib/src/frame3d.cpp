#include "frame3d/frame3d.h"

#include "common.hpp"
#include "gbm_allocator.hpp"
#include "openvr_runtime.hpp"
#include "screen_follower.hpp"

#include <poll.h>
#include <unistd.h>

#include <algorithm>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace {

using frame3d::Logger;

// Shown and Retiring images are what SteamVR may still be sampling (Gamescope keeps
// its queued and visible framebuffers alive the same way), so neither is handed out.
enum class BufferState { Free, Acquired, Pending, Presenting, Shown, Retiring };

struct Buffer {
  frame3d::GbmImage image;
  vr::SharedTextureHandle_t handle = 0;
  BufferState state = BufferState::Free;
};

void close_fd(int& fd) {
  if (fd >= 0)
    close(fd);
  fd = -1;
}

}  // namespace

struct frame3d_context {
  Logger log;
  frame3d_config config{};
  frame3d::OpenVRRuntime vr;
  frame3d::GbmAllocator gbm;
  bool gbmReady = false;
  std::vector<Buffer> buffers;
  vr::VROverlayHandle_t overlay = vr::k_ulOverlayHandleInvalid;
  frame3d::ScreenFollower follower;

  mutable std::mutex mutex;
  std::condition_variable wake;
  int pending = -1;
  int pendingFd = -1;
  int shown = -1;
  int retiring = -1;
  bool wantVisible = true;
  bool visibilityChanged = false;
  bool recenterRequested = false;
  bool stop = false;
  frame3d_status status{};
  std::thread worker;

  // Worker thread only.
  bool hasFrame = false;
  bool overlayShown = false;
  bool warnedTexture = false;

  void run();
  void present(int index, int fd);
  void teardown();
};

void frame3d_context::present(int index, int fd) {
  if (fd >= 0) {
    pollfd pfd{fd, POLLIN, 0};
    if (poll(&pfd, 1, 1000) == 0)
      log(FRAME3D_LOG_WARN, "frame fence not signalled after 1 s; showing it anyway");
    close_fd(fd);
  }
  vr::Texture_t tex{&buffers[index].handle, vr::TextureType_SharedTextureHandle, vr::ColorSpace_Gamma};
  const vr::EVROverlayError err = vr.overlay()->SetOverlayTexture(overlay, &tex);
  if (err != vr::VROverlayError_None && !warnedTexture) {
    log(FRAME3D_LOG_WARN, "SetOverlayTexture: %s", vr.overlay()->GetOverlayErrorNameFromEnum(err));
    warnedTexture = true;
  }
  std::lock_guard lock(mutex);
  if (retiring >= 0)
    buffers[retiring].state = BufferState::Free;
  retiring = shown;
  if (retiring >= 0)
    buffers[retiring].state = BufferState::Retiring;
  shown = index;
  buffers[index].state = BufferState::Shown;
  hasFrame = true;
}

void frame3d_context::run() {
  using Clock = frame3d::ScreenFollower::Clock;
  Clock::duration sleep = std::chrono::milliseconds(16);
  vr::IVROverlay* ov = vr.overlay();
  for (;;) {
    int index = -1, fd = -1;
    bool recenter = false, visible = true;
    {
      std::unique_lock lock(mutex);
      wake.wait_for(lock, sleep, [&] { return stop || pending >= 0 || recenterRequested || visibilityChanged; });
      if (stop)
        break;
      visibilityChanged = false;
      if (pending >= 0) {
        index = std::exchange(pending, -1);
        fd = std::exchange(pendingFd, -1);
        buffers[index].state = BufferState::Presenting;
      }
      recenter = std::exchange(recenterRequested, false);
      visible = wantVisible;
    }
    if (index >= 0)
      present(index, fd);
    if (recenter)
      follower.recenter();

    bool quit = false;
    vr::VREvent_t ev;
    if (vr.system()) {
      while (vr.system()->PollNextEvent(&ev, sizeof ev))
        quit = quit || ev.eventType == vr::VREvent_Quit;
    }
    while (ov->PollNextOverlayEvent(overlay, &ev, sizeof ev)) {
    }

    sleep = follower.tick(Clock::now());

    // Shown once there's a frame to show, and hidden while the game screen is.
    const bool show = visible && hasFrame && follower.screen_visible();
    if (show != overlayShown) {
      if (show)
        ov->ShowOverlay(overlay);
      else
        ov->HideOverlay(overlay);
      overlayShown = show;
    }

    std::lock_guard lock(mutex);
    status.placement = follower.following() ? FRAME3D_PLACEMENT_FOLLOWING : FRAME3D_PLACEMENT_FIXED;
    status.visible = overlayShown;
    status.width_m = follower.width();
    status.height_m = follower.height();
    status.curvature = follower.curvature();
    status.quit_requested = status.quit_requested || quit;
  }
}

void frame3d_context::teardown() {
  {
    std::lock_guard lock(mutex);
    stop = true;
  }
  wake.notify_all();
  if (worker.joinable())
    worker.join();
  close_fd(pendingFd);
  if (vr.overlay() && overlay != vr::k_ulOverlayHandleInvalid) {
    vr.overlay()->ClearOverlayTexture(overlay);
    vr.overlay()->DestroyOverlay(overlay);
    overlay = vr::k_ulOverlayHandleInvalid;
  }
  for (Buffer& b : buffers) {
    if (b.handle && vr.ipc())
      vr.ipc()->UnrefResource(b.handle);
    if (gbmReady)
      gbm.release(b.image);
  }
  buffers.clear();
  if (gbmReady)
    gbm.shutdown();
  gbmReady = false;
  vr.shutdown();
}

extern "C" {

void frame3d_config_init(frame3d_config* config) {
  *config = {};
  config->eye_width = 1920;
  config->eye_height = 1080;
  config->buffer_count = 4;
  config->fallback_distance_m = 2.0f;
  config->fallback_width_m = 1.6f;
  config->log_level = FRAME3D_LOG_INFO;
}

bool frame3d_runtime_available(void) { return frame3d::OpenVRRuntime::available(); }

frame3d_context* frame3d_create(const frame3d_config* config) {
  auto ctx = std::make_unique<frame3d_context>();
  frame3d_config& cfg = ctx->config;
  cfg = *config;
  ctx->log.configure(cfg.log, cfg.log_user, cfg.log_level);
  const Logger& log = ctx->log;
  if (cfg.eye_width == 0 || cfg.eye_height == 0) {
    log(FRAME3D_LOG_ERROR, "eye size must be non-zero");
    return nullptr;
  }
  if (cfg.buffer_count == 0)
    cfg.buffer_count = 4;
  cfg.buffer_count = std::max(cfg.buffer_count, 3u);
  if (cfg.fallback_distance_m <= 0.f)
    cfg.fallback_distance_m = 2.0f;
  if (cfg.fallback_width_m <= 0.f)
    cfg.fallback_width_m = 1.6f;
  const uint32_t width = cfg.eye_width * 2, height = cfg.eye_height;

  // Everything after this point unwinds through teardown().
  const auto fail = [&]() -> frame3d_context* {
    ctx->teardown();
    return nullptr;
  };
  if (!ctx->vr.init(log))
    return fail();
  vr::IVRIPCResourceManagerClient* ipc = ctx->vr.ipc();
  vr::IVROverlay* ov = ctx->vr.overlay();

  // Formats and layouts SteamVR accepts for overlay images.
  uint32_t count = 0;
  std::vector<uint32_t> formats;
  if (ipc->GetDmabufFormats(&count, nullptr) && count > 0) {
    formats.resize(count);
    ipc->GetDmabufFormats(&count, formats.data());
    formats.resize(count);
  }
  const auto accepted = [&](uint32_t f) { return std::find(formats.begin(), formats.end(), f) != formats.end(); };
  uint32_t format = 0;
  if (cfg.drm_format != 0) {
    if (!accepted(cfg.drm_format)) {
      log(FRAME3D_LOG_ERROR, "SteamVR doesn't accept dma-buf format %s", frame3d::fourcc_str(cfg.drm_format).c_str());
      return fail();
    }
    format = cfg.drm_format;
  } else {
    for (const uint32_t candidate : {FRAME3D_FORMAT_RGBA8, FRAME3D_FORMAT_BGRA8}) {
      if (accepted(candidate)) {
        format = candidate;
        break;
      }
    }
  }
  if (format == 0) {
    log(FRAME3D_LOG_ERROR, "SteamVR accepts no 8-bit RGBA/BGRA dma-buf format (%zu formats listed)", formats.size());
    return fail();
  }
  std::vector<uint64_t> modifiers;
  count = 0;
  if (ipc->GetDmabufModifiers(vr::VRApplication_Overlay, format, &count, nullptr) && count > 0) {
    modifiers.resize(count);
    ipc->GetDmabufModifiers(vr::VRApplication_Overlay, format, &count, modifiers.data());
    modifiers.resize(count);
  }
  modifiers.erase(std::remove(modifiers.begin(), modifiers.end(), frame3d::kModifierInvalid), modifiers.end());
  if (modifiers.empty()) {
    log(FRAME3D_LOG_ERROR, "SteamVR lists no explicit dma-buf layouts for %s", frame3d::fourcc_str(format).c_str());
    return fail();
  }
  const bool linear = cfg.cpu_writable || cfg.prefer_linear;
  if (linear && std::find(modifiers.begin(), modifiers.end(), FRAME3D_MODIFIER_LINEAR) == modifiers.end())
    log(FRAME3D_LOG_WARN, "SteamVR doesn't list the LINEAR layout; trying it anyway");

  if (!ctx->gbm.init(log))
    return fail();
  ctx->gbmReady = true;
  ctx->buffers.resize(cfg.buffer_count);
  for (Buffer& b : ctx->buffers) {
    if (!ctx->gbm.allocate(width, height, format, modifiers, linear, b.image, log))
      return fail();
    vr::DmabufAttributes_t attrs{};
    attrs.pNext = nullptr;
    attrs.unWidth = width;
    attrs.unHeight = height;
    attrs.unDepth = 1;
    attrs.unMipLevels = 1;
    attrs.unArrayLayers = 1;
    attrs.unSampleCount = 1;
    attrs.unFormat = format;
    attrs.ulModifier = b.image.modifier;
    attrs.unPlaneCount = b.image.planeCount;
    for (uint32_t p = 0; p < b.image.planeCount; ++p) {
      attrs.plane[p].unOffset = b.image.offsets[p];
      attrs.plane[p].unStride = b.image.strides[p];
      attrs.plane[p].nFd = b.image.fds[p];
    }
    if (!ipc->ImportDmabuf(vr::VRApplication_Overlay, &attrs, &b.handle) || b.handle == 0) {
      log(FRAME3D_LOG_ERROR, "SteamVR rejected a %ux%u %s image with layout %s", width, height,
          frame3d::fourcc_str(format).c_str(), frame3d::modifier_str(b.image.modifier).c_str());
      b.handle = 0;
      return fail();
    }
    // Gamescope takes the first reference right after importing.
    if (!ipc->RefResource(b.handle, nullptr))
      log(FRAME3D_LOG_WARN, "RefResource failed for an imported image");
  }
  log(FRAME3D_LOG_INFO, "%u x %ux%u %s images, layout %s", cfg.buffer_count, width, height,
      frame3d::fourcc_str(format).c_str(), frame3d::modifier_str(ctx->buffers[0].image.modifier).c_str());

  const std::string key = "frame3d." + std::to_string(getpid());
  vr::EVROverlayError oe = ov->CreateOverlay(key.c_str(), "Frame 3D", &ctx->overlay);
  if (oe != vr::VROverlayError_None) {
    log(FRAME3D_LOG_ERROR, "CreateOverlay: %s", ov->GetOverlayErrorNameFromEnum(oe));
    ctx->overlay = vr::k_ulOverlayHandleInvalid;
    return fail();
  }
  ov->SetOverlayFlag(ctx->overlay, vr::VROverlayFlags_SideBySide_Parallel, true);
  ov->SetOverlayFlag(ctx->overlay, vr::VROverlayFlags_IgnoreTextureAlpha, true);
  ov->SetOverlayTexelAspect(ctx->overlay, 1.0f);  // full SbS: each half keeps its own aspect
  ov->SetOverlayInputMethod(ctx->overlay, vr::VROverlayInputMethod_None);

  frame3d::ScreenFollower::Fallback fallback;
  fallback.distanceM = cfg.fallback_distance_m;
  fallback.widthM = cfg.fallback_width_m;
  fallback.aspect = float(cfg.eye_width) / float(cfg.eye_height);
  ctx->follower.init(ov, ctx->vr.system(), ctx->overlay, cfg.screen_key ? cfg.screen_key : "", fallback, &ctx->log);
  ctx->status.placement = FRAME3D_PLACEMENT_FIXED;
  ctx->status.width_m = ctx->follower.width();
  ctx->status.height_m = ctx->follower.height();

  frame3d_context* raw = ctx.get();
  ctx->worker = std::thread([raw] { raw->run(); });
  return ctx.release();
}

void frame3d_destroy(frame3d_context* ctx) {
  if (!ctx)
    return;
  ctx->teardown();
  delete ctx;
}

uint32_t frame3d_buffer_count(const frame3d_context* ctx) { return ctx ? uint32_t(ctx->buffers.size()) : 0; }

bool frame3d_get_buffer_info(const frame3d_context* ctx, uint32_t index, frame3d_buffer_info* info) {
  if (!ctx || index >= ctx->buffers.size() || !info)
    return false;
  const frame3d::GbmImage& img = ctx->buffers[index].image;
  *info = {};
  info->width = img.width;
  info->height = img.height;
  info->drm_format = img.format;
  info->drm_modifier = img.modifier;
  info->plane_count = img.planeCount;
  for (uint32_t p = 0; p < img.planeCount; ++p)
    info->planes[p] = {img.fds[p], img.offsets[p], img.strides[p]};
  return true;
}

int frame3d_acquire(frame3d_context* ctx) {
  if (!ctx)
    return -1;
  std::lock_guard lock(ctx->mutex);
  for (size_t i = 0; i < ctx->buffers.size(); ++i) {
    if (ctx->buffers[i].state == BufferState::Free) {
      ctx->buffers[i].state = BufferState::Acquired;
      return int(i);
    }
  }
  return -1;
}

bool frame3d_submit(frame3d_context* ctx, int index, int sync_fd) {
  if (!ctx || index < 0 || size_t(index) >= ctx->buffers.size()) {
    close_fd(sync_fd);
    return false;
  }
  {
    std::lock_guard lock(ctx->mutex);
    Buffer& b = ctx->buffers[size_t(index)];
    if (b.state != BufferState::Acquired) {
      close_fd(sync_fd);
      return false;
    }
    // A newer frame replaces one still waiting for the worker.
    if (ctx->pending >= 0) {
      ctx->buffers[size_t(ctx->pending)].state = BufferState::Free;
      close_fd(ctx->pendingFd);
    }
    b.state = BufferState::Pending;
    ctx->pending = index;
    ctx->pendingFd = sync_fd;
  }
  ctx->wake.notify_one();
  return true;
}

void frame3d_release(frame3d_context* ctx, int index) {
  if (!ctx || index < 0 || size_t(index) >= ctx->buffers.size())
    return;
  std::lock_guard lock(ctx->mutex);
  Buffer& b = ctx->buffers[size_t(index)];
  if (b.state == BufferState::Acquired)
    b.state = BufferState::Free;
}

void* frame3d_map(frame3d_context* ctx, int index, uint32_t* stride) {
  if (!ctx || index < 0 || size_t(index) >= ctx->buffers.size())
    return nullptr;
  {
    std::lock_guard lock(ctx->mutex);
    if (ctx->buffers[size_t(index)].state != BufferState::Acquired)
      return nullptr;
  }
  return ctx->gbm.map(ctx->buffers[size_t(index)].image, stride);
}

void frame3d_unmap(frame3d_context* ctx, int index) {
  if (!ctx || index < 0 || size_t(index) >= ctx->buffers.size())
    return;
  ctx->gbm.unmap(ctx->buffers[size_t(index)].image);
}

void frame3d_set_visible(frame3d_context* ctx, bool visible) {
  if (!ctx)
    return;
  {
    std::lock_guard lock(ctx->mutex);
    ctx->wantVisible = visible;
    ctx->visibilityChanged = true;
  }
  ctx->wake.notify_one();
}

void frame3d_recenter(frame3d_context* ctx) {
  if (!ctx)
    return;
  {
    std::lock_guard lock(ctx->mutex);
    ctx->recenterRequested = true;
  }
  ctx->wake.notify_one();
}

void frame3d_get_status(const frame3d_context* ctx, frame3d_status* status) {
  if (!status)
    return;
  *status = {};
  if (!ctx)
    return;
  std::lock_guard lock(ctx->mutex);
  *status = ctx->status;
}

}  // extern "C"
