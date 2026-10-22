#pragma once

#include "common.hpp"

#include "openvr.h"

#include <chrono>

namespace frame3d {

// Keeps our overlay over the app's Steam game screen.
//
// On the Frame that screen is a DashboardTab overlay: the dashboard places it, and
// there is no getter for that transform. GetTransformForOverlayCoordinates reports
// the right center but ignores the scale applied when the user resizes the screen,
// and GetOverlayCurvature reports 0 although the dashboard curves it. So the
// geometry is measured with the laser hit test (ComputeOverlayIntersection), which
// has to match what is drawn: rays from the head through the left edge, center and
// right edge give three surface points whose circle is the screen's curvature, and
// their UVs give its true width and center.
//
// Cadence: a one-call movement check every 100 ms and a full measurement every
// second while the screen is still; a full measurement every tick (~60 Hz) while it
// changes, until it has been still for 0.5 s.
class ScreenFollower {
public:
  using Clock = std::chrono::steady_clock;

  struct Fallback {
    float distanceM = 2.0f;
    float widthM = 1.6f;
    float aspect = 16.f / 9.f;  // per-eye width / height, for the reported height
  };

  void init(vr::IVROverlay* overlay, vr::IVRSystem* system, vr::VROverlayHandle_t ours, std::string screenKey,
            Fallback fallback, const Logger* log);

  // Runs the cadence above. Returns how long the caller may sleep before the next tick.
  Clock::duration tick(Clock::time_point now);

  // Re-place in front of the headset (only while there is no screen to follow).
  void recenter();

  bool following() const { return m_source != vr::k_ulOverlayHandleInvalid; }
  // False only once the followed screen has been seen visible and then hidden.
  bool screen_visible() const { return m_screenVisible; }
  float width() const { return m_current.width; }
  float height() const { return m_current.height; }
  float curvature() const { return m_current.curvature; }

private:
  struct Placement {
    bool absolute = true;  // else relative to m_current.device
    vr::ETrackingUniverseOrigin origin = vr::TrackingUniverseStanding;
    vr::TrackedDeviceIndex_t device = 0;
    vr::HmdMatrix34_t matrix{};
    float width = 0.f, height = 0.f, curvature = 0.f;
    bool measuredByRays = false;

    bool same_as(const Placement& o) const;
  };

  void discover(Clock::time_point now);
  bool measure(Placement& out, std::string& why);
  bool measure_dashboard(Vec3 head, Placement& out, std::string& why);
  bool head_position(Vec3& out) const;
  bool probe_center(Vec3& out) const;
  void apply(const Placement& p);
  void place_in_front();
  void lose_source(const char* why);
  void log_placement(const char* what) const;

  vr::IVROverlay* m_overlay = nullptr;
  vr::IVRSystem* m_system = nullptr;
  vr::VROverlayHandle_t m_ours = vr::k_ulOverlayHandleInvalid;
  std::string m_key;
  Fallback m_fallback;
  const Logger* m_log = nullptr;

  vr::VROverlayHandle_t m_source = vr::k_ulOverlayHandleInvalid;
  Placement m_current;
  vr::HmdVector2_t m_mouseScale{{1.f, 1.f}};
  Vec3 m_probeCenter{};
  bool m_screenVisible = true;
  bool m_seenVisible = false;
  bool m_active = false;
  bool m_warnedConvex = false;
  int m_failures = 0;
  std::string m_lastFailure;
  Clock::time_point m_nextDiscover{}, m_nextProbe{}, m_nextFull{}, m_lastChange{}, m_lastLog{};
};

}  // namespace frame3d
