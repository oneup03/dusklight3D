// Non-Linux builds: the API links but the mode is never available.
#include "frame3d/frame3d.h"

extern "C" {

void frame3d_config_init(frame3d_config* config) {
  *config = {};
  config->eye_width = 1920;
  config->eye_height = 1080;
  config->buffer_count = 4;
  config->fallback_distance_m = 2.0f;
  config->fallback_width_m = 1.6f;
  config->log_level = FRAME3D_LOG_INFO;
}

bool frame3d_runtime_available(void) { return false; }
frame3d_context* frame3d_create(const frame3d_config*) { return nullptr; }
void frame3d_destroy(frame3d_context*) {}
uint32_t frame3d_buffer_count(const frame3d_context*) { return 0; }
bool frame3d_get_buffer_info(const frame3d_context*, uint32_t, frame3d_buffer_info*) { return false; }
int frame3d_acquire(frame3d_context*) { return -1; }
bool frame3d_submit(frame3d_context*, int, int) { return false; }
void frame3d_release(frame3d_context*, int) {}
void* frame3d_map(frame3d_context*, int, uint32_t*) { return nullptr; }
void frame3d_unmap(frame3d_context*, int) {}
void frame3d_set_visible(frame3d_context*, bool) {}
void frame3d_recenter(frame3d_context*) {}
void frame3d_get_status(const frame3d_context*, frame3d_status* status) {
  if (status)
    *status = {};
}

}  // extern "C"
