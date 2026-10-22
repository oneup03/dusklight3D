# Frame-3D-Lib

Full-resolution side-by-side stereo for flat apps running on the **Steam Frame**,
and other SteamVR-on-Linux setups where Steam shows the app on a screen in VR.

Steam can already split a game screen into two eyes (the SbS toggle), but then each
eye only gets half of Gamescope's surface. This library instead gives SteamVR an
overlay of the app's own, flagged side-by-side and backed by images the app renders
into at full resolution per eye. The overlay is kept over the app's Steam game
screen, so moving, resizing and curving the screen with Steam's controls still works.
The app stays an ordinary Gamescope app, so its input (Steam Input / SDL) is
untouched.

- **Renderer-agnostic.** The library allocates dma-buf images and reports their
  layout; the app imports them into Dawn, Vulkan, GL, or writes them on the CPU.
- **No link-time dependencies.** `libopenvr_api.so` (shipped with SteamVR) and
  `libgbm` are loaded at runtime. Only the pinned, header-only `openvr.h` is
  vendored (`third_party/openvr`, BSD-3-Clause).
- **C API**, C99 header, C++17 implementation.
- **Linux only.** Other platforms build a stub whose `frame3d_create` returns NULL,
  so the library can be linked unconditionally.

## Using it

```cmake
add_subdirectory(extern/Frame-3D-Lib)
target_link_libraries(your_target PRIVATE frame3d::frame3d)
```

`FRAME3D_SUPPORTED` is defined to 1 or 0 for consumers.

```c
#include <frame3d/frame3d.h>

if (frame3d_runtime_available()) {            /* cheap: offer the mode or not */
  frame3d_config cfg;
  frame3d_config_init(&cfg);
  cfg.eye_width = 1920;                       /* images are 2*eye_width x eye_height */
  cfg.eye_height = 1080;
  frame3d_context* ctx = frame3d_create(&cfg);

  for (uint32_t i = 0; i < frame3d_buffer_count(ctx); ++i) {
    frame3d_buffer_info info;
    frame3d_get_buffer_info(ctx, i, &info);   /* import once: fds, offsets, strides, */
    /* ...import into your renderer... */     /* DRM format and modifier             */
  }

  /* every frame */
  int i = frame3d_acquire(ctx);               /* -1: none free, skip this frame */
  /* ...render left eye into the left half, right eye into the right half... */
  frame3d_submit(ctx, i, sync_fd);            /* sync_file signalled when done, or -1 */
}
```

`frame3d_submit` takes ownership of the fence. A library thread waits for it before
handing the image to SteamVR, so only finished frames are shown; the two most
recently shown images are held back from `frame3d_acquire` because SteamVR may
still be sampling them.

### Importing the images

- **Dawn (Vulkan backend):** request `SharedTextureMemoryDmaBuf` and
  `SharedFenceSyncFD`, import each image with `SharedTextureMemoryDmaBufDescriptor`
  (format, modifier, planes), and pass the sync fd exported by `EndAccess` to
  `frame3d_submit`.
- **Vulkan:** `VK_EXT_external_memory_dma_buf` + `VK_EXT_image_drm_format_modifier`
  for the images, `VK_KHR_external_semaphore_fd` with a `SYNC_FD` export for the
  fence.
- **CPU:** set `cpu_writable` and use `frame3d_map` / `frame3d_unmap` (forces a
  LINEAR layout).

`drm_format` is `FRAME3D_FORMAT_RGBA8` (R,G,B,A bytes) or `FRAME3D_FORMAT_BGRA8`.
By default the driver picks a layout from those SteamVR accepts (on the Frame,
Qualcomm's compressed UBWC); if your renderer can't import it, recreate with
`prefer_linear`.

## How the screen is followed

On the Frame, Steam's game screen is a `DashboardTab` overlay. The dashboard
places it, and OpenVR has no getter for that transform. Its other queries are
incomplete too: `GetTransformForOverlayCoordinates` reports the right center but
ignores the scale applied when the user resizes the screen, and
`GetOverlayCurvature` reports 0 although the dashboard curves it.

So the library measures the screen with the laser hit test
(`ComputeOverlayIntersection`), which has to match what is drawn. Rays from the head
through the screen's left edge, center and right edge give three surface points:
their circle is the screen's curvature (OpenVR's definition, width / (2 pi r)), and
their UVs give its true width and center. A fourth ray above the center gives the
height. The overlay is placed 2 cm in front of the screen with the same curvature.

The screen is found by key: `valve.steam.desktopgame.<appid>`, with the Steam app ID
read from the process's cgroup (`app-steam-app<id>-<pid>.scope`), which is
Gamescope's own lookup. Pass `screen_key` to follow a different overlay.

Cadence: a one-call movement check every 100 ms and a full measurement every second
while the screen is still; a full measurement at ~60 Hz while it changes, until it has
been still for half a second. With no screen to follow (not launched from Steam), the
overlay sits in front of the headset; `frame3d_recenter` re-places it. The overlay
also hides while the game screen is hidden.

## Test pattern

`examples/test_pattern` shows a stereo test image through the library: eye-order
tags, near/screen/far squares, resolution gratings and a moving bar. Add it to Steam
as a non-Steam game and launch it there, so it gets a game screen to follow. It
opens a blank window for that, and logs to `~/frame3d-test-pattern.log` when there
is no terminal.

```sh
cmake -S . -B build -DFRAME3D_BUILD_EXAMPLES=ON
cmake --build build
```
