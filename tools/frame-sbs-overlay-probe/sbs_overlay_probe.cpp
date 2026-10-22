// Steam Frame full-resolution side-by-side overlay probe.
//
// Standalone feasibility test for a Dusklight stereo output that hands SteamVR a
// full-SbS image (2W x H) on an overlay of its own, instead of letting Gamescope's
// game screen be split in half. It checks, in order:
//
//   1. Vulkan: the driver extensions Dawn needs to import a dma-buf and export a
//      sync-fd fence (how Aurora would render into the shared buffers).
//   2. OpenVR: connecting as an overlay app, and which dma-buf formats/modifiers
//      SteamVR accepts.
//   3. GBM -> IVRIPCResourceManagerClient::ImportDmabuf -> SetOverlayTexture with
//      VROverlayFlags_SideBySide_Parallel. This is the path Gamescope uses for the
//      game screen (OpenVRBackend.cpp, COpenVRFb::Import), done from a normal app.
//   4. Covering Gamescope's game screen: copying its transform, width and curvature
//      and drawing above it.
//
// Only openvr.h is needed to build. libopenvr_api.so, libgbm and libvulkan are
// loaded at runtime, so nothing here links against SteamVR or Mesa.
//
// Build with build.sh (on the Frame, or via the frame-probe GitHub workflow), then
// run ./sbs-overlay-probe --help.

#include "openvr.h"

#include <dlfcn.h>
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <future>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

namespace {

constexpr double kPi = 3.14159265358979323846;

std::atomic<bool> g_quit{false};
void on_signal(int) { g_quit = true; }

void section(const char* title) { std::printf("\n== %s\n", title); }
#define OK(...) (std::printf("  [ok] "), std::printf(__VA_ARGS__), std::printf("\n"))
#define BAD(...) (std::printf("  [!!] "), std::printf(__VA_ARGS__), std::printf("\n"))
#define INFO(...) (std::printf("       "), std::printf(__VA_ARGS__), std::printf("\n"))

// ------------------------------------------------------------------ options

struct Options {
  uint32_t eyeWidth = 1920;
  uint32_t eyeHeight = 1080;
  std::string cover;      // overlay key to draw over; empty = auto-detect the game screen
  bool front = false;     // skip the game screen, place in front of the HMD instead
  float distance = 2.0f;  // front placement only
  float widthM = 1.6f;    // front placement only
  float texelAspect = 1.0f;
  bool swapEyes = false;
  bool linear = true;  // false: let GBM choose from SteamVR's modifier list
  bool animate = true;
  bool window = true;  // open a blank window (when DISPLAY is set) so Steam gives us a screen
  double wait = -1.0;  // seconds to keep looking for the game screen; <0 = 30 with a window, else 0
  std::string logPath;  // empty = ~/sbs-overlay-probe.log when stdout is not a terminal
  double fps = 60.0;
  double seconds = 0.0;  // 0 = until Ctrl+C
  std::string openvrLib = "/opt/steamvr/bin/linuxarm64/libopenvr_api.so";
  std::string vrcmd = "/opt/steamvr/bin/linuxarm64/vrcmd";
  std::string drmNode;  // empty = first /dev/dri/renderD*
};

void usage() {
  std::printf(
      "Usage: sbs-overlay-probe [options]\n"
      "  --eye WxH            per-eye resolution (default 1920x1080; texture is 2W x H)\n"
      "  --cover KEY          draw over this overlay (default: auto-detect the game screen)\n"
      "  --front              ignore the game screen; place 2 m in front of the headset\n"
      "  --distance M         front placement distance (default 2.0)\n"
      "  --width M            front placement width in metres (default 1.6)\n"
      "  --texel-aspect A     overlay texel aspect (default 1.0, correct for full SbS)\n"
      "  --swap-eyes          use SideBySide_Crossed instead of SideBySide_Parallel\n"
      "  --native-modifier    allocate with SteamVR's modifier list instead of LINEAR\n"
      "  --static             submit one frame only (no moving bar)\n"
      "  --no-window          don't open a window (cover another app's screen instead)\n"
      "  --wait N             keep looking for the game screen for N seconds\n"
      "                       (default 30 with a window, else 0)\n"
      "  --log PATH           write output here (default ~/sbs-overlay-probe.log when\n"
      "                       there is no terminal, e.g. launched from Steam)\n"
      "  --fps N              animation submit rate (default 60)\n"
      "  --seconds N          exit after N seconds (default: run until Ctrl+C)\n"
      "  --openvr-lib PATH    libopenvr_api.so to load\n"
      "  --vrcmd PATH         vrcmd used to find the game screen\n"
      "  --drm-node PATH      DRM render node for GBM (default: first /dev/dri/renderD*)\n");
}

bool parse_args(int argc, char** argv, Options& o) {
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto next = [&](const char* name) -> const char* {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "%s needs a value\n", name);
        std::exit(2);
      }
      return argv[++i];
    };
    if (a == "--help" || a == "-h") {
      usage();
      std::exit(0);
    } else if (a == "--eye") {
      unsigned w = 0, h = 0;
      if (std::sscanf(next("--eye"), "%ux%u", &w, &h) != 2 || w == 0 || h == 0) {
        std::fprintf(stderr, "--eye expects WxH\n");
        return false;
      }
      o.eyeWidth = w;
      o.eyeHeight = h;
    } else if (a == "--cover") {
      o.cover = next("--cover");
    } else if (a == "--front") {
      o.front = true;
    } else if (a == "--distance") {
      o.distance = std::strtof(next("--distance"), nullptr);
    } else if (a == "--width") {
      o.widthM = std::strtof(next("--width"), nullptr);
    } else if (a == "--texel-aspect") {
      o.texelAspect = std::strtof(next("--texel-aspect"), nullptr);
    } else if (a == "--swap-eyes") {
      o.swapEyes = true;
    } else if (a == "--native-modifier") {
      o.linear = false;
    } else if (a == "--static") {
      o.animate = false;
    } else if (a == "--no-window") {
      o.window = false;
    } else if (a == "--wait") {
      o.wait = std::strtod(next("--wait"), nullptr);
    } else if (a == "--log") {
      o.logPath = next("--log");
    } else if (a == "--fps") {
      o.fps = std::max(1.0, std::strtod(next("--fps"), nullptr));
    } else if (a == "--seconds") {
      o.seconds = std::strtod(next("--seconds"), nullptr);
    } else if (a == "--openvr-lib") {
      o.openvrLib = next("--openvr-lib");
    } else if (a == "--vrcmd") {
      o.vrcmd = next("--vrcmd");
    } else if (a == "--drm-node") {
      o.drmNode = next("--drm-node");
    } else {
      std::fprintf(stderr, "Unknown option %s\n", a.c_str());
      usage();
      return false;
    }
  }
  return true;
}

// ------------------------------------------------------------- DRM formats

constexpr uint32_t fourcc(char a, char b, char c, char d) {
  return uint32_t(uint8_t(a)) | (uint32_t(uint8_t(b)) << 8) | (uint32_t(uint8_t(c)) << 16) |
         (uint32_t(uint8_t(d)) << 24);
}
constexpr uint32_t DRM_FORMAT_ABGR8888 = fourcc('A', 'B', '2', '4');  // bytes R,G,B,A = RGBA8Unorm
constexpr uint32_t DRM_FORMAT_XBGR8888 = fourcc('X', 'B', '2', '4');
constexpr uint32_t DRM_FORMAT_ARGB8888 = fourcc('A', 'R', '2', '4');  // bytes B,G,R,A = BGRA8Unorm
constexpr uint32_t DRM_FORMAT_XRGB8888 = fourcc('X', 'R', '2', '4');
constexpr uint64_t DRM_FORMAT_MOD_LINEAR = 0;
constexpr uint64_t DRM_FORMAT_MOD_INVALID = 0x00ffffffffffffffull;

std::string fourcc_str(uint32_t f) {
  char s[5] = {char(f & 0xff), char((f >> 8) & 0xff), char((f >> 16) & 0xff), char((f >> 24) & 0xff), 0};
  for (int i = 0; i < 4; ++i) {
    if (s[i] < 32 || s[i] > 126)
      s[i] = '?';
  }
  return s;
}

std::string modifier_str(uint64_t m) {
  char buf[64];
  if (m == DRM_FORMAT_MOD_LINEAR)
    return "LINEAR";
  if (m == DRM_FORMAT_MOD_INVALID)
    return "INVALID";
  std::snprintf(buf, sizeof buf, "0x%016llx", static_cast<unsigned long long>(m));
  return buf;
}

// --------------------------------------------------------- Vulkan (probe only)
//
// Just enough of the Vulkan ABI to list device extensions, so the build needs no
// Vulkan headers. Dawn's SharedTextureMemoryDmaBuf / SharedFenceSyncFD features sit
// on these extensions; Aurora would request those features to render into the
// buffers SteamVR displays.

struct VkExtensionPropertiesT {
  char extensionName[256];
  uint32_t specVersion;
};
struct VkInstanceCreateInfoT {
  uint32_t sType;  // VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO = 1
  const void* pNext;
  uint32_t flags;
  const void* pApplicationInfo;
  uint32_t enabledLayerCount;
  const char* const* ppEnabledLayerNames;
  uint32_t enabledExtensionCount;
  const char* const* ppEnabledExtensionNames;
};

void check_vulkan() {
  section("Vulkan driver (extensions behind Dawn's dma-buf import)");
  void* lib = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
  if (!lib) {
    BAD("libvulkan.so.1 not loadable: %s", dlerror());
    return;
  }
  using PFN_Void = void (*)();
  using PFN_GetInstanceProcAddr = PFN_Void (*)(void*, const char*);
  using PFN_CreateInstance = int32_t (*)(const VkInstanceCreateInfoT*, const void*, void**);
  using PFN_EnumeratePhysicalDevices = int32_t (*)(void*, uint32_t*, void**);
  using PFN_EnumerateDeviceExtensions = int32_t (*)(void*, const char*, uint32_t*, VkExtensionPropertiesT*);
  using PFN_GetPhysicalDeviceProperties = void (*)(void*, void*);
  using PFN_DestroyInstance = void (*)(void*, const void*);

  const auto gipa = reinterpret_cast<PFN_GetInstanceProcAddr>(dlsym(lib, "vkGetInstanceProcAddr"));
  const auto createInstance =
      gipa ? reinterpret_cast<PFN_CreateInstance>(gipa(nullptr, "vkCreateInstance")) : nullptr;
  void* instance = nullptr;
  VkInstanceCreateInfoT ci{};
  ci.sType = 1;
  if (!createInstance || createInstance(&ci, nullptr, &instance) != 0 || !instance) {
    BAD("vkCreateInstance failed");
    dlclose(lib);
    return;
  }
  const auto enumPhys =
      reinterpret_cast<PFN_EnumeratePhysicalDevices>(gipa(instance, "vkEnumeratePhysicalDevices"));
  const auto enumExts =
      reinterpret_cast<PFN_EnumerateDeviceExtensions>(gipa(instance, "vkEnumerateDeviceExtensionProperties"));
  const auto getProps =
      reinterpret_cast<PFN_GetPhysicalDeviceProperties>(gipa(instance, "vkGetPhysicalDeviceProperties"));
  const auto destroyInstance = reinterpret_cast<PFN_DestroyInstance>(gipa(instance, "vkDestroyInstance"));

  uint32_t count = 0;
  enumPhys(instance, &count, nullptr);
  std::vector<void*> devices(count);
  enumPhys(instance, &count, devices.data());
  if (count == 0)
    BAD("no Vulkan physical devices");

  struct Wanted {
    const char* name;
    const char* why;
  };
  const Wanted wanted[] = {
      {"VK_EXT_external_memory_dma_buf", "import a dma-buf"},
      {"VK_EXT_image_drm_format_modifier", "import tiled/compressed layouts"},
      {"VK_KHR_external_memory_fd", "fd-based external memory"},
      {"VK_KHR_external_semaphore_fd", "export a sync-fd fence when rendering ends"},
      {"VK_EXT_queue_family_foreign", "hand the image to another process"},
  };

  for (void* dev : devices) {
    // deviceName follows apiVersion, driverVersion, vendorID, deviceID, deviceType.
    alignas(16) unsigned char props[2048] = {};
    getProps(dev, props);
    std::printf("  device: %.255s\n", reinterpret_cast<const char*>(props + 20));

    uint32_t extCount = 0;
    enumExts(dev, nullptr, &extCount, nullptr);
    std::vector<VkExtensionPropertiesT> exts(extCount);
    enumExts(dev, nullptr, &extCount, exts.data());
    for (const Wanted& w : wanted) {
      const bool has = std::any_of(exts.begin(), exts.end(), [&](const VkExtensionPropertiesT& e) {
        return std::strcmp(e.extensionName, w.name) == 0;
      });
      if (has)
        OK("%-34s (%s)", w.name, w.why);
      else
        BAD("%-34s missing (%s)", w.name, w.why);
    }
  }
  destroyInstance(instance, nullptr);
  dlclose(lib);
}

// --------------------------------------------------------------------- GBM

struct gbm_device;
struct gbm_bo;
constexpr uint32_t GBM_BO_USE_RENDERING = 1u << 2;
constexpr uint32_t GBM_BO_USE_LINEAR = 1u << 4;
constexpr uint32_t GBM_BO_TRANSFER_WRITE = 1u << 1;

struct Gbm {
  void* lib = nullptr;
  gbm_device* (*create_device)(int) = nullptr;
  void (*device_destroy)(gbm_device*) = nullptr;
  gbm_bo* (*bo_create)(gbm_device*, uint32_t, uint32_t, uint32_t, uint32_t) = nullptr;
  gbm_bo* (*bo_create_with_modifiers)(gbm_device*, uint32_t, uint32_t, uint32_t, const uint64_t*,
                                      unsigned int) = nullptr;
  void (*bo_destroy)(gbm_bo*) = nullptr;
  int (*bo_get_fd)(gbm_bo*) = nullptr;
  int (*bo_get_fd_for_plane)(gbm_bo*, int) = nullptr;
  uint32_t (*bo_get_stride_for_plane)(gbm_bo*, int) = nullptr;
  uint32_t (*bo_get_offset)(gbm_bo*, int) = nullptr;
  uint64_t (*bo_get_modifier)(gbm_bo*) = nullptr;
  int (*bo_get_plane_count)(gbm_bo*) = nullptr;
  void* (*bo_map)(gbm_bo*, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t*, void**) = nullptr;
  void (*bo_unmap)(gbm_bo*, void*) = nullptr;

  bool load() {
    lib = dlopen("libgbm.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!lib)
      lib = dlopen("libgbm.so", RTLD_NOW | RTLD_LOCAL);
    if (!lib) {
      BAD("libgbm not loadable: %s", dlerror());
      return false;
    }
    const auto sym = [&](auto& fn, const char* name) {
      fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(dlsym(lib, name));
    };
    sym(create_device, "gbm_create_device");
    sym(device_destroy, "gbm_device_destroy");
    sym(bo_create, "gbm_bo_create");
    sym(bo_create_with_modifiers, "gbm_bo_create_with_modifiers");
    sym(bo_destroy, "gbm_bo_destroy");
    sym(bo_get_fd, "gbm_bo_get_fd");
    sym(bo_get_fd_for_plane, "gbm_bo_get_fd_for_plane");
    sym(bo_get_stride_for_plane, "gbm_bo_get_stride_for_plane");
    sym(bo_get_offset, "gbm_bo_get_offset");
    sym(bo_get_modifier, "gbm_bo_get_modifier");
    sym(bo_get_plane_count, "gbm_bo_get_plane_count");
    sym(bo_map, "gbm_bo_map");
    sym(bo_unmap, "gbm_bo_unmap");
    if (!create_device || !device_destroy || !bo_create || !bo_destroy || !bo_get_fd ||
        !bo_get_stride_for_plane || !bo_get_offset || !bo_get_modifier || !bo_get_plane_count || !bo_map ||
        !bo_unmap) {
      BAD("libgbm is missing required entry points");
      return false;
    }
    return true;
  }
};

// --------------------------------------------------------------- X11 window
//
// A blank window so Gamescope gives this process a game screen of its own. Steam's
// move and resize controls belong to that screen, and covering it is what makes the
// 3D image movable. Gamescope finds the owning app from the X client's pid (XRes)
// and Steam's launcher above it, so a plain Xlib window launched from Steam is enough.

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
      BAD("libX11.so.6 not loadable: %s", dlerror());
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
      BAD("libX11 is missing required entry points");
      return false;
    }
    dpy = OpenDisplay(nullptr);
    if (!dpy) {
      BAD("XOpenDisplay(%s) failed", std::getenv("DISPLAY"));
      return false;
    }
    const int screen = DefaultScreen(dpy);
    win = CreateSimpleWindow(dpy, RootWindow(dpy, screen), 0, 0, width, height, 0, BlackPixel(dpy, screen),
                             BlackPixel(dpy, screen));
    StoreName(dpy, win, "Dusklight SbS probe");
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

// ------------------------------------------------------------------ OpenVR

struct OpenVRApi {
  void* lib = nullptr;
  uint32_t (*InitInternal2)(vr::EVRInitError*, vr::EVRApplicationType, const char*) = nullptr;
  void (*ShutdownInternal)() = nullptr;
  void* (*GetGenericInterface)(const char*, vr::EVRInitError*) = nullptr;
  const char* (*InitErrorSymbol)(vr::EVRInitError) = nullptr;
  const char* (*InitErrorText)(vr::EVRInitError) = nullptr;

  bool load(const std::string& path) {
    lib = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!lib)
      lib = dlopen("libopenvr_api.so", RTLD_NOW | RTLD_LOCAL);
    if (!lib) {
      BAD("libopenvr_api.so not loadable: %s", dlerror());
      return false;
    }
    InitInternal2 = reinterpret_cast<decltype(InitInternal2)>(dlsym(lib, "VR_InitInternal2"));
    ShutdownInternal = reinterpret_cast<decltype(ShutdownInternal)>(dlsym(lib, "VR_ShutdownInternal"));
    GetGenericInterface = reinterpret_cast<decltype(GetGenericInterface)>(dlsym(lib, "VR_GetGenericInterface"));
    InitErrorSymbol = reinterpret_cast<decltype(InitErrorSymbol)>(dlsym(lib, "VR_GetVRInitErrorAsSymbol"));
    InitErrorText =
        reinterpret_cast<decltype(InitErrorText)>(dlsym(lib, "VR_GetVRInitErrorAsEnglishDescription"));
    if (!InitInternal2 || !ShutdownInternal || !GetGenericInterface) {
      BAD("libopenvr_api.so is missing VR_InitInternal2 / VR_GetGenericInterface");
      return false;
    }
    return true;
  }

  std::string describe(vr::EVRInitError e) const {
    std::string s = InitErrorSymbol ? InitErrorSymbol(e) : std::to_string(int(e));
    if (InitErrorText)
      s += std::string(" (") + InitErrorText(e) + ")";
    return s;
  }
};

vr::IVROverlay* g_overlay = nullptr;
const char* ov_err(vr::EVROverlayError e) { return g_overlay->GetOverlayErrorNameFromEnum(e); }

const char* transform_type_name(vr::VROverlayTransformType t) {
  switch (t) {
  case vr::VROverlayTransform_Absolute:
    return "Absolute";
  case vr::VROverlayTransform_TrackedDeviceRelative:
    return "TrackedDeviceRelative";
  case vr::VROverlayTransform_TrackedComponent:
    return "TrackedComponent";
  case vr::VROverlayTransform_Cursor:
    return "Cursor";
  case vr::VROverlayTransform_DashboardTab:
    return "DashboardTab";
  case vr::VROverlayTransform_DashboardThumb:
    return "DashboardThumb";
  case vr::VROverlayTransform_Mountable:
    return "Mountable";
  case vr::VROverlayTransform_Projection:
    return "Projection";
  case vr::VROverlayTransform_Subview:
    return "Subview";
  default:
    return "Invalid/unknown";
  }
}

// Same discovery as frame-sbs-toggle: game screens are listed by `vrcmd --overlays`
// as 'valve.steam.desktopgame[.<appid>]' -- ... visible ...
struct Listed {
  std::string key;
  bool visible;
};

std::vector<Listed> list_game_screens(const std::string& vrcmd) {
  std::vector<Listed> out;
  // Steam's LD_PRELOAD (overlay renderer) only makes the shell print loader errors.
  unsetenv("LD_PRELOAD");
  const std::string cmd = "'" + vrcmd + "' --overlays 2>/dev/null";
  FILE* p = popen(cmd.c_str(), "r");
  if (!p)
    return out;
  char buf[4096];
  while (std::fgets(buf, sizeof buf, p)) {
    std::string line = buf;
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r'))
      line.pop_back();
    if (line.empty() || line[0] != '\'')
      continue;
    const size_t end = line.find('\'', 1);
    if (end == std::string::npos || line.compare(end, 5, "' -- ") != 0)
      continue;
    const std::string key = line.substr(1, end - 1);
    const std::string prefix = "valve.steam.desktopgame";
    if (key.compare(0, prefix.size(), prefix) != 0)
      continue;
    const std::string rest = key.substr(prefix.size());
    // Only the screen itself (optionally .<digits>), not Gamescope's .layerN subviews.
    if (!rest.empty() &&
        (rest[0] != '.' || rest.size() == 1 ||
         !std::all_of(rest.begin() + 1, rest.end(), [](char c) { return c >= '0' && c <= '9'; })))
      continue;
    out.push_back({key, (line + " ").find(" visible ") != std::string::npos});
  }
  pclose(p);
  return out;
}

std::string choose_game_screen(const std::vector<Listed>& screens) {
  std::vector<std::string> visible, hidden;
  for (const Listed& s : screens) {
    if (s.visible)
      visible.push_back(s.key);
    else if (s.key != "valve.steam.desktopgame.0")
      hidden.push_back(s.key);
  }
  if (visible.size() == 1)
    return visible[0];
  if (visible.empty() && hidden.size() == 1)
    return hidden[0];
  return {};
}

// The Steam app ID Gamescope files our screen under (valve.steam.desktopgame.<id>):
// from our cgroup scope, app-steam-app<id>-<reaper pid>.scope, which is the lookup
// Gamescope does, else Steam's launch environment. 0 when not launched from Steam.
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

struct Vec3 {
  float x, y, z;
};
Vec3 operator+(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
Vec3 operator-(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Vec3 operator*(Vec3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
float dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
Vec3 cross(Vec3 a, Vec3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
float length(Vec3 a) { return std::sqrt(dot(a, a)); }

// Placement copied from another overlay, re-read every frame so ours follows it.
struct SourcePlacement {
  vr::VROverlayTransformType type = vr::VROverlayTransform_Invalid;
  bool sampled = false;  // rebuilt from points on the overlay (dashboard-placed screens)
  vr::ETrackingUniverseOrigin origin = vr::TrackingUniverseStanding;
  vr::TrackedDeviceIndex_t device = 0;
  vr::HmdMatrix34_t matrix{};
  float widthM = 0.f;
  float heightM = 0.f;  // sampled only
  float curvature = 0.f;
  float widthSetting = 0.f;  // the source's own SetOverlayWidthInMeters value
  vr::HmdVector2_t mouseScale{};  // sampled only
  // Diagnostics for sampled placements: the scale in the matrix SteamVR returns for the
  // center point, and its Z axis against ours (+1 same, -1 opposite).
  float matrixScale = 0.f;
  float facing = 0.f;

  bool same_as(const SourcePlacement& o) const {
    return type == o.type && sampled == o.sampled && origin == o.origin && device == o.device &&
           std::memcmp(&matrix, &o.matrix, sizeof matrix) == 0 && widthM == o.widthM &&
           curvature == o.curvature;
  }
};

// Steam's game screen on the Frame is a DashboardTab overlay: the dashboard places it
// and there is no getter for that transform. GetTransformForOverlayCoordinates still
// reports where any point of it is in the world, so rebuild the pose from its center,
// left/right edges and top edge. Overlay coordinates are in mouse-scale units, 0,0 at
// the lower left.
bool sample_placement(vr::VROverlayHandle_t src, SourcePlacement& p, std::string& why) {
  vr::HmdVector2_t scale{};
  if (g_overlay->GetOverlayMouseScale(src, &scale) != vr::VROverlayError_None || scale.v[0] <= 0.f ||
      scale.v[1] <= 0.f)
    scale.v[0] = scale.v[1] = 1.f;
  const float w = scale.v[0], h = scale.v[1];
  vr::HmdMatrix34_t centerMatrix{};
  const auto at = [&](float u, float v, Vec3& out, vr::HmdMatrix34_t* keep = nullptr) {
    vr::HmdMatrix34_t m{};
    const vr::EVROverlayError e =
        g_overlay->GetTransformForOverlayCoordinates(src, vr::TrackingUniverseStanding, vr::HmdVector2_t{{u, v}}, &m);
    if (e != vr::VROverlayError_None) {
      why = std::string("GetTransformForOverlayCoordinates: ") + ov_err(e);
      return false;
    }
    out = {m.m[0][3], m.m[1][3], m.m[2][3]};
    if (keep)
      *keep = m;
    return true;
  };
  Vec3 c{}, l{}, r{}, t{};
  if (!at(w * 0.5f, h * 0.5f, c, &centerMatrix) || !at(0.f, h * 0.5f, l) || !at(w, h * 0.5f, r) ||
      !at(w * 0.5f, h, t))
    return false;

  const float chord = length(r - l);
  if (chord < 1e-4f) {
    why = "game screen reports zero width (all sampled points coincide)";
    return false;
  }
  const Vec3 x = (r - l) * (1.f / chord);
  Vec3 up = t - c;
  up = up - x * dot(up, x);
  const float halfHeight = length(up);
  if (halfHeight < 1e-5f) {
    why = "game screen reports zero height";
    return false;
  }
  const Vec3 y = up * (1.f / halfHeight);
  const Vec3 z = cross(x, y);  // toward the viewer: the overlay's front

  float curvature = 0.f;
  g_overlay->GetOverlayCurvature(src, &curvature);
  // A curved overlay wraps a cylinder whose circumference is width / curvature, so the
  // edge-to-edge chord is shorter than the width.
  const float a = float(kPi) * curvature;
  const float widthM = a > 1e-4f ? chord * a / std::sin(a) : chord;

  // 1 cm toward the viewer so we also win the back-to-front ordering.
  const Vec3 pos = c + z * 0.01f;
  const Vec3 cols[4] = {x, y, z, pos};
  for (int col = 0; col < 4; ++col) {
    p.matrix.m[0][col] = cols[col].x;
    p.matrix.m[1][col] = cols[col].y;
    p.matrix.m[2][col] = cols[col].z;
  }
  p.sampled = true;
  p.origin = vr::TrackingUniverseStanding;
  p.widthM = widthM;
  p.heightM = 2.f * halfHeight;
  p.curvature = curvature;
  p.mouseScale = scale;
  g_overlay->GetOverlayWidthInMeters(src, &p.widthSetting);
  const auto& cm = centerMatrix.m;
  p.matrixScale = length(Vec3{cm[0][0], cm[1][0], cm[2][0]});
  const Vec3 cz{cm[0][2], cm[1][2], cm[2][2]};
  const float czLen = length(cz);
  p.facing = czLen > 1e-6f ? dot(cz, z) / czLen : 0.f;
  return true;
}

bool read_placement(vr::VROverlayHandle_t src, SourcePlacement& p, std::string& why) {
  vr::EVROverlayError e = g_overlay->GetOverlayTransformType(src, &p.type);
  if (e != vr::VROverlayError_None) {
    why = std::string("GetOverlayTransformType: ") + ov_err(e);
    return false;
  }
  if (p.type == vr::VROverlayTransform_Absolute)
    e = g_overlay->GetOverlayTransformAbsolute(src, &p.origin, &p.matrix);
  else if (p.type == vr::VROverlayTransform_TrackedDeviceRelative)
    e = g_overlay->GetOverlayTransformTrackedDeviceRelative(src, &p.device, &p.matrix);
  else
    return sample_placement(src, p, why);
  if (e != vr::VROverlayError_None) {
    why = std::string("reading the game screen transform: ") + ov_err(e);
    return false;
  }
  p.sampled = false;
  g_overlay->GetOverlayWidthInMeters(src, &p.widthM);
  g_overlay->GetOverlayCurvature(src, &p.curvature);
  p.widthSetting = p.widthM;
  return true;
}

void apply_placement(vr::VROverlayHandle_t dst, const SourcePlacement& p) {
  if (p.sampled || p.type == vr::VROverlayTransform_Absolute)
    g_overlay->SetOverlayTransformAbsolute(dst, p.origin, &p.matrix);
  else
    g_overlay->SetOverlayTransformTrackedDeviceRelative(dst, p.device, &p.matrix);
  if (p.widthM > 0.f)
    g_overlay->SetOverlayWidthInMeters(dst, p.widthM);
  g_overlay->SetOverlayCurvature(dst, p.curvature);
}

// Level, yaw-only placement `distance` metres ahead of the headset, facing it.
bool place_in_front(vr::IVRSystem* sys, vr::VROverlayHandle_t dst, float distance, float widthM) {
  g_overlay->SetOverlayWidthInMeters(dst, widthM);
  vr::TrackedDevicePose_t pose{};
  if (sys)
    sys->GetDeviceToAbsoluteTrackingPose(vr::TrackingUniverseStanding, 0.f, &pose, 1);
  vr::HmdMatrix34_t m{};
  if (!pose.bPoseIsValid) {
    // No pose yet: lock it to the head instead.
    m.m[0][0] = m.m[1][1] = m.m[2][2] = 1.f;
    m.m[2][3] = -distance;
    return g_overlay->SetOverlayTransformTrackedDeviceRelative(dst, vr::k_unTrackedDeviceIndex_Hmd, &m) ==
           vr::VROverlayError_None;
  }
  const auto& h = pose.mDeviceToAbsoluteTracking.m;
  const float yaw = std::atan2(h[0][2], h[2][2]);
  const float c = std::cos(yaw), s = std::sin(yaw);
  m.m[0][0] = c;
  m.m[0][2] = s;
  m.m[0][3] = h[0][3] - s * distance;
  m.m[1][1] = 1.f;
  m.m[1][3] = h[1][3];
  m.m[2][0] = -s;
  m.m[2][2] = c;
  m.m[2][3] = h[2][3] - c * distance;
  return g_overlay->SetOverlayTransformAbsolute(dst, vr::TrackingUniverseStanding, &m) == vr::VROverlayError_None;
}

// ------------------------------------------------------------ test pattern

struct Canvas {
  uint32_t width = 0, height = 0, format = DRM_FORMAT_ABGR8888;
  std::vector<uint32_t> px;

  uint32_t rgb(uint8_t r, uint8_t g, uint8_t b) const {
    if (format == DRM_FORMAT_ARGB8888 || format == DRM_FORMAT_XRGB8888)
      return 0xff000000u | (uint32_t(r) << 16) | (uint32_t(g) << 8) | b;
    return 0xff000000u | (uint32_t(b) << 16) | (uint32_t(g) << 8) | r;
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

// Both eyes share one layout; only the eye tag and the per-object disparity differ.
// eye 0 is the left half of the texture.
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

    // Eye tag: red top-left in the left eye, blue top-right in the right eye.
    const int tag = int(160 * s), inset = int(48 * s);
    if (eye == 0)
      c.fill(ox + inset, inset, tag, tag, c.rgb(230, 30, 30));
    else
      c.fill(ox + l.eyeW - inset - tag, inset, tag, tag, c.rgb(30, 80, 255));

    // Depth row: crossed disparity (in front), none (on the screen), uncrossed (behind).
    const int q = int(200 * s);
    const int qy = int(l.eyeH * 0.40) - q / 2;
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
      const int shift = (eye == 0 ? 1 : -1) * sq.sign * l.disparity;
      const int x = ox + int(l.eyeW * sq.cx) - q / 2 + shift;
      c.fill(x - border, qy - border, q + 2 * border, q + 2 * border, white);
      c.fill(x, qy, q, q, sq.color);
    }

    // Track for the moving bar.
    c.fill(ox + int(l.eyeW * 0.1), l.barY + l.barH / 2, int(l.eyeW * 0.8), std::max(1, int(2 * s)),
           c.rgb(90, 90, 90));

    // Resolution gratings, identical in both eyes: for 1, 2 and 3 px pitch, vertical
    // lines on the left and horizontal lines on the right. Halved horizontal
    // resolution blurs the vertical ones but leaves the horizontal ones crisp.
    const int g = int(150 * s), gap = int(40 * s);
    const int total = 3 * (2 * g + gap) - gap;
    const int gy = int(l.eyeH * 0.70);
    int gx = ox + (l.eyeW - total) / 2;
    const auto plot = [&](int x, int y, uint32_t color) {
      if (x >= ox && x < ox + l.eyeW && y >= 0 && y < l.eyeH)
        c.px[size_t(y) * c.width + x] = color;
    };
    for (int pitch = 1; pitch <= 3; ++pitch) {
      for (int yy = 0; yy < g; ++yy) {
        for (int xx = 0; xx < g; ++xx) {
          const bool v = (xx / pitch) % 2 == 0;
          const bool h = (yy / pitch) % 2 == 0;
          plot(gx + xx, gy + yy, v ? white : black);
          plot(gx + g + xx, gy + yy, h ? white : black);
        }
      }
      gx += 2 * g + gap;
    }

    // 16-step gray ramp (gamma check).
    const int rampY = gy + g + int(30 * s), rampH = int(36 * s);
    const int rampX = ox + int(l.eyeW * 0.1), rampW = int(l.eyeW * 0.8);
    for (int i = 0; i < 16; ++i) {
      const uint8_t v = uint8_t(i * 17);
      c.fill(rampX + rampW * i / 16, rampY, rampW / 16 + 1, rampH, c.rgb(v, v, v));
    }
  }
}

void draw_bar(Canvas& c, const Layout& l, double t) {
  const double phase = 0.5 + 0.5 * std::sin(t * 2.0 * kPi / 4.0);
  const int travel = int(l.eyeW * 0.8) - l.barW;
  const int bx = int(l.eyeW * 0.1) + int(phase * travel);
  for (int eye = 0; eye < 2; ++eye) {
    const int shift = eye == 0 ? l.disparity : -l.disparity;  // in front of the screen
    c.fill(eye * l.eyeW + bx + shift, l.barY, l.barW, l.barH, c.rgb(230, 40, 220));
  }
}

// ------------------------------------------------------------ shared buffers

struct Buffer {
  gbm_bo* bo = nullptr;
  int fds[4] = {-1, -1, -1, -1};
  vr::SharedTextureHandle_t handle = 0;
};

bool upload_rows(const Gbm& gbm, Buffer& b, const Canvas& c, uint32_t y, uint32_t h) {
  uint32_t stride = 0;
  void* mapData = nullptr;
  auto* dst = static_cast<uint8_t*>(gbm.bo_map(b.bo, 0, y, c.width, h, GBM_BO_TRANSFER_WRITE, &stride, &mapData));
  if (!dst)
    return false;
  for (uint32_t row = 0; row < h; ++row)
    std::memcpy(dst + size_t(row) * stride, c.px.data() + size_t(y + row) * c.width, size_t(c.width) * 4);
  gbm.bo_unmap(b.bo, mapData);
  return true;
}

int open_render_node(const std::string& requested, std::string& used) {
  if (!requested.empty()) {
    used = requested;
    return open(requested.c_str(), O_RDWR | O_CLOEXEC);
  }
  for (int i = 128; i < 136; ++i) {
    used = "/dev/dri/renderD" + std::to_string(i);
    const int fd = open(used.c_str(), O_RDWR | O_CLOEXEC);
    if (fd >= 0)
      return fd;
  }
  return -1;
}

}  // namespace

int main(int argc, char** argv) {
  Options opt;
  if (!parse_args(argc, argv, opt))
    return 2;
  signal(SIGINT, on_signal);
  signal(SIGTERM, on_signal);

  // Launched from Steam there is no terminal, so keep the output in a file.
  std::string logPath = opt.logPath;
  if (logPath.empty() && !isatty(STDOUT_FILENO)) {
    const char* home = std::getenv("HOME");
    logPath = std::string(home && *home ? home : "/tmp") + "/sbs-overlay-probe.log";
  }
  if (!logPath.empty()) {
    if (std::freopen(logPath.c_str(), "w", stdout))
      dup2(STDOUT_FILENO, STDERR_FILENO);
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
  }

  const uint32_t texW = opt.eyeWidth * 2, texH = opt.eyeHeight;
  std::printf("Steam Frame full-SbS overlay probe: %ux%u per eye -> %ux%u texture\n", opt.eyeWidth,
              opt.eyeHeight, texW, texH);

  X11Window window;
  bool haveWindow = false;
  if (opt.window && std::getenv("DISPLAY")) {
    section("Window");
    haveWindow = window.create(
        opt.eyeWidth, opt.eyeHeight,
        {"Dusklight SbS overlay probe",
         "The 3D test pattern should cover this screen. Move or resize it with Steam's controls.",
         "If you can read this, the pattern is not covering it.",
         "Output: " + (logPath.empty() ? std::string("terminal") : logPath)});
    if (haveWindow)
      OK("opened a %ux%u window so Steam gives this app a game screen", opt.eyeWidth, opt.eyeHeight);
  }
  const double waitSeconds = opt.front ? 0.0 : opt.wait >= 0.0 ? opt.wait : haveWindow ? 30.0 : 0.0;

  check_vulkan();

  // ---- OpenVR
  section("OpenVR");
  OpenVRApi api;
  if (!api.load(opt.openvrLib))
    return 1;
  vr::EVRInitError initErr = vr::VRInitError_None;
  api.InitInternal2(&initErr, vr::VRApplication_Overlay, nullptr);
  if (initErr != vr::VRInitError_None) {
    BAD("VR_InitInternal2(VRApplication_Overlay): %s", api.describe(initErr).c_str());
    return 1;
  }
  OK("connected as an overlay application");

  int exitCode = 0;
  std::vector<Buffer> buffers;
  Gbm gbm;
  gbm_device* gbmDevice = nullptr;
  int drmFd = -1;
  vr::VROverlayHandle_t ours = vr::k_ulOverlayHandleInvalid;
  vr::IVRIPCResourceManagerClient* ipc = nullptr;

  // Everything below runs inside this lambda so every exit path reaches the cleanup.
  [&] {
    vr::EVRInitError e = vr::VRInitError_None;
    g_overlay = static_cast<vr::IVROverlay*>(api.GetGenericInterface(vr::IVROverlay_Version, &e));
    if (!g_overlay) {
      BAD("%s: %s", vr::IVROverlay_Version, api.describe(e).c_str());
      exitCode = 1;
      return;
    }
    OK("%s", vr::IVROverlay_Version);
    auto* sys = static_cast<vr::IVRSystem*>(api.GetGenericInterface(vr::IVRSystem_Version, &e));
    if (sys)
      OK("%s", vr::IVRSystem_Version);
    else
      BAD("%s: %s (front placement falls back to head-locked)", vr::IVRSystem_Version, api.describe(e).c_str());
    ipc = static_cast<vr::IVRIPCResourceManagerClient*>(
        api.GetGenericInterface(vr::IVRIPCResourceManagerClient_Version, &e));
    if (!ipc) {
      BAD("%s: %s", vr::IVRIPCResourceManagerClient_Version, api.describe(e).c_str());
      exitCode = 1;
      return;
    }
    OK("%s", vr::IVRIPCResourceManagerClient_Version);

    // ---- formats and modifiers SteamVR accepts
    section("dma-buf formats SteamVR accepts");
    uint32_t formatCount = 0;
    std::vector<uint32_t> formats;
    if (ipc->GetDmabufFormats(&formatCount, nullptr) && formatCount > 0) {
      formats.resize(formatCount);
      ipc->GetDmabufFormats(&formatCount, formats.data());
      formats.resize(formatCount);
    }
    if (formats.empty())
      BAD("GetDmabufFormats returned nothing");
    std::string list;
    for (uint32_t f : formats)
      list += fourcc_str(f) + " ";
    INFO("formats: %s", list.c_str());

    uint32_t format = 0;
    for (uint32_t candidate : {DRM_FORMAT_ABGR8888, DRM_FORMAT_XBGR8888, DRM_FORMAT_ARGB8888, DRM_FORMAT_XRGB8888}) {
      if (std::find(formats.begin(), formats.end(), candidate) != formats.end()) {
        format = candidate;
        break;
      }
    }
    if (format == 0) {
      format = DRM_FORMAT_ABGR8888;
      BAD("no 8-bit RGBA format listed; trying %s anyway", fourcc_str(format).c_str());
    } else {
      OK("using %s%s", fourcc_str(format).c_str(),
         format == DRM_FORMAT_ABGR8888 ? " (= Dawn RGBA8Unorm)" :
         format == DRM_FORMAT_ARGB8888 ? " (= Dawn BGRA8Unorm)" : "");
    }

    std::vector<uint64_t> modifiers;
    uint32_t modCount = 0;
    if (ipc->GetDmabufModifiers(vr::VRApplication_Overlay, format, &modCount, nullptr) && modCount > 0) {
      modifiers.resize(modCount);
      ipc->GetDmabufModifiers(vr::VRApplication_Overlay, format, &modCount, modifiers.data());
      modifiers.resize(modCount);
    }
    list.clear();
    for (uint64_t m : modifiers)
      list += modifier_str(m) + " ";
    INFO("modifiers for %s: %s", fourcc_str(format).c_str(), list.empty() ? "(none)" : list.c_str());
    std::vector<uint64_t> usable;
    for (uint64_t m : modifiers) {
      if (m != DRM_FORMAT_MOD_INVALID)
        usable.push_back(m);
    }
    if (usable.empty())
      BAD("SteamVR lists no explicit modifiers (Gamescope skips dma-buf import in this case)");
    const bool linearListed =
        std::find(usable.begin(), usable.end(), DRM_FORMAT_MOD_LINEAR) != usable.end();
    if (opt.linear && !linearListed && !usable.empty())
      BAD("LINEAR is not in SteamVR's list; allocating LINEAR anyway (try --native-modifier too)");

    // ---- GBM allocation
    section("GBM buffers");
    if (!gbm.load()) {
      exitCode = 1;
      return;
    }
    std::string nodePath;
    drmFd = open_render_node(opt.drmNode, nodePath);
    if (drmFd < 0) {
      BAD("cannot open %s: %s", nodePath.c_str(), std::strerror(errno));
      exitCode = 1;
      return;
    }
    gbmDevice = gbm.create_device(drmFd);
    if (!gbmDevice) {
      BAD("gbm_create_device(%s) failed", nodePath.c_str());
      exitCode = 1;
      return;
    }
    OK("GBM device on %s", nodePath.c_str());

    // Three buffers: SteamVR may still be sampling the last two we handed it
    // (Gamescope keeps its queued and visible framebuffers alive the same way).
    const int bufferCount = opt.animate ? 3 : 1;
    for (int i = 0; i < bufferCount; ++i) {
      Buffer b;
      if (opt.linear) {
        const uint64_t linear = DRM_FORMAT_MOD_LINEAR;
        if (gbm.bo_create_with_modifiers)
          b.bo = gbm.bo_create_with_modifiers(gbmDevice, texW, texH, format, &linear, 1);
        if (!b.bo)
          b.bo = gbm.bo_create(gbmDevice, texW, texH, format, GBM_BO_USE_RENDERING | GBM_BO_USE_LINEAR);
      } else if (!usable.empty() && gbm.bo_create_with_modifiers) {
        b.bo = gbm.bo_create_with_modifiers(gbmDevice, texW, texH, format, usable.data(), unsigned(usable.size()));
      } else {
        b.bo = gbm.bo_create(gbmDevice, texW, texH, format, GBM_BO_USE_RENDERING);
      }
      if (!b.bo) {
        BAD("gbm_bo_create %ux%u %s failed", texW, texH, fourcc_str(format).c_str());
        exitCode = 1;
        return;
      }
      buffers.push_back(b);
    }
    const uint64_t modifier = gbm.bo_get_modifier(buffers[0].bo);
    const int planes = std::clamp(gbm.bo_get_plane_count(buffers[0].bo), 1, 4);
    OK("%d x %ux%u %s, modifier %s, %d plane(s), stride %u", bufferCount, texW, texH,
       fourcc_str(format).c_str(), modifier_str(modifier).c_str(), planes,
       gbm.bo_get_stride_for_plane(buffers[0].bo, 0));
    if (!usable.empty() && std::find(usable.begin(), usable.end(), modifier) == usable.end())
      BAD("GBM picked a modifier SteamVR did not list");

    // ---- import into SteamVR
    section("ImportDmabuf");
    for (size_t i = 0; i < buffers.size(); ++i) {
      Buffer& b = buffers[i];
      vr::DmabufAttributes_t attrs{};
      attrs.pNext = nullptr;
      attrs.unWidth = texW;
      attrs.unHeight = texH;
      attrs.unDepth = 1;
      attrs.unMipLevels = 1;
      attrs.unArrayLayers = 1;
      attrs.unSampleCount = 1;
      attrs.unFormat = format;
      attrs.ulModifier = gbm.bo_get_modifier(b.bo);
      attrs.unPlaneCount = uint32_t(planes);
      for (int p = 0; p < planes; ++p) {
        b.fds[p] = gbm.bo_get_fd_for_plane ? gbm.bo_get_fd_for_plane(b.bo, p) : (p == 0 ? gbm.bo_get_fd(b.bo) : -1);
        if (b.fds[p] < 0) {
          BAD("buffer %zu: no fd for plane %d", i, p);
          exitCode = 1;
          return;
        }
        attrs.plane[p].unOffset = gbm.bo_get_offset(b.bo, p);
        attrs.plane[p].unStride = gbm.bo_get_stride_for_plane(b.bo, p);
        attrs.plane[p].nFd = b.fds[p];
      }
      if (!ipc->ImportDmabuf(vr::VRApplication_Overlay, &attrs, &b.handle) || b.handle == 0) {
        BAD("buffer %zu: ImportDmabuf rejected it", i);
        b.handle = 0;
        exitCode = 1;
        return;
      }
      // Gamescope takes the first reference right after importing.
      if (!ipc->RefResource(b.handle, nullptr))
        BAD("buffer %zu: RefResource failed", i);
    }
    OK("%zu buffer(s) imported as shared texture handles", buffers.size());

    // ---- test pattern
    Canvas canvas;
    canvas.width = texW;
    canvas.height = texH;
    canvas.format = format;
    canvas.px.assign(size_t(texW) * texH, 0);
    const Layout layout = make_layout(opt.eyeWidth, opt.eyeHeight);
    draw_pattern(canvas, layout);
    const std::vector<uint32_t> barBand(canvas.px.begin() + size_t(layout.barY) * texW,
                                        canvas.px.begin() + size_t(layout.barY + layout.barH) * texW);
    if (opt.animate)
      draw_bar(canvas, layout, 0.0);
    for (Buffer& b : buffers) {
      if (!upload_rows(gbm, b, canvas, 0, texH)) {
        BAD("gbm_bo_map failed; cannot write the test pattern");
        exitCode = 1;
        return;
      }
    }

    // ---- overlay
    section("Overlay");
    vr::EVROverlayError oe = g_overlay->CreateOverlay("dusklight.sbs_probe", "Dusklight SbS probe", &ours);
    if (oe != vr::VROverlayError_None) {
      BAD("CreateOverlay: %s", ov_err(oe));
      exitCode = 1;
      return;
    }
    g_overlay->SetOverlayFlag(ours, vr::VROverlayFlags_SideBySide_Parallel, !opt.swapEyes);
    g_overlay->SetOverlayFlag(ours, vr::VROverlayFlags_SideBySide_Crossed, opt.swapEyes);
    g_overlay->SetOverlayFlag(ours, vr::VROverlayFlags_IgnoreTextureAlpha, true);
    g_overlay->SetOverlayTexelAspect(ours, opt.texelAspect);
    g_overlay->SetOverlayInputMethod(ours, vr::VROverlayInputMethod_None);
    uint32_t flags = 0;
    g_overlay->GetOverlayFlags(ours, &flags);
    const uint32_t sbsBit = opt.swapEyes ? vr::VROverlayFlags_SideBySide_Crossed : vr::VROverlayFlags_SideBySide_Parallel;
    if (flags & sbsBit)
      OK("created; %s set", opt.swapEyes ? "SideBySide_Crossed" : "SideBySide_Parallel");
    else
      BAD("created, but the side-by-side flag did not stick (flags 0x%08x)", flags);

    // ---- placement: over the game screen, which Steam lets you move and resize; we
    // copy its placement every frame. Until it exists, sit in front of the headset.
    vr::VROverlayHandle_t source = vr::k_ulOverlayHandleInvalid;
    SourcePlacement placement;
    // Each distinct failure is logged once; retries while waiting would repeat it.
    std::string lastFailure;
    const auto fail = [&](const std::string& why) {
      if (why != lastFailure) {
        BAD("%s", why.c_str());
        lastFailure = why;
      }
    };
    // Launched from Steam, our screen's key follows from our app ID; otherwise ask vrcmd.
    const uint32_t appId = own_app_id();
    std::string screenKey = opt.cover;
    if (screenKey.empty() && appId != 0)
      screenKey = "valve.steam.desktopgame." + std::to_string(appId);
    if (appId != 0)
      INFO("launched by Steam as app %u", appId);
    const auto pick_screen = [&](const std::vector<Listed>& screens, bool verbose) -> std::string {
      if (verbose) {
        for (const Listed& s : screens)
          INFO("game screen: %s (%s)", s.key.c_str(), s.visible ? "visible" : "hidden");
      }
      std::string key = choose_game_screen(screens);
      if (key.empty())
        fail(screens.empty() ? "no Gamescope game screen found yet" :
                               "several game screens; pick one with --cover KEY");
      return key;
    };
    bool describedSource = false;
    const auto attach = [&](const std::string& key) -> bool {
      vr::VROverlayHandle_t handle = vr::k_ulOverlayHandleInvalid;
      const vr::EVROverlayError fe = g_overlay->FindOverlay(key.c_str(), &handle);
      if (fe != vr::VROverlayError_None) {
        fail("game screen " + key + " not found yet (" + ov_err(fe) + ")");
        return false;
      }
      uint32_t srcSort = 0;
      g_overlay->GetOverlaySortOrder(handle, &srcSort);
      if (!describedSource) {
        describedSource = true;
        float texel = 0.f, srcWidth = 0.f;
        uint32_t srcW = 0, srcH = 0, srcFlags = 0;
        vr::VROverlayTransformType type = vr::VROverlayTransform_Invalid;
        g_overlay->GetOverlayTransformType(handle, &type);
        g_overlay->GetOverlayTexelAspect(handle, &texel);
        g_overlay->GetOverlayWidthInMeters(handle, &srcWidth);
        g_overlay->GetOverlayFlags(handle, &srcFlags);
        const vr::EVROverlayError sizeErr = g_overlay->GetOverlayTextureSize(handle, &srcW, &srcH);
        INFO("game screen %s: transform %s, width setting %.2f m, sort order %u, flags 0x%08x", key.c_str(),
             transform_type_name(type), srcWidth, srcSort, srcFlags);
        if (sizeErr == vr::VROverlayError_None)
          INFO("game screen texture %ux%u, texel aspect %.3f", srcW, srcH, texel);
        else
          INFO("game screen texture size unavailable (%s), texel aspect %.3f", ov_err(sizeErr), texel);
        if (srcFlags & (vr::VROverlayFlags_SideBySide_Parallel | vr::VROverlayFlags_SideBySide_Crossed))
          INFO("note: the game screen currently has SbS enabled (frame-sbs-toggle?)");
      }
      SourcePlacement p;
      std::string why;
      if (!read_placement(handle, p, why)) {
        fail(why);
        return false;
      }
      source = handle;
      placement = p;
      apply_placement(ours, placement);
      const uint32_t sort = srcSort == UINT32_MAX ? srcSort : srcSort + 1;
      g_overlay->SetOverlaySortOrder(ours, sort);
      if (p.sampled) {
        const Vec3 c{p.matrix.m[0][3], p.matrix.m[1][3], p.matrix.m[2][3]};
        OK("following %s by sampling points on it: %.2f x %.2f m, curvature %.2f, sort order %u",
           transform_type_name(p.type), p.widthM, p.heightM, p.curvature, sort);
        INFO("mouse scale %.0f x %.0f, center (%.2f, %.2f, %.2f) m", p.mouseScale.v[0], p.mouseScale.v[1], c.x, c.y,
             c.z);
        const double ourAspect = double(opt.eyeWidth) / opt.eyeHeight * opt.texelAspect;
        if (p.heightM > 0.f && std::fabs(p.widthM / p.heightM - ourAspect) > 0.02)
          INFO("aspect differs (screen %.3f vs probe %.3f per eye); --eye can match it", p.widthM / p.heightM,
               ourAspect);
      } else {
        OK("placed over the game screen (%s, %.2f m wide, curvature %.2f, sort order %u)",
           transform_type_name(p.type), p.widthM, p.curvature, sort);
      }
      return true;
    };

    bool attached = false;
    if (!opt.front) {
      if (!screenKey.empty()) {
        attached = attach(screenKey);
      } else {
        const std::string key = pick_screen(list_game_screens(opt.vrcmd), true);
        attached = !key.empty() && attach(key);
      }
    }
    if (!attached) {
      if (place_in_front(sys, ours, opt.distance, opt.widthM))
        OK("placed %.1f m in front of the headset, %.2f m wide", opt.distance, opt.widthM);
      else
        BAD("could not place the overlay");
      if (waitSeconds > 0)
        INFO("will keep looking for the game screen for %.0f s", waitSeconds);
    }

    oe = g_overlay->ShowOverlay(ours);
    if (oe != vr::VROverlayError_None)
      BAD("ShowOverlay: %s", ov_err(oe));

    std::printf(
        "\n== In the headset\n"
        "  1. Eye order: close your right eye -> RED square top-left; close your left eye ->\n"
        "     BLUE square top-right.\n"
        "  2. Depth: yellow square in front of the screen, green on it, cyan behind it%s.\n"
        "  3. Resolution: lean in toward the gratings. Each pair is vertical | horizontal\n"
        "     lines at 1, 2 and 3 px. If the vertical half of each pair is as sharp as the\n"
        "     horizontal half, each eye is getting full horizontal resolution. (From a\n"
        "     distance both shimmer: the overlay has more texels than the panel shows.)\n"
        "  4. Motion: the magenta bar (in front) should slide smoothly with no flicker.\n"
        "  5. Cover: the pattern should sit exactly over the game screen and follow it when\n"
        "     you move, resize or recenter it with Steam's controls. Check whether the\n"
        "     laser still reaches the screen's controls with the pattern on top.\n"
        "  Ctrl+C (or quit the app in Steam) to exit.\n",
        opt.animate ? "; the magenta bar is in front too" : "");

    // ---- submit loop
    section("Running");
    using clock = std::chrono::steady_clock;
    const auto start = clock::now();
    // Static mode only needs to keep polling events and following the game screen.
    const clock::duration step =
        opt.animate ? std::chrono::duration_cast<clock::duration>(std::chrono::duration<double>(1.0 / opt.fps)) :
                      clock::duration(std::chrono::milliseconds(16));
    auto nextFrame = start;
    auto lastDetect = start, lastStats = start;
    bool checkedTexture = false, waitReported = false;
    std::future<std::vector<Listed>> pendingScreens;  // vrcmd takes ~2 s; never block the loop on it
    // While the game screen moves, log what we read ~4x/s, plus once when it settles.
    auto lastChange = start, lastChangeLog = start;
    bool settled = true;
    const auto log_placement = [&](const char* what, double t) {
      const Vec3 c{placement.matrix.m[0][3], placement.matrix.m[1][3], placement.matrix.m[2][3]};
      vr::TrackedDevicePose_t head{};
      if (sys)
        sys->GetDeviceToAbsoluteTrackingPose(vr::TrackingUniverseStanding, 0.f, &head, 1);
      const auto& hm = head.mDeviceToAbsoluteTracking.m;
      const float dist = head.bPoseIsValid ? length(c - Vec3{hm[0][3], hm[1][3], hm[2][3]}) : -1.f;
      INFO("[%6.1fs] %-8s setting %.2f m | ours %.2f x %.2f m at (%.2f, %.2f, %.2f), %.2f m from head | "
           "matrix scale %.3f, facing %+.2f",
           t, what, placement.widthSetting, placement.widthM, placement.heightM, c.x, c.y, c.z, dist,
           placement.matrixScale, placement.facing);
    };
    if (source != vr::k_ulOverlayHandleInvalid)
      log_placement("start", 0.0);
    uint64_t frame = 0;
    uint32_t submits = 0, errors = 0, moves = 0;
    double worstCallMs = 0.0, worstFollowMs = 0.0;
    vr::EVROverlayError lastError = vr::VROverlayError_None;

    while (!g_quit) {
      const auto now = clock::now();
      const double elapsed = std::chrono::duration<double>(now - start).count();
      if (opt.seconds > 0 && elapsed >= opt.seconds)
        break;

      vr::VREvent_t ev;
      while (sys && sys->PollNextEvent(&ev, sizeof ev)) {
        if (ev.eventType == vr::VREvent_Quit) {
          INFO("SteamVR asked us to quit");
          g_quit = true;
        }
      }
      while (g_overlay->PollNextOverlayEvent(ours, &ev, sizeof ev)) {
      }
      if (haveWindow && !window.pump()) {
        INFO("window closed");
        g_quit = true;
      }

      if (source == vr::k_ulOverlayHandleInvalid && !opt.front) {
        if (elapsed < waitSeconds) {
          if (!screenKey.empty()) {
            if (now - lastDetect >= std::chrono::milliseconds(250)) {
              lastDetect = now;
              attach(screenKey);
            }
          } else if (pendingScreens.valid()) {
            if (pendingScreens.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
              const std::string key = pick_screen(pendingScreens.get(), false);
              if (!key.empty())
                attach(key);
            }
          } else if (now - lastDetect >= std::chrono::seconds(1)) {
            lastDetect = now;
            pendingScreens = std::async(std::launch::async, list_game_screens, opt.vrcmd);
          }
        } else if (waitSeconds > 0 && !waitReported) {
          waitReported = true;
          BAD("nothing to follow after %.0f s; staying in front of the headset", waitSeconds);
        }
      }

      if (source != vr::k_ulOverlayHandleInvalid) {
        const auto f0 = clock::now();
        SourcePlacement p;
        std::string why;
        if (read_placement(source, p, why) && !p.same_as(placement)) {
          placement = p;
          apply_placement(ours, placement);
          ++moves;
          lastChange = now;
          if (settled) {
            settled = false;
            lastChangeLog = now;
            log_placement("changing", elapsed);
          } else if (now - lastChangeLog >= std::chrono::milliseconds(250)) {
            lastChangeLog = now;
            log_placement("changing", elapsed);
          }
        } else if (!settled && now - lastChange >= std::chrono::milliseconds(500)) {
          settled = true;
          log_placement("settled", elapsed);
        }
        worstFollowMs = std::max(worstFollowMs, std::chrono::duration<double, std::milli>(clock::now() - f0).count());
      }

      if (opt.animate || frame == 0) {
        Buffer& b = buffers[frame % buffers.size()];
        if (opt.animate && frame > 0) {
          std::copy(barBand.begin(), barBand.end(), canvas.px.begin() + size_t(layout.barY) * texW);
          draw_bar(canvas, layout, elapsed);
          if (!upload_rows(gbm, b, canvas, uint32_t(layout.barY), uint32_t(layout.barH)))
            ++errors;
        }
        vr::Texture_t tex{&b.handle, vr::TextureType_SharedTextureHandle, vr::ColorSpace_Gamma};
        const auto t0 = clock::now();
        const vr::EVROverlayError se = g_overlay->SetOverlayTexture(ours, &tex);
        worstCallMs = std::max(worstCallMs, std::chrono::duration<double, std::milli>(clock::now() - t0).count());
        if (se != vr::VROverlayError_None) {
          ++errors;
          lastError = se;
        }
        ++submits;
        ++frame;
      }

      if (!checkedTexture && elapsed >= 1.0) {
        checkedTexture = true;
        uint32_t w = 0, h = 0;
        const vr::EVROverlayError te = g_overlay->GetOverlayTextureSize(ours, &w, &h);
        // Shared-handle textures report 0x0 here even when they display correctly.
        if (te == vr::VROverlayError_None && w == texW && h == texH)
          OK("SteamVR reports our texture as %ux%u", w, h);
        else if (te == vr::VROverlayError_None && w == 0 && h == 0)
          INFO("SteamVR reports 0x0 for shared-handle textures (expected)");
        else if (te == vr::VROverlayError_None)
          BAD("SteamVR reports our texture as %ux%u (expected %ux%u)", w, h, texW, texH);
        else
          INFO("texture size not reported for shared handles (%s)", ov_err(te));
        if (g_overlay->IsOverlayVisible(ours))
          OK("overlay is visible");
        else
          BAD("overlay is not visible");
        if (lastError != vr::VROverlayError_None)
          BAD("SetOverlayTexture: %s", ov_err(lastError));
      }

      if (opt.animate && now - lastStats >= std::chrono::seconds(5)) {
        const double secs = std::chrono::duration<double>(now - lastStats).count();
        INFO("%.1f submits/s, %u error(s), slowest SetOverlayTexture %.2f ms", submits / secs, errors, worstCallMs);
        if (source != vr::k_ulOverlayHandleInvalid)
          INFO("following: %u placement change(s), slowest read %.2f ms", moves, worstFollowMs);
        lastStats = now;
        submits = errors = moves = 0;
        worstCallMs = worstFollowMs = 0.0;
      }

      nextFrame += step;
      if (nextFrame < clock::now())
        nextFrame = clock::now();
      std::this_thread::sleep_until(nextFrame);
    }
  }();

  // ---- cleanup
  if (g_overlay && ours != vr::k_ulOverlayHandleInvalid) {
    g_overlay->ClearOverlayTexture(ours);
    g_overlay->DestroyOverlay(ours);
  }
  for (Buffer& b : buffers) {
    if (ipc && b.handle)
      ipc->UnrefResource(b.handle);
    for (int fd : b.fds) {
      if (fd >= 0)
        close(fd);
    }
    if (b.bo)
      gbm.bo_destroy(b.bo);
  }
  if (gbmDevice)
    gbm.device_destroy(gbmDevice);
  if (drmFd >= 0)
    close(drmFd);
  api.ShutdownInternal();
  window.destroy();
  std::printf("\nDone.\n");
  return exitCode;
}
