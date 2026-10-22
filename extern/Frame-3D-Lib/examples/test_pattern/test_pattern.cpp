// Frame-3D-Lib test pattern: a full-SbS stereo test image shown through the library,
// for checking a device without a real renderer. Launch it from Steam (as a
// non-Steam game) so it gets a game screen to follow; it opens a blank window for
// that. With no terminal (launched from Steam) it logs to ~/frame3d-test-pattern.log.
//
// What to look for in the headset:
//   1. Eye order: right eye closed -> RED square top-left; left eye closed -> BLUE
//      square top-right.
//   2. Depth: yellow square in front of the screen, green on it, cyan behind it.
//   3. Resolution: each grating pair is vertical | horizontal lines at 1, 2, 3 px;
//      leaning in, the vertical half should be as sharp as the horizontal half.
//   4. Motion: the magenta bar (in front) slides smoothly with no flicker.
//   5. Placement: the pattern covers the game screen exactly -- edges, size and
//      curvature -- and follows it when moved or resized.

#include "frame3d/frame3d.h"

#include <dlfcn.h>
#include <signal.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

namespace {

std::atomic<bool> g_quit{false};
void on_signal(int) { g_quit = true; }

struct Options {
  uint32_t eyeWidth = 1920;
  uint32_t eyeHeight = 1080;
  bool animate = true;
  bool window = true;
  bool verbose = true;
  double fps = 60.0;
  double seconds = 0.0;
  std::string screenKey;
  std::string logPath;
};

void usage() {
  std::printf(
      "Usage: frame3d-test-pattern [options]\n"
      "  --eye WxH       per-eye resolution (default 1920x1080)\n"
      "  --static        one frame, no moving bar\n"
      "  --fps N         submit rate (default 60)\n"
      "  --seconds N     exit after N seconds (default: until quit)\n"
      "  --screen KEY    SteamVR overlay to follow (default: this app's game screen)\n"
      "  --no-window     don't open a window\n"
      "  --quiet         don't log every placement change\n"
      "  --log PATH      output file (default ~/frame3d-test-pattern.log without a terminal)\n");
}

bool parse_args(int argc, char** argv, Options& o) {
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    const auto next = [&]() -> const char* {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "%s needs a value\n", a.c_str());
        std::exit(2);
      }
      return argv[++i];
    };
    if (a == "--help" || a == "-h") {
      usage();
      std::exit(0);
    } else if (a == "--eye") {
      unsigned w = 0, h = 0;
      if (std::sscanf(next(), "%ux%u", &w, &h) != 2 || w == 0 || h == 0) {
        std::fprintf(stderr, "--eye expects WxH\n");
        return false;
      }
      o.eyeWidth = w;
      o.eyeHeight = h;
    } else if (a == "--static") {
      o.animate = false;
    } else if (a == "--fps") {
      o.fps = std::max(1.0, std::strtod(next(), nullptr));
    } else if (a == "--seconds") {
      o.seconds = std::strtod(next(), nullptr);
    } else if (a == "--screen") {
      o.screenKey = next();
    } else if (a == "--no-window") {
      o.window = false;
    } else if (a == "--quiet") {
      o.verbose = false;
    } else if (a == "--log") {
      o.logPath = next();
    } else {
      std::fprintf(stderr, "Unknown option %s\n", a.c_str());
      usage();
      return false;
    }
  }
  return true;
}

// ---------------------------------------------------------------- X11 window
//
// Blank window so Gamescope gives this process a game screen to follow. Loaded at
// runtime; only enough of the Xlib ABI to open, title and pump one window.

using XID = unsigned long;
struct XDisplay;
struct XGCRec;
union XEventT {
  int type;
  long pad[24];  // ClientMessage: data.l[0] is pad[7] on LP64
};
constexpr int kExpose = 12;
constexpr int kClientMessage = 33;
constexpr long kExposureMask = 1L << 15;

struct X11Window {
  void* lib = nullptr;
  XDisplay* dpy = nullptr;
  XID win = 0;
  XID wmDelete = 0;
  std::vector<std::string> lines;

  XDisplay* (*OpenDisplay)(const char*) = nullptr;
  int (*CloseDisplay)(XDisplay*) = nullptr;
  int (*DefaultScreen)(XDisplay*) = nullptr;
  XID (*RootWindow)(XDisplay*, int) = nullptr;
  unsigned long (*BlackPixel)(XDisplay*, int) = nullptr;
  unsigned long (*WhitePixel)(XDisplay*, int) = nullptr;
  XID (*CreateSimpleWindow)(XDisplay*, XID, int, int, unsigned, unsigned, unsigned, unsigned long,
                            unsigned long) = nullptr;
  int (*StoreName)(XDisplay*, XID, const char*) = nullptr;
  XID (*InternAtom)(XDisplay*, const char*, int) = nullptr;
  int (*SetWMProtocols)(XDisplay*, XID, XID*, int) = nullptr;
  int (*SelectInput)(XDisplay*, XID, long) = nullptr;
  int (*MapWindow)(XDisplay*, XID) = nullptr;
  int (*Flush)(XDisplay*) = nullptr;
  int (*Pending)(XDisplay*) = nullptr;
  int (*NextEvent)(XDisplay*, XEventT*) = nullptr;
  int (*DestroyWindow)(XDisplay*, XID) = nullptr;
  XGCRec* (*DefaultGC)(XDisplay*, int) = nullptr;
  int (*SetForeground)(XDisplay*, XGCRec*, unsigned long) = nullptr;
  int (*DrawString)(XDisplay*, XID, XGCRec*, int, int, const char*, int) = nullptr;

  bool create(uint32_t width, uint32_t height, std::vector<std::string> text) {
    lib = dlopen("libX11.so.6", RTLD_NOW | RTLD_LOCAL);
    if (!lib) {
      std::printf("[test] libX11.so.6 not loadable: %s\n", dlerror());
      return false;
    }
    const auto sym = [&](auto& fn, const char* name) {
      fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(dlsym(lib, name));
      return fn != nullptr;
    };
    if (!sym(OpenDisplay, "XOpenDisplay") || !sym(CloseDisplay, "XCloseDisplay") ||
        !sym(DefaultScreen, "XDefaultScreen") || !sym(RootWindow, "XRootWindow") ||
        !sym(BlackPixel, "XBlackPixel") || !sym(WhitePixel, "XWhitePixel") ||
        !sym(CreateSimpleWindow, "XCreateSimpleWindow") || !sym(StoreName, "XStoreName") ||
        !sym(InternAtom, "XInternAtom") || !sym(SetWMProtocols, "XSetWMProtocols") ||
        !sym(SelectInput, "XSelectInput") || !sym(MapWindow, "XMapWindow") || !sym(Flush, "XFlush") ||
        !sym(Pending, "XPending") || !sym(NextEvent, "XNextEvent") || !sym(DestroyWindow, "XDestroyWindow") ||
        !sym(DefaultGC, "XDefaultGC") || !sym(SetForeground, "XSetForeground") ||
        !sym(DrawString, "XDrawString")) {
      std::printf("[test] libX11 is missing required entry points\n");
      return false;
    }
    dpy = OpenDisplay(nullptr);
    if (!dpy) {
      std::printf("[test] XOpenDisplay failed\n");
      return false;
    }
    const int screen = DefaultScreen(dpy);
    win = CreateSimpleWindow(dpy, RootWindow(dpy, screen), 0, 0, width, height, 0, BlackPixel(dpy, screen),
                             BlackPixel(dpy, screen));
    StoreName(dpy, win, "Frame-3D-Lib test pattern");
    wmDelete = InternAtom(dpy, "WM_DELETE_WINDOW", 0);
    SetWMProtocols(dpy, win, &wmDelete, 1);
    SelectInput(dpy, win, kExposureMask);
    MapWindow(dpy, win);
    lines = std::move(text);
    draw();
    return true;
  }

  void draw() {
    const int screen = DefaultScreen(dpy);
    XGCRec* gc = DefaultGC(dpy, screen);
    SetForeground(dpy, gc, WhitePixel(dpy, screen));
    for (size_t i = 0; i < lines.size(); ++i)
      DrawString(dpy, win, gc, 24, 40 + int(i) * 20, lines[i].c_str(), int(lines[i].size()));
    Flush(dpy);
  }

  // False once the window is asked to close.
  bool pump() {
    while (Pending(dpy) > 0) {
      XEventT ev{};
      NextEvent(dpy, &ev);
      if (ev.type == kExpose)
        draw();
      else if (ev.type == kClientMessage && XID(ev.pad[7]) == wmDelete)
        return false;
    }
    return true;
  }

  void destroy() {
    if (dpy) {
      DestroyWindow(dpy, win);
      CloseDisplay(dpy);
      dpy = nullptr;
    }
  }
};

// ------------------------------------------------------------ test pattern

struct Canvas {
  uint32_t width = 0, height = 0;
  bool bgra = false;
  std::vector<uint32_t> px;

  uint32_t rgb(uint8_t r, uint8_t g, uint8_t b) const {
    return bgra ? 0xff000000u | (uint32_t(r) << 16) | (uint32_t(g) << 8) | b
                : 0xff000000u | (uint32_t(b) << 16) | (uint32_t(g) << 8) | r;
  }

  void fill(int x, int y, int w, int h, uint32_t c) {
    const int x0 = std::max(x, 0), y0 = std::max(y, 0);
    const int x1 = std::min<int>(x + w, int(width)), y1 = std::min<int>(y + h, int(height));
    for (int yy = y0; yy < y1; ++yy)
      std::fill(px.begin() + size_t(yy) * width + x0, px.begin() + size_t(yy) * width + x1, c);
  }
};

struct Layout {
  int eyeW, eyeH, disparity, barY, barH, barW;
};

Layout make_layout(uint32_t eyeW, uint32_t eyeH) {
  const double s = eyeH / 1080.0;
  Layout l{};
  l.eyeW = int(eyeW);
  l.eyeH = int(eyeH);
  l.disparity = std::max(2, int(std::lround(eyeW * 0.012)));
  l.barY = int(eyeH * 0.58);
  l.barH = int(eyeH * 0.08);
  l.barW = std::max(8, int(40 * s));
  return l;
}

// Both eyes share one layout; only the eye tag and per-object disparity differ.
void draw_pattern(Canvas& c, const Layout& l) {
  const double s = l.eyeH / 1080.0;
  const uint32_t bg = c.rgb(48, 48, 48), white = c.rgb(255, 255, 255), black = c.rgb(0, 0, 0);
  c.fill(0, 0, int(c.width), int(c.height), bg);
  for (int eye = 0; eye < 2; ++eye) {
    const int ox = eye * l.eyeW;
    const int border = std::max(2, int(6 * s));
    c.fill(ox, 0, l.eyeW, border, white);
    c.fill(ox, l.eyeH - border, l.eyeW, border, white);
    c.fill(ox, 0, border, l.eyeH, white);
    c.fill(ox + l.eyeW - border, 0, border, l.eyeH, white);

    const int tag = int(160 * s), inset = int(48 * s);
    if (eye == 0)
      c.fill(ox + inset, inset, tag, tag, c.rgb(230, 30, 30));
    else
      c.fill(ox + l.eyeW - inset - tag, inset, tag, tag, c.rgb(30, 80, 255));

    // Crossed disparity (in front), none (on the screen), uncrossed (behind).
    const int q = int(200 * s), qy = int(l.eyeH * 0.40) - q / 2;
    const struct {
      double cx;
      int sign;
      uint32_t color;
    } squares[] = {
        {0.25, +1, c.rgb(240, 220, 40)},
        {0.50, 0, c.rgb(40, 200, 80)},
        {0.75, -1, c.rgb(40, 200, 220)},
    };
    for (const auto& sq : squares) {
      const int x = ox + int(l.eyeW * sq.cx) - q / 2 + (eye == 0 ? 1 : -1) * sq.sign * l.disparity;
      c.fill(x - border, qy - border, q + 2 * border, q + 2 * border, white);
      c.fill(x, qy, q, q, sq.color);
    }

    c.fill(ox + int(l.eyeW * 0.1), l.barY + l.barH / 2, int(l.eyeW * 0.8), std::max(1, int(2 * s)),
           c.rgb(90, 90, 90));

    // Gratings: vertical | horizontal lines at 1, 2, 3 px.
    const int g = int(150 * s), gap = int(40 * s), gy = int(l.eyeH * 0.70);
    int gx = ox + (l.eyeW - (3 * (2 * g + gap) - gap)) / 2;
    const auto plot = [&](int x, int y, uint32_t color) {
      if (x >= ox && x < ox + l.eyeW && y >= 0 && y < l.eyeH)
        c.px[size_t(y) * c.width + x] = color;
    };
    for (int pitch = 1; pitch <= 3; ++pitch) {
      for (int yy = 0; yy < g; ++yy) {
        for (int xx = 0; xx < g; ++xx) {
          plot(gx + xx, gy + yy, (xx / pitch) % 2 == 0 ? white : black);
          plot(gx + g + xx, gy + yy, (yy / pitch) % 2 == 0 ? white : black);
        }
      }
      gx += 2 * g + gap;
    }

    const int rampY = gy + g + int(30 * s), rampH = int(36 * s);
    const int rampX = ox + int(l.eyeW * 0.1), rampW = int(l.eyeW * 0.8);
    for (int i = 0; i < 16; ++i) {
      const uint8_t v = uint8_t(i * 17);
      c.fill(rampX + rampW * i / 16, rampY, rampW / 16 + 1, rampH, c.rgb(v, v, v));
    }
  }
}

void draw_bar(Canvas& c, const Layout& l, double t) {
  const double phase = 0.5 + 0.5 * std::sin(t * 2.0 * 3.14159265358979323846 / 4.0);
  const int bx = int(l.eyeW * 0.1) + int(phase * (int(l.eyeW * 0.8) - l.barW));
  for (int eye = 0; eye < 2; ++eye)
    c.fill(eye * l.eyeW + bx + (eye == 0 ? l.disparity : -l.disparity), l.barY, l.barW, l.barH,
           c.rgb(230, 40, 220));
}

void log_to_stdout(frame3d_log_level level, const char* message, void*) {
  static const char* const kTags[] = {"debug", "info", "warn", "error"};
  std::printf("[frame3d %s] %s\n", kTags[level], message);
}

}  // namespace

int main(int argc, char** argv) {
  Options opt;
  if (!parse_args(argc, argv, opt))
    return 2;
  signal(SIGINT, on_signal);
  signal(SIGTERM, on_signal);

  std::string logPath = opt.logPath;
  if (logPath.empty() && !isatty(STDOUT_FILENO)) {
    const char* home = std::getenv("HOME");
    logPath = std::string(home && *home ? home : "/tmp") + "/frame3d-test-pattern.log";
  }
  if (!logPath.empty()) {
    if (std::freopen(logPath.c_str(), "w", stdout))
      dup2(STDOUT_FILENO, STDERR_FILENO);
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
  }
  // Steam's LD_PRELOAD (overlay renderer) only produces loader errors in children.
  unsetenv("LD_PRELOAD");

  std::printf("[test] Frame-3D-Lib test pattern, %ux%u per eye\n", opt.eyeWidth, opt.eyeHeight);
  X11Window window;
  const bool haveWindow =
      opt.window && std::getenv("DISPLAY") &&
      window.create(opt.eyeWidth, opt.eyeHeight,
                    {"Frame-3D-Lib test pattern",
                     "The 3D test pattern should cover this screen. Move or resize it with Steam's controls.",
                     "If you can read this, the pattern is not covering it.",
                     "Output: " + (logPath.empty() ? std::string("terminal") : logPath)});

  if (!frame3d_runtime_available()) {
    std::printf("[test] SteamVR's libopenvr_api.so is not available\n");
    return 1;
  }
  frame3d_config cfg;
  frame3d_config_init(&cfg);
  cfg.eye_width = opt.eyeWidth;
  cfg.eye_height = opt.eyeHeight;
  cfg.cpu_writable = true;
  cfg.screen_key = opt.screenKey.empty() ? nullptr : opt.screenKey.c_str();
  cfg.log = log_to_stdout;
  cfg.log_level = opt.verbose ? FRAME3D_LOG_DEBUG : FRAME3D_LOG_INFO;
  frame3d_context* ctx = frame3d_create(&cfg);
  if (!ctx) {
    std::printf("[test] frame3d_create failed\n");
    window.destroy();
    return 1;
  }

  frame3d_buffer_info info{};
  frame3d_get_buffer_info(ctx, 0, &info);
  Canvas canvas;
  canvas.width = info.width;
  canvas.height = info.height;
  canvas.bgra = info.drm_format == FRAME3D_FORMAT_BGRA8;
  canvas.px.assign(size_t(canvas.width) * canvas.height, 0);
  const Layout layout = make_layout(opt.eyeWidth, opt.eyeHeight);
  draw_pattern(canvas, layout);
  const std::vector<uint32_t> barBand(canvas.px.begin() + size_t(layout.barY) * canvas.width,
                                      canvas.px.begin() + size_t(layout.barY + layout.barH) * canvas.width);
  std::vector<bool> initialized(frame3d_buffer_count(ctx), false);

  using clock = std::chrono::steady_clock;
  const auto start = clock::now();
  const auto step = std::chrono::duration_cast<clock::duration>(std::chrono::duration<double>(1.0 / opt.fps));
  auto next = start, lastStats = start;
  uint32_t submits = 0, skipped = 0;
  bool submitted = false;
  while (!g_quit) {
    const auto now = clock::now();
    const double elapsed = std::chrono::duration<double>(now - start).count();
    if (opt.seconds > 0 && elapsed >= opt.seconds)
      break;
    if (haveWindow && !window.pump())
      break;
    frame3d_status status{};
    frame3d_get_status(ctx, &status);
    if (status.quit_requested) {
      std::printf("[test] SteamVR asked us to quit\n");
      break;
    }

    if (opt.animate || !submitted) {
      const int i = frame3d_acquire(ctx);
      if (i < 0) {
        ++skipped;
      } else {
        uint32_t stride = 0;
        if (auto* dst = static_cast<uint8_t*>(frame3d_map(ctx, i, &stride))) {
          // A fresh image gets the whole pattern; after that only the bar's band changes.
          const uint32_t y0 = initialized[size_t(i)] ? uint32_t(layout.barY) : 0u;
          const uint32_t rows = initialized[size_t(i)] ? uint32_t(layout.barH) : canvas.height;
          if (opt.animate) {
            std::copy(barBand.begin(), barBand.end(), canvas.px.begin() + size_t(layout.barY) * canvas.width);
            draw_bar(canvas, layout, elapsed);
          }
          for (uint32_t row = 0; row < rows; ++row)
            std::memcpy(dst + size_t(y0 + row) * stride, canvas.px.data() + size_t(y0 + row) * canvas.width,
                        size_t(canvas.width) * 4);
          frame3d_unmap(ctx, i);
          initialized[size_t(i)] = true;
          frame3d_submit(ctx, i, -1);
          ++submits;
          submitted = true;
        } else {
          frame3d_release(ctx, i);
        }
      }
    }

    if (now - lastStats >= std::chrono::seconds(5)) {
      const double secs = std::chrono::duration<double>(now - lastStats).count();
      std::printf("[test] %.1f frames/s (%u skipped); %s, %.2f x %.2f m, curvature %.3f, %s\n", submits / secs,
                  skipped, status.placement == FRAME3D_PLACEMENT_FOLLOWING ? "following" : "fixed",
                  status.width_m, status.height_m, status.curvature, status.visible ? "visible" : "hidden");
      lastStats = now;
      submits = skipped = 0;
    }

    next += step;
    if (next < clock::now())
      next = clock::now();
    std::this_thread::sleep_until(next);
  }

  frame3d_destroy(ctx);
  window.destroy();
  std::printf("[test] done\n");
  return 0;
}
