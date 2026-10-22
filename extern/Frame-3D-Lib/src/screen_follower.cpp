#include "screen_follower.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace frame3d {
namespace {

using namespace std::chrono_literals;

constexpr float kInFrontM = 0.02f;  // our overlay sits this far toward the viewer
constexpr auto kProbeInterval = 100ms;
constexpr auto kFullInterval = 1s;
constexpr auto kActiveInterval = 16ms;
constexpr auto kSettleTime = 500ms;
constexpr auto kDiscoverInterval = 1s;
constexpr auto kChangeLogInterval = 250ms;
constexpr int kMaxFailures = 3;

void set_pose(vr::HmdMatrix34_t& m, Vec3 x, Vec3 y, Vec3 z, Vec3 pos) {
  const Vec3 cols[4] = {x, y, z, pos};
  for (int col = 0; col < 4; ++col) {
    m.m[0][col] = cols[col].x;
    m.m[1][col] = cols[col].y;
    m.m[2][col] = cols[col].z;
  }
}

Vec3 column(const vr::HmdMatrix34_t& m, int col) { return {m.m[0][col], m.m[1][col], m.m[2][col]}; }

// The Steam app ID Gamescope files this process's screen under: from our cgroup
// scope (app-steam-app<id>-<reaper pid>.scope, Gamescope's own lookup), else
// Steam's launch environment. 0 when not launched from Steam.
uint32_t own_app_id() {
  if (FILE* f = std::fopen("/proc/self/cgroup", "r")) {
    char line[1024];
    uint32_t appid = 0;
    while (appid == 0 && std::fgets(line, sizeof line, f)) {
      const char* scope = std::strrchr(line, '/');
      scope = scope ? scope + 1 : line;
      unsigned id = 0;
      int reaper = 0;
      if (std::sscanf(scope, "app-steam-app%u-%d.scope", &id, &reaper) == 2)
        appid = id;
    }
    std::fclose(f);
    if (appid != 0)
      return appid;
  }
  for (const char* name : {"SteamGameId", "SteamAppId"}) {
    if (const char* v = std::getenv(name)) {
      // Non-Steam shortcuts get a 64-bit game ID with the app ID in the top half.
      const unsigned long long id = std::strtoull(v, nullptr, 10);
      if (id > 0xffffffffull)
        return uint32_t(id >> 32);
      if (id != 0)
        return uint32_t(id);
    }
  }
  return 0;
}

const char* transform_type_name(vr::VROverlayTransformType t) {
  switch (t) {
  case vr::VROverlayTransform_Absolute:
    return "Absolute";
  case vr::VROverlayTransform_TrackedDeviceRelative:
    return "TrackedDeviceRelative";
  case vr::VROverlayTransform_DashboardTab:
    return "DashboardTab";
  case vr::VROverlayTransform_Mountable:
    return "Mountable";
  case vr::VROverlayTransform_Projection:
    return "Projection";
  default:
    return "other";
  }
}

}  // namespace

bool ScreenFollower::Placement::same_as(const Placement& o) const {
  // Measured placements jitter slightly with the head: compare within 1 mm and
  // 0.0001 of rotation rather than exactly.
  if (absolute != o.absolute || origin != o.origin || device != o.device)
    return false;
  for (int row = 0; row < 3; ++row) {
    for (int col = 0; col < 4; ++col) {
      if (std::fabs(matrix.m[row][col] - o.matrix.m[row][col]) > (col == 3 ? 1e-3f : 1e-4f))
        return false;
    }
  }
  return std::fabs(width - o.width) <= 1e-3f && std::fabs(curvature - o.curvature) <= 1e-4f;
}

void ScreenFollower::init(vr::IVROverlay* overlay, vr::IVRSystem* system, vr::VROverlayHandle_t ours,
                          std::string screenKey, Fallback fallback, const Logger* log) {
  m_overlay = overlay;
  m_system = system;
  m_ours = ours;
  m_fallback = fallback;
  m_log = log;
  m_key = std::move(screenKey);
  if (m_key.empty()) {
    if (const uint32_t appid = own_app_id())
      m_key = "valve.steam.desktopgame." + std::to_string(appid);
  }
  if (m_key.empty())
    (*m_log)(FRAME3D_LOG_INFO, "not launched from Steam: no game screen to follow, staying in front of the headset");
  else
    (*m_log)(FRAME3D_LOG_DEBUG, "will follow %s", m_key.c_str());
  place_in_front();
  m_nextDiscover = Clock::now();
}

ScreenFollower::Clock::duration ScreenFollower::tick(Clock::time_point now) {
  if (!following()) {
    // The fallback locks to the head only until there is a pose to place it with.
    Vec3 head{};
    if (!m_current.absolute && head_position(head))
      place_in_front();
    if (!m_key.empty() && now >= m_nextDiscover) {
      m_nextDiscover = now + kDiscoverInterval;
      discover(now);
    }
    if (!following())
      return m_key.empty() ? Clock::duration(kFullInterval) : Clock::duration(kProbeInterval);
  }

  bool full = m_active || now >= m_nextFull;
  if (now >= m_nextProbe) {
    m_nextProbe = now + kProbeInterval;
    Vec3 c{};
    if (probe_center(c) && length(c - m_probeCenter) > 1e-3f)
      full = true;
    // Mirror the screen's visibility, but only once it has been seen visible: if
    // a dashboard tab never reports visible, hiding ours would hide it for good.
    const bool vis = m_overlay->IsOverlayVisible(m_source);
    m_seenVisible = m_seenVisible || vis;
    const bool screenVisible = vis || !m_seenVisible;
    if (screenVisible != m_screenVisible) {
      m_screenVisible = screenVisible;
      (*m_log)(FRAME3D_LOG_DEBUG, "game screen %s", screenVisible ? "shown" : "hidden");
    }
  }

  if (full) {
    m_nextFull = now + kFullInterval;
    Placement p;
    std::string why;
    if (!measure(p, why)) {
      if (why != m_lastFailure) {
        (*m_log)(FRAME3D_LOG_WARN, "measuring the game screen: %s", why.c_str());
        m_lastFailure = why;
      }
      if (++m_failures >= kMaxFailures)
        lose_source(why.c_str());
    } else {
      m_failures = 0;
      m_lastFailure.clear();
      probe_center(m_probeCenter);
      if (!p.same_as(m_current)) {
        const bool wasActive = m_active;
        apply(p);
        m_active = true;
        m_lastChange = now;
        if (!wasActive || now - m_lastLog >= kChangeLogInterval) {
          m_lastLog = now;
          log_placement("changing");
        }
      }
    }
  }

  if (m_active && now - m_lastChange >= kSettleTime) {
    m_active = false;
    log_placement("settled");
  }
  if (!following())
    return kProbeInterval;
  if (m_active)
    return kActiveInterval;
  return std::max<Clock::duration>(1ms, std::min(m_nextProbe, m_nextFull) - now);
}

void ScreenFollower::recenter() {
  if (!following())
    place_in_front();
}

void ScreenFollower::discover(Clock::time_point now) {
  vr::VROverlayHandle_t handle = vr::k_ulOverlayHandleInvalid;
  const vr::EVROverlayError fe = m_overlay->FindOverlay(m_key.c_str(), &handle);
  if (fe != vr::VROverlayError_None) {
    const std::string why = std::string("game screen ") + m_key + " not found yet (" +
                            m_overlay->GetOverlayErrorNameFromEnum(fe) + ")";
    if (why != m_lastFailure) {
      (*m_log)(FRAME3D_LOG_DEBUG, "%s", why.c_str());
      m_lastFailure = why;
    }
    return;
  }

  vr::VROverlayTransformType type = vr::VROverlayTransform_Invalid;
  m_overlay->GetOverlayTransformType(handle, &type);
  m_source = handle;
  Placement p;
  std::string why;
  if (!measure(p, why)) {
    (*m_log)(FRAME3D_LOG_WARN, "found %s (%s) but can't measure it: %s", m_key.c_str(), transform_type_name(type),
             why.c_str());
    m_source = vr::k_ulOverlayHandleInvalid;
    return;
  }
  uint32_t sort = 0;
  m_overlay->GetOverlaySortOrder(handle, &sort);
  m_overlay->SetOverlaySortOrder(m_ours, sort == UINT32_MAX ? sort : sort + 1);
  apply(p);
  m_failures = 0;
  m_lastFailure.clear();
  m_seenVisible = false;
  m_screenVisible = true;
  m_active = true;
  m_lastChange = m_lastLog = now;
  m_nextProbe = now + kProbeInterval;
  m_nextFull = now + kFullInterval;
  probe_center(m_probeCenter);
  (*m_log)(FRAME3D_LOG_INFO, "following %s (%s)", m_key.c_str(), transform_type_name(type));
  log_placement("found");
}

bool ScreenFollower::measure(Placement& out, std::string& why) {
  vr::VROverlayTransformType type = vr::VROverlayTransform_Invalid;
  vr::EVROverlayError e = m_overlay->GetOverlayTransformType(m_source, &type);
  if (e != vr::VROverlayError_None) {
    why = std::string("GetOverlayTransformType: ") + m_overlay->GetOverlayErrorNameFromEnum(e);
    return false;
  }
  if (type != vr::VROverlayTransform_Absolute && type != vr::VROverlayTransform_TrackedDeviceRelative) {
    Vec3 head{};
    if (!head_position(head)) {
      why = "no head pose to measure from";
      return false;
    }
    return measure_dashboard(head, out, why);
  }

  // A screen with a readable transform (not the Frame's dashboard tab): copy it.
  out = {};
  out.absolute = type == vr::VROverlayTransform_Absolute;
  e = out.absolute ? m_overlay->GetOverlayTransformAbsolute(m_source, &out.origin, &out.matrix)
                   : m_overlay->GetOverlayTransformTrackedDeviceRelative(m_source, &out.device, &out.matrix);
  if (e != vr::VROverlayError_None) {
    why = std::string("reading the game screen transform: ") + m_overlay->GetOverlayErrorNameFromEnum(e);
    return false;
  }
  m_overlay->GetOverlayWidthInMeters(m_source, &out.width);
  m_overlay->GetOverlayCurvature(m_source, &out.curvature);
  vr::HmdVector2_t ms{};
  if (m_overlay->GetOverlayMouseScale(m_source, &ms) == vr::VROverlayError_None && ms.v[0] > 0.f)
    out.height = out.width * ms.v[1] / ms.v[0];
  const Vec3 z = normalize(column(out.matrix, 2));
  const Vec3 pos = column(out.matrix, 3) + z * kInFrontM;
  out.matrix.m[0][3] = pos.x;
  out.matrix.m[1][3] = pos.y;
  out.matrix.m[2][3] = pos.z;
  return true;
}

bool ScreenFollower::measure_dashboard(Vec3 head, Placement& out, std::string& why) {
  // Rough frame from GetTransformForOverlayCoordinates: the right center and
  // orientation, but the unscaled size. Coordinates are in mouse-scale units, 0,0 at
  // the lower left.
  vr::HmdVector2_t ms{};
  if (m_overlay->GetOverlayMouseScale(m_source, &ms) != vr::VROverlayError_None || ms.v[0] <= 0.f ||
      ms.v[1] <= 0.f)
    ms.v[0] = ms.v[1] = 1.f;
  m_mouseScale = ms;
  const float w = ms.v[0], h = ms.v[1];
  const auto at = [&](float u, float v, Vec3& p) {
    vr::HmdMatrix34_t m{};
    const vr::EVROverlayError e =
        m_overlay->GetTransformForOverlayCoordinates(m_source, vr::TrackingUniverseStanding, vr::HmdVector2_t{{u, v}}, &m);
    if (e != vr::VROverlayError_None) {
      why = std::string("GetTransformForOverlayCoordinates: ") + m_overlay->GetOverlayErrorNameFromEnum(e);
      return false;
    }
    p = column(m, 3);
    return true;
  };
  Vec3 c{}, l{}, r{}, t{};
  if (!at(w * 0.5f, h * 0.5f, c) || !at(0.f, h * 0.5f, l) || !at(w, h * 0.5f, r) || !at(w * 0.5f, h, t))
    return false;
  const float pointsW = length(r - l);
  if (pointsW < 1e-4f) {
    why = "the game screen reports zero width";
    return false;
  }
  const Vec3 x = (r - l) * (1.f / pointsW);
  Vec3 up = t - c;
  up = up - x * dot(up, x);
  const float pointsH = 2.f * length(up);
  if (pointsH < 1e-4f) {
    why = "the game screen reports zero height";
    return false;
  }
  const Vec3 y = up * (2.f / pointsH);

  // Rays: center, left, right and above. Aim with the last measured width when we
  // have one, since the unscaled width can be far off; retry closer in on a miss.
  struct Hit {
    Vec3 p;
    float u, v;
  };
  const auto cast = [&](Vec3 target, Hit& hit) {
    const Vec3 dir = target - head;
    const float len = length(dir);
    if (len < 1e-4f)
      return false;
    vr::VROverlayIntersectionParams_t params{};
    params.vSource = {{head.x, head.y, head.z}};
    params.vDirection = {{dir.x / len, dir.y / len, dir.z / len}};
    params.eOrigin = vr::TrackingUniverseStanding;
    vr::VROverlayIntersectionResults_t res{};
    if (!m_overlay->ComputeOverlayIntersection(m_source, &params, &res))
      return false;
    hit = {{res.vPoint.v[0], res.vPoint.v[1], res.vPoint.v[2]}, res.vUVs.v[0], res.vUVs.v[1]};
    return true;
  };
  const float aimW = m_current.measuredByRays && m_current.width > 0.f ? m_current.width : pointsW;
  Hit hc{}, hl{}, hr{}, hu{};
  bool hitAll = false;
  for (const float o : {0.35f * aimW, 0.15f * std::min(aimW, pointsW)}) {
    const float ov = o * pointsH / pointsW;
    if (cast(c, hc) && cast(c - x * o, hl) && cast(c + x * o, hr) && cast(c + y * ov, hu)) {
      hitAll = true;
      break;
    }
  }
  if (!hitAll) {
    why = "rays missed the game screen";
    return false;
  }
  // UVs come back normalized or in mouse-scale units; the center hit tells which.
  if (std::fabs(hc.u) > 1.5f || std::fabs(hc.v) > 1.5f) {
    for (Hit* hit : {&hc, &hl, &hr, &hu}) {
      hit->u /= w;
      hit->v /= h;
    }
  }
  const float du = hr.u - hl.u, dv = hu.v - hc.v;
  if (std::fabs(du) < 1e-4f || std::fabs(dv) < 1e-4f) {
    why = "ray hits too close together to measure";
    return false;
  }

  // The side rays leave the head at a different height than the screen, so where
  // the surface curves forward their hits land slightly off the center hit's
  // height. Project them onto the plane through the center hit perpendicular to the
  // screen's vertical axis so the fitted circle doesn't tilt.
  const auto level = [&](Vec3 q) { return q - y * dot(q - hc.p, y); };
  const Vec3 pl = level(hl.p), pr = level(hr.p);

  Vec3 center{}, xr{}, zr{};
  float width = 0.f, curvature = 0.f;
  const Vec3 a = pl - hc.p, b = pr - hc.p, n = cross(a, b);
  const float n2 = dot(n, n);
  if (n2 > 1e-8f * dot(a, a) * dot(b, b)) {
    // Curved: the three hits lie on a circle around the cylinder's (vertical) axis.
    const Vec3 o = hc.p + (cross(b, n) * dot(a, a) + cross(n, a) * dot(b, b)) * (1.f / (2.f * n2));
    const float radius = length(hc.p - o);
    const Vec3 e1 = (hc.p - o) * (1.f / radius);
    const Vec3 e2 = cross(normalize(n), e1);
    const auto angle = [&](Vec3 q) {
      const Vec3 d = q - o;
      return std::atan2(dot(d, e2), dot(d, e1));
    };
    const float span = angle(pr) - angle(pl);
    if (std::fabs(span) < 1e-6f) {
      why = "ray hits too close together to measure";
      return false;
    }
    const float uPerRad = du / span;
    const float ac = (0.5f - hc.u) / uPerRad;  // angle from the center hit to u = 0.5
    width = radius / std::fabs(uPerRad);
    center = o + (e1 * std::cos(ac) + e2 * std::sin(ac)) * radius;
    const Vec3 radial = (center - o) * (1.f / radius);
    xr = (e2 * std::cos(ac) - e1 * std::sin(ac)) * (uPerRad > 0.f ? 1.f : -1.f);  // toward increasing u
    if (dot(o - center, head - center) > 0.f) {
      // Curves toward the viewer, which is how OpenVR curves overlays.
      zr = radial * -1.f;
      curvature = width / (2.f * kPi * radius);
    } else {
      zr = radial;
      if (!m_warnedConvex) {
        (*m_log)(FRAME3D_LOG_WARN, "the game screen curves away from the viewer; drawing ours flat");
        m_warnedConvex = true;
      }
    }
  } else {
    // Flat.
    const Vec3 d = pr - pl;
    width = length(d) / std::fabs(du);
    xr = normalize(d) * (du > 0.f ? 1.f : -1.f);
    center = hc.p + xr * ((0.5f - hc.u) * width);
    zr = normalize(cross(xr, normalize(y - xr * dot(y, xr))));
  }
  if (!std::isfinite(width) || width <= 0.f || curvature >= 1.f) {
    why = "measured an implausible screen";
    return false;
  }
  const Vec3 yr = cross(zr, xr);
  // Height and vertical center from the hit above center: along the axis the screen
  // is straight, so v is linear in height.
  const float rise = dot(hu.p - hc.p, yr);
  if (std::fabs(rise) < 1e-5f) {
    why = "ray hits too close together to measure";
    return false;
  }
  center = center + yr * (rise * (0.5f - hc.v) / dv);

  out = {};
  out.absolute = true;
  out.origin = vr::TrackingUniverseStanding;
  set_pose(out.matrix, xr, yr, zr, center + zr * kInFrontM);
  out.width = width;
  out.height = std::fabs(rise / dv);
  out.curvature = curvature;
  out.measuredByRays = true;
  return true;
}

bool ScreenFollower::head_position(Vec3& out) const {
  if (!m_system)
    return false;
  vr::TrackedDevicePose_t pose{};
  m_system->GetDeviceToAbsoluteTrackingPose(vr::TrackingUniverseStanding, 0.f, &pose, 1);
  if (!pose.bPoseIsValid)
    return false;
  out = column(pose.mDeviceToAbsoluteTracking, 3);
  return true;
}

// One call: where the screen's center is, to notice it moving between full measurements.
bool ScreenFollower::probe_center(Vec3& out) const {
  vr::HmdMatrix34_t m{};
  if (m_overlay->GetTransformForOverlayCoordinates(m_source, vr::TrackingUniverseStanding,
                                                   vr::HmdVector2_t{{m_mouseScale.v[0] * 0.5f, m_mouseScale.v[1] * 0.5f}},
                                                   &m) != vr::VROverlayError_None)
    return false;
  out = column(m, 3);
  return true;
}

void ScreenFollower::apply(const Placement& p) {
  if (p.absolute)
    m_overlay->SetOverlayTransformAbsolute(m_ours, p.origin, &p.matrix);
  else
    m_overlay->SetOverlayTransformTrackedDeviceRelative(m_ours, p.device, &p.matrix);
  if (p.width > 0.f)
    m_overlay->SetOverlayWidthInMeters(m_ours, p.width);
  m_overlay->SetOverlayCurvature(m_ours, p.curvature);
  m_current = p;
}

// Level, yaw-only, fallback.distanceM ahead of the headset and facing it; locked to
// the head if there's no pose yet.
void ScreenFollower::place_in_front() {
  Placement p;
  p.width = m_fallback.widthM;
  p.height = m_fallback.aspect > 0.f ? m_fallback.widthM / m_fallback.aspect : 0.f;
  vr::TrackedDevicePose_t pose{};
  if (m_system)
    m_system->GetDeviceToAbsoluteTrackingPose(vr::TrackingUniverseStanding, 0.f, &pose, 1);
  if (!pose.bPoseIsValid) {
    p.absolute = false;
    p.device = vr::k_unTrackedDeviceIndex_Hmd;
    set_pose(p.matrix, {1.f, 0.f, 0.f}, {0.f, 1.f, 0.f}, {0.f, 0.f, 1.f}, {0.f, 0.f, -m_fallback.distanceM});
  } else {
    const auto& hm = pose.mDeviceToAbsoluteTracking.m;
    const float yaw = std::atan2(hm[0][2], hm[2][2]);
    const float cy = std::cos(yaw), sy = std::sin(yaw);
    const Vec3 headPos{hm[0][3], hm[1][3], hm[2][3]};
    set_pose(p.matrix, {cy, 0.f, -sy}, {0.f, 1.f, 0.f}, {sy, 0.f, cy},
             headPos + Vec3{-sy, 0.f, -cy} * m_fallback.distanceM);
  }
  apply(p);
}

void ScreenFollower::lose_source(const char* why) {
  (*m_log)(FRAME3D_LOG_WARN, "lost the game screen (%s); keeping the last placement", why);
  m_source = vr::k_ulOverlayHandleInvalid;
  m_active = false;
  m_failures = 0;
  m_screenVisible = true;
  m_seenVisible = false;
  m_nextDiscover = Clock::now() + kDiscoverInterval;
}

void ScreenFollower::log_placement(const char* what) const {
  const Vec3 c = column(m_current.matrix, 3);
  Vec3 head{};
  const float dist = head_position(head) ? length(c - head) : -1.f;
  const float radius = m_current.curvature > 0.f ? m_current.width / (2.f * kPi * m_current.curvature) : 0.f;
  (*m_log)(std::strcmp(what, "changing") == 0 ? FRAME3D_LOG_DEBUG : FRAME3D_LOG_INFO,
           "screen %s: %.2f x %.2f m, curvature %.3f (radius %.2f m), center (%.2f, %.2f, %.2f), %.2f m from head",
           what, m_current.width, m_current.height, m_current.curvature, radius, c.x, c.y, c.z, dist);
}

}  // namespace frame3d
