/*
 * Frame-3D-Lib: full-resolution side-by-side stereo output for flat apps running
 * on the Steam Frame (and other SteamVR-on-Linux setups where Steam shows the app
 * on a screen in VR).
 *
 * The library creates a SteamVR overlay of its own with the side-by-side flag set,
 * backed by dma-buf images that the host renderer draws into, and keeps that
 * overlay over the app's Steam game screen as the user moves, resizes or curves it.
 * The app stays a normal Gamescope app, so its input (Steam Input / SDL) is
 * unaffected.
 *
 * Renderer-agnostic: the library allocates the images and reports their dma-buf
 * layout; the host imports them into its own API (Dawn, Vulkan, GL, or CPU via
 * frame3d_map). Per frame:
 *
 *     int i = frame3d_acquire(ctx);            // -1: nothing free, skip this frame
 *     ...render left eye into the left half, right eye into the right half...
 *     frame3d_submit(ctx, i, sync_fd);         // fd signalled when rendering ends
 *
 * A library thread waits for the fence before handing the image to SteamVR, so
 * only finished frames are ever shown. All other SteamVR traffic (screen
 * following, events) also runs on that thread.
 *
 * Linux only. Other platforms build a stub whose frame3d_create returns NULL, so
 * hosts can link it unconditionally. Nothing is linked against SteamVR or Mesa:
 * libopenvr_api.so and libgbm are loaded at runtime.
 */
#ifndef FRAME3D_FRAME3D_H
#define FRAME3D_FRAME3D_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct frame3d_context frame3d_context;

typedef enum frame3d_log_level {
  FRAME3D_LOG_DEBUG = 0,
  FRAME3D_LOG_INFO = 1,
  FRAME3D_LOG_WARN = 2,
  FRAME3D_LOG_ERROR = 3,
} frame3d_log_level;

typedef void (*frame3d_log_fn)(frame3d_log_level level, const char* message, void* user);

/* DRM fourcc codes for the formats the library may choose (see frame3d_buffer_info). */
#define FRAME3D_FOURCC(a, b, c, d)                                                                  \
  ((uint32_t)(uint8_t)(a) | ((uint32_t)(uint8_t)(b) << 8) | ((uint32_t)(uint8_t)(c) << 16) |      \
   ((uint32_t)(uint8_t)(d) << 24))
#define FRAME3D_FORMAT_RGBA8 FRAME3D_FOURCC('A', 'B', '2', '4') /* DRM_FORMAT_ABGR8888: R,G,B,A bytes */
#define FRAME3D_FORMAT_BGRA8 FRAME3D_FOURCC('A', 'R', '2', '4') /* DRM_FORMAT_ARGB8888: B,G,R,A bytes */
#define FRAME3D_MODIFIER_LINEAR ((uint64_t)0)

typedef struct frame3d_config {
  /* Per-eye size. Images are (2 * eye_width) x eye_height, left eye on the left. */
  uint32_t eye_width;
  uint32_t eye_height;
  /* Images in the ring. Two are held by SteamVR (shown and previously shown), so
   * 4 lets the host render while another frame waits on its fence. 0 = 4. */
  uint32_t buffer_count;
  /* FRAME3D_FORMAT_RGBA8 or FRAME3D_FORMAT_BGRA8 to match the host's render
   * target format; 0 = whichever SteamVR accepts, RGBA8 first. */
  uint32_t drm_format;
  /* The host fills images on the CPU through frame3d_map. Forces a LINEAR layout. */
  bool cpu_writable;
  /* Use a LINEAR layout even for GPU rendering. By default the driver picks among
   * the layouts SteamVR accepts (on the Frame, Qualcomm's compressed UBWC). If the
   * host renderer can't import that layout, recreate with this set. */
  bool prefer_linear;
  /* SteamVR overlay to follow. NULL = this app's own Steam game screen, found from
   * the Steam app ID in the process's cgroup (the lookup Gamescope does). */
  const char* screen_key;
  /* Placement in front of the headset when there is no screen to follow. */
  float fallback_distance_m;
  float fallback_width_m;
  /* Logging. NULL = stderr. Messages below log_level are dropped. */
  frame3d_log_fn log;
  void* log_user;
  frame3d_log_level log_level;
} frame3d_config;

/* Fills in defaults: 1920x1080 per eye, 4 images, follow this app's screen, 2 m /
 * 1.6 m fallback, INFO logging to stderr. */
void frame3d_config_init(frame3d_config* config);

/* Cheap and side-effect free: true when this is Linux and SteamVR's
 * libopenvr_api.so can be loaded. Use it to decide whether to offer the mode. */
bool frame3d_runtime_available(void);

/* Connects to SteamVR as an overlay app, allocates the images and shows the
 * overlay. NULL on failure, with the reason logged. One context per process. */
frame3d_context* frame3d_create(const frame3d_config* config);
void frame3d_destroy(frame3d_context* ctx);

#define FRAME3D_MAX_PLANES 4

typedef struct frame3d_plane {
  int fd; /* owned by the library; dup() it if your importer takes ownership */
  uint32_t offset;
  uint32_t stride;
} frame3d_plane;

typedef struct frame3d_buffer_info {
  uint32_t width;  /* 2 * eye_width */
  uint32_t height; /* eye_height */
  uint32_t drm_format;   /* FRAME3D_FORMAT_RGBA8 or FRAME3D_FORMAT_BGRA8 */
  uint64_t drm_modifier; /* FRAME3D_MODIFIER_LINEAR or a vendor layout */
  uint32_t plane_count;
  frame3d_plane planes[FRAME3D_MAX_PLANES];
} frame3d_buffer_info;

/* The images, for importing into the host renderer once after frame3d_create. */
uint32_t frame3d_buffer_count(const frame3d_context* ctx);
bool frame3d_get_buffer_info(const frame3d_context* ctx, uint32_t index, frame3d_buffer_info* info);

/* Index of an image that SteamVR is not using and the host may render into, or -1
 * when none is free (skip the overlay this frame). */
int frame3d_acquire(frame3d_context* ctx);

/* Shows an acquired image once sync_fd (a sync_file) signals; -1 if the image is
 * already complete. Takes ownership of sync_fd. A frame still waiting on its fence
 * when a newer one arrives is dropped. */
bool frame3d_submit(frame3d_context* ctx, int index, int sync_fd);

/* Returns an acquired image without showing it. */
void frame3d_release(frame3d_context* ctx, int index);

/* CPU access to an acquired image (cpu_writable contexts). Returns the first byte
 * and the row pitch. */
void* frame3d_map(frame3d_context* ctx, int index, uint32_t* stride);
void frame3d_unmap(frame3d_context* ctx, int index);

/* Hide or show the overlay (it also hides itself while the game screen is hidden). */
void frame3d_set_visible(frame3d_context* ctx, bool visible);

/* With no screen to follow, re-place the overlay in front of the headset. */
void frame3d_recenter(frame3d_context* ctx);

typedef enum frame3d_placement {
  FRAME3D_PLACEMENT_FIXED = 0,     /* in front of the headset; no screen to follow */
  FRAME3D_PLACEMENT_FOLLOWING = 1, /* over the game screen */
} frame3d_placement;

typedef struct frame3d_status {
  frame3d_placement placement;
  bool visible;   /* the overlay is currently shown */
  float width_m;  /* current overlay size and curvature */
  float height_m;
  float curvature; /* OpenVR's definition: width / (2 * pi * radius) */
  bool quit_requested; /* SteamVR asked the app to quit */
} frame3d_status;

void frame3d_get_status(const frame3d_context* ctx, frame3d_status* status);

#ifdef __cplusplus
}
#endif

#endif /* FRAME3D_FRAME3D_H */
