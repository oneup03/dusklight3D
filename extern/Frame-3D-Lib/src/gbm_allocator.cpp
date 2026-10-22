#include "gbm_allocator.hpp"

#include <dlfcn.h>
#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <type_traits>

namespace frame3d {
namespace {

constexpr uint32_t kUseRendering = 1u << 2;  // GBM_BO_USE_RENDERING
constexpr uint32_t kUseLinear = 1u << 4;     // GBM_BO_USE_LINEAR
constexpr uint32_t kTransferReadWrite = 3u;  // GBM_BO_TRANSFER_READ_WRITE

}  // namespace

bool GbmAllocator::init(const Logger& log) {
  m_lib = dlopen("libgbm.so.1", RTLD_NOW | RTLD_LOCAL);
  if (!m_lib)
    m_lib = dlopen("libgbm.so", RTLD_NOW | RTLD_LOCAL);
  if (!m_lib) {
    log(FRAME3D_LOG_ERROR, "libgbm not loadable: %s", dlerror());
    return false;
  }
  const auto sym = [&](auto& fn, const char* name) {
    fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(dlsym(m_lib, name));
    return fn != nullptr;
  };
  // Optional: newer GBM only.
  sym(m_boCreateWithModifiers, "gbm_bo_create_with_modifiers");
  sym(m_boCreateWithModifiers2, "gbm_bo_create_with_modifiers2");
  sym(m_boGetFdForPlane, "gbm_bo_get_fd_for_plane");
  if (!sym(m_createDevice, "gbm_create_device") || !sym(m_deviceDestroy, "gbm_device_destroy") ||
      !sym(m_boCreate, "gbm_bo_create") || !sym(m_boDestroy, "gbm_bo_destroy") || !sym(m_boGetFd, "gbm_bo_get_fd") ||
      !sym(m_boGetStrideForPlane, "gbm_bo_get_stride_for_plane") || !sym(m_boGetOffset, "gbm_bo_get_offset") ||
      !sym(m_boGetModifier, "gbm_bo_get_modifier") || !sym(m_boGetPlaneCount, "gbm_bo_get_plane_count") ||
      !sym(m_boMap, "gbm_bo_map") || !sym(m_boUnmap, "gbm_bo_unmap")) {
    log(FRAME3D_LOG_ERROR, "libgbm is missing required entry points");
    return false;
  }

  std::string node;
  for (int i = 128; i < 136 && m_drmFd < 0; ++i) {
    node = "/dev/dri/renderD" + std::to_string(i);
    m_drmFd = open(node.c_str(), O_RDWR | O_CLOEXEC);
  }
  if (m_drmFd < 0) {
    log(FRAME3D_LOG_ERROR, "no usable DRM render node: %s", std::strerror(errno));
    return false;
  }
  m_device = m_createDevice(m_drmFd);
  if (!m_device) {
    log(FRAME3D_LOG_ERROR, "gbm_create_device(%s) failed", node.c_str());
    return false;
  }
  log(FRAME3D_LOG_DEBUG, "GBM device on %s", node.c_str());
  return true;
}

void GbmAllocator::shutdown() {
  if (m_device) {
    m_deviceDestroy(m_device);
    m_device = nullptr;
  }
  if (m_drmFd >= 0) {
    close(m_drmFd);
    m_drmFd = -1;
  }
  if (m_lib) {
    dlclose(m_lib);
    m_lib = nullptr;
  }
}

bool GbmAllocator::allocate(uint32_t width, uint32_t height, uint32_t format, const std::vector<uint64_t>& modifiers,
                            bool linear, GbmImage& out, const Logger& log) {
  gbm_bo* bo = nullptr;
  const uint64_t linearMod = FRAME3D_MODIFIER_LINEAR;
  const uint64_t* mods = linear ? &linearMod : modifiers.data();
  const unsigned modCount = linear ? 1u : unsigned(modifiers.size());
  if (m_boCreateWithModifiers2 && modCount > 0)
    bo = m_boCreateWithModifiers2(m_device, width, height, format, mods, modCount, kUseRendering);
  if (!bo && m_boCreateWithModifiers && modCount > 0)
    bo = m_boCreateWithModifiers(m_device, width, height, format, mods, modCount);
  if (!bo)
    bo = m_boCreate(m_device, width, height, format, kUseRendering | (linear ? kUseLinear : 0u));
  if (!bo) {
    log(FRAME3D_LOG_ERROR, "allocating a %ux%u %s image failed", width, height, fourcc_str(format).c_str());
    return false;
  }

  out = {};
  out.bo = bo;
  out.width = width;
  out.height = height;
  out.format = format;
  out.modifier = m_boGetModifier(bo);
  out.planeCount = uint32_t(std::clamp(m_boGetPlaneCount(bo), 1, int(FRAME3D_MAX_PLANES)));
  for (uint32_t p = 0; p < out.planeCount; ++p) {
    out.fds[p] = m_boGetFdForPlane ? m_boGetFdForPlane(bo, int(p)) : (p == 0 ? m_boGetFd(bo) : -1);
    out.offsets[p] = m_boGetOffset(bo, int(p));
    out.strides[p] = m_boGetStrideForPlane(bo, int(p));
    if (out.fds[p] < 0) {
      log(FRAME3D_LOG_ERROR, "exporting plane %u of a %ux%u image failed", p, width, height);
      release(out);
      return false;
    }
  }
  return true;
}

void GbmAllocator::release(GbmImage& image) {
  if (image.mapData)
    unmap(image);
  for (int& fd : image.fds) {
    if (fd >= 0)
      close(fd);
    fd = -1;
  }
  if (image.bo)
    m_boDestroy(image.bo);
  image.bo = nullptr;
}

void* GbmAllocator::map(GbmImage& image, uint32_t* stride) {
  if (image.mapData)
    return nullptr;
  uint32_t pitch = 0;
  void* ptr = m_boMap(image.bo, 0, 0, image.width, image.height, kTransferReadWrite, &pitch, &image.mapData);
  if (!ptr) {
    image.mapData = nullptr;
    return nullptr;
  }
  if (stride)
    *stride = pitch;
  return ptr;
}

void GbmAllocator::unmap(GbmImage& image) {
  if (image.mapData) {
    m_boUnmap(image.bo, image.mapData);
    image.mapData = nullptr;
  }
}

}  // namespace frame3d
