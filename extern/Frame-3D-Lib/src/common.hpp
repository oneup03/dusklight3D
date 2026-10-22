#pragma once

#include "frame3d/frame3d.h"

#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <string>

namespace frame3d {

constexpr float kPi = 3.14159265358979323846f;
constexpr uint64_t kModifierInvalid = 0x00ffffffffffffffull;  // DRM_FORMAT_MOD_INVALID

class Logger {
public:
  void configure(frame3d_log_fn fn, void* user, frame3d_log_level level) {
    m_fn = fn;
    m_user = user;
    m_level = level;
  }

#if defined(__GNUC__)
  __attribute__((format(printf, 3, 4)))
#endif
  void
  operator()(frame3d_log_level level, const char* fmt, ...) const {
    if (level < m_level)
      return;
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buf, sizeof buf, fmt, args);
    va_end(args);
    if (m_fn) {
      m_fn(level, buf, m_user);
      return;
    }
    static const char* const kTags[] = {"debug", "info", "warn", "error"};
    std::fprintf(stderr, "[frame3d %s] %s\n", kTags[level], buf);
  }

private:
  frame3d_log_fn m_fn = nullptr;
  void* m_user = nullptr;
  frame3d_log_level m_level = FRAME3D_LOG_INFO;
};

struct Vec3 {
  float x, y, z;
};
inline Vec3 operator+(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline Vec3 operator-(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline Vec3 operator*(Vec3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
inline float dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3 cross(Vec3 a, Vec3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
inline float length(Vec3 a) { return std::sqrt(dot(a, a)); }
inline Vec3 normalize(Vec3 a) {
  const float len = length(a);
  return len > 0.f ? a * (1.f / len) : a;
}

inline std::string fourcc_str(uint32_t f) {
  char s[5] = {char(f & 0xff), char((f >> 8) & 0xff), char((f >> 16) & 0xff), char((f >> 24) & 0xff), 0};
  for (int i = 0; i < 4; ++i) {
    if (s[i] < 32 || s[i] > 126)
      s[i] = '?';
  }
  return s;
}

inline std::string modifier_str(uint64_t m) {
  if (m == FRAME3D_MODIFIER_LINEAR)
    return "LINEAR";
  if (m == kModifierInvalid)
    return "INVALID";
  char buf[32];
  std::snprintf(buf, sizeof buf, "0x%016llx", static_cast<unsigned long long>(m));
  return buf;
}

}  // namespace frame3d
