#pragma once

#include "common.hpp"

#include <vector>

struct gbm_device;
struct gbm_bo;

namespace frame3d {

struct GbmImage {
  gbm_bo* bo = nullptr;
  uint32_t width = 0, height = 0, format = 0;
  uint64_t modifier = kModifierInvalid;
  uint32_t planeCount = 0;
  int fds[FRAME3D_MAX_PLANES] = {-1, -1, -1, -1};
  uint32_t offsets[FRAME3D_MAX_PLANES] = {};
  uint32_t strides[FRAME3D_MAX_PLANES] = {};
  void* mapData = nullptr;  // set while mapped
};

// libgbm loaded at runtime, on the first DRM render node.
class GbmAllocator {
public:
  bool init(const Logger& log);
  void shutdown();

  // linear: force DRM_FORMAT_MOD_LINEAR; otherwise the driver picks from modifiers.
  bool allocate(uint32_t width, uint32_t height, uint32_t format, const std::vector<uint64_t>& modifiers,
                bool linear, GbmImage& out, const Logger& log);
  void release(GbmImage& image);

  void* map(GbmImage& image, uint32_t* stride);
  void unmap(GbmImage& image);

private:
  void* m_lib = nullptr;
  int m_drmFd = -1;
  gbm_device* m_device = nullptr;

  gbm_device* (*m_createDevice)(int) = nullptr;
  void (*m_deviceDestroy)(gbm_device*) = nullptr;
  gbm_bo* (*m_boCreate)(gbm_device*, uint32_t, uint32_t, uint32_t, uint32_t) = nullptr;
  gbm_bo* (*m_boCreateWithModifiers)(gbm_device*, uint32_t, uint32_t, uint32_t, const uint64_t*, unsigned) = nullptr;
  gbm_bo* (*m_boCreateWithModifiers2)(gbm_device*, uint32_t, uint32_t, uint32_t, const uint64_t*, unsigned,
                                      uint32_t) = nullptr;
  void (*m_boDestroy)(gbm_bo*) = nullptr;
  int (*m_boGetFd)(gbm_bo*) = nullptr;
  int (*m_boGetFdForPlane)(gbm_bo*, int) = nullptr;
  uint32_t (*m_boGetStrideForPlane)(gbm_bo*, int) = nullptr;
  uint32_t (*m_boGetOffset)(gbm_bo*, int) = nullptr;
  uint64_t (*m_boGetModifier)(gbm_bo*) = nullptr;
  int (*m_boGetPlaneCount)(gbm_bo*) = nullptr;
  void* (*m_boMap)(gbm_bo*, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t*, void**) = nullptr;
  void (*m_boUnmap)(gbm_bo*, void*) = nullptr;
};

}  // namespace frame3d
