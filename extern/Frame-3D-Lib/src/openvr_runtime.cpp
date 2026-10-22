#include "openvr_runtime.hpp"

#include <dlfcn.h>

#include <cstdlib>
#include <mutex>

namespace frame3d {
namespace {

// FRAME3D_OPENVR_LIB overrides; the Frame's runtime ships one beside SteamVR.
void* open_library() {
  const char* candidates[] = {
      std::getenv("FRAME3D_OPENVR_LIB"),
      "/opt/steamvr/bin/linuxarm64/libopenvr_api.so",
      "libopenvr_api.so",
  };
  for (const char* path : candidates) {
    if (!path || !*path)
      continue;
    if (void* lib = dlopen(path, RTLD_NOW | RTLD_LOCAL))
      return lib;
  }
  return nullptr;
}

template <typename Fn>
Fn sym(void* lib, const char* name) {
  return reinterpret_cast<Fn>(dlsym(lib, name));
}

}  // namespace

bool OpenVRRuntime::available() {
  static std::once_flag once;
  static bool result = false;
  std::call_once(once, [] {
    if (void* lib = open_library()) {
      result = dlsym(lib, "VR_InitInternal2") != nullptr;
      dlclose(lib);
    }
  });
  return result;
}

bool OpenVRRuntime::init(const Logger& log) {
  m_lib = open_library();
  if (!m_lib) {
    log(FRAME3D_LOG_ERROR, "libopenvr_api.so not loadable (set FRAME3D_OPENVR_LIB to its path)");
    return false;
  }
  const auto initInternal =
      sym<uint32_t (*)(vr::EVRInitError*, vr::EVRApplicationType, const char*)>(m_lib, "VR_InitInternal2");
  const auto getInterface = sym<void* (*)(const char*, vr::EVRInitError*)>(m_lib, "VR_GetGenericInterface");
  const auto errorSymbol = sym<const char* (*)(vr::EVRInitError)>(m_lib, "VR_GetVRInitErrorAsSymbol");
  m_shutdownInternal = sym<void (*)()>(m_lib, "VR_ShutdownInternal");
  if (!initInternal || !getInterface || !m_shutdownInternal) {
    log(FRAME3D_LOG_ERROR, "libopenvr_api.so is missing VR_InitInternal2 / VR_GetGenericInterface");
    return false;
  }
  const auto describe = [&](vr::EVRInitError e) {
    return errorSymbol ? std::string(errorSymbol(e)) : std::to_string(int(e));
  };

  vr::EVRInitError err = vr::VRInitError_None;
  initInternal(&err, vr::VRApplication_Overlay, nullptr);
  if (err != vr::VRInitError_None) {
    log(FRAME3D_LOG_ERROR, "connecting to SteamVR as an overlay app failed: %s", describe(err).c_str());
    return false;
  }
  m_initialized = true;

  m_overlay = static_cast<vr::IVROverlay*>(getInterface(vr::IVROverlay_Version, &err));
  if (!m_overlay) {
    log(FRAME3D_LOG_ERROR, "%s unavailable: %s", vr::IVROverlay_Version, describe(err).c_str());
    return false;
  }
  m_ipc = static_cast<vr::IVRIPCResourceManagerClient*>(
      getInterface(vr::IVRIPCResourceManagerClient_Version, &err));
  if (!m_ipc) {
    log(FRAME3D_LOG_ERROR, "%s unavailable: %s", vr::IVRIPCResourceManagerClient_Version, describe(err).c_str());
    return false;
  }
  // Optional: head pose for measuring the screen and for the fallback placement.
  m_system = static_cast<vr::IVRSystem*>(getInterface(vr::IVRSystem_Version, &err));
  if (!m_system)
    log(FRAME3D_LOG_WARN, "%s unavailable (%s); screen size can't be measured", vr::IVRSystem_Version,
        describe(err).c_str());
  return true;
}

void OpenVRRuntime::shutdown() {
  if (m_initialized && m_shutdownInternal)
    m_shutdownInternal();
  m_initialized = false;
  m_overlay = nullptr;
  m_system = nullptr;
  m_ipc = nullptr;
  if (m_lib) {
    dlclose(m_lib);
    m_lib = nullptr;
  }
}

}  // namespace frame3d
