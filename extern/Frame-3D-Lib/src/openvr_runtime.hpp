#pragma once

#include "common.hpp"

#include "openvr.h"

namespace frame3d {

// libopenvr_api.so loaded at runtime. Only the entry points the C++ interfaces need
// are resolved; openvr.h's inline helpers (VR_Init etc.) are never used, so nothing
// links against the library.
class OpenVRRuntime {
public:
  // Cheap, cached: can libopenvr_api.so be loaded at all?
  static bool available();

  bool init(const Logger& log);
  void shutdown();

  vr::IVROverlay* overlay() const { return m_overlay; }
  vr::IVRSystem* system() const { return m_system; }
  vr::IVRIPCResourceManagerClient* ipc() const { return m_ipc; }

private:
  void* m_lib = nullptr;
  bool m_initialized = false;
  void (*m_shutdownInternal)() = nullptr;
  vr::IVROverlay* m_overlay = nullptr;
  vr::IVRSystem* m_system = nullptr;
  vr::IVRIPCResourceManagerClient* m_ipc = nullptr;
};

}  // namespace frame3d
