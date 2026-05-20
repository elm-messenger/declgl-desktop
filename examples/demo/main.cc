// declgl_demo — M2 round-trip test.
//
// Drives libdeclgl through its public C ABI:
//   1. declgl_init opens a window + GL ctx + audio device.
//   2. We encode a BackendCommandBatch in C++ and push it through
//      declgl_exec_backend_cmd; the engine decodes and prints what it saw.
//   3. The view callback returns a small Renderable each frame; the engine
//      decodes it and prints its kind. (Real rendering arrives in M3.)
//
// This shuts down after a fixed number of frames so it's also useful as a
// non-interactive smoke test.

#include "c_api/declgl.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "transport_backend.pb.h"
#include "transport_render.pb.h"

namespace {

// Pre-encoded Renderable bytes. The engine borrows this every frame; the
// demo owns the buffer for the whole run.
struct DemoState {
    std::string view_bytes;
    int32_t     frames_remaining = 240;  // ~4s at 60fps
};

declgl_status_t view_cb(void*           userdata,
                        const uint8_t** out_bytes,
                        size_t*         out_len) {
    auto* st = static_cast<DemoState*>(userdata);
    *out_bytes = reinterpret_cast<const uint8_t*>(st->view_bytes.data());
    *out_len   = st->view_bytes.size();
    return DECLGL_OK;
}

void event_cb(void* /*userdata*/,
              declgl_event_kind_t kind,
              const uint8_t*      bytes,
              size_t              len) {
    std::printf("[demo] event kind=%d bytes=%zu\n",
                static_cast<int>(kind), len);
    (void)bytes;
}

// Build a small BackendCommandBatch and encode it to a byte string.
std::string make_backend_batch() {
    using namespace mlregl::transport::backend;
    BackendCommandBatch batch;

    {
        auto* cmd = batch.add_commands();
        auto* lt  = cmd->mutable_load_texture();
        lt->set_name("enemy");
        lt->set_url("/test/assets/enemy.png");
        auto* opts = lt->mutable_options();
        opts->set_mag(TextureMagOption::TEXTURE_MAG_OPTION_LINEAR);
        opts->set_min(TextureMinOption::TEXTURE_MIN_OPTION_LINEAR);
    }
    {
        auto* cmd = batch.add_commands();
        auto* lf  = cmd->mutable_load_font();
        lf->set_name("consolas");
        lf->set_image_url("/assets/consolas.png");
        lf->set_json_url("/assets/consolas.json");
    }
    {
        auto* cmd = batch.add_commands();
        auto* sr  = cmd->mutable_start_regl();
        sr->set_virt_width(1280);
        sr->set_virt_height(720);
        sr->set_fbo_num(2);
    }

    std::string out;
    batch.SerializeToString(&out);
    return out;
}

// Build a single AtomicRenderable so the engine has something to decode
// each frame.
std::string make_view_bytes() {
    using namespace mlregl::transport::render;
    Renderable r;
    auto* atomic = r.mutable_atomic();
    atomic->set_program("builtin_textured_quad");
    std::string out;
    r.SerializeToString(&out);
    return out;
}

}  // namespace

int main(int /*argc*/, char* /*argv*/[]) {
    GOOGLE_PROTOBUF_VERIFY_VERSION;

    declgl_init_config_t cfg{};
    cfg.window_title    = "declgl_demo (M2)";
    cfg.window_width    = 1280;
    cfg.window_height   = 720;
    cfg.asset_root      = "";
    cfg.io_thread_count = 0;  // auto

    declgl_engine_t* eng = declgl_init(&cfg);
    if (!eng) {
        std::fprintf(stderr, "declgl_init failed: %s\n", declgl_last_error());
        return 1;
    }

    DemoState state;
    state.view_bytes = make_view_bytes();

    declgl_callbacks_t cb{};
    cb.userdata = &state;
    cb.view     = view_cb;
    cb.event    = event_cb;
    declgl_set_callbacks(eng, &cb);

    // Send a one-shot setup batch.
    {
        const std::string batch = make_backend_batch();
        declgl_status_t s = declgl_exec_backend_cmd(
            eng,
            reinterpret_cast<const uint8_t*>(batch.data()),
            batch.size());
        if (s != DECLGL_OK) {
            std::fprintf(stderr,
                         "declgl_exec_backend_cmd failed: %d (%s)\n",
                         s, declgl_last_error());
        }
    }

    while (declgl_should_run(eng) && state.frames_remaining-- > 0) {
        declgl_run_frame(eng);
    }

    declgl_shutdown(eng);
    google::protobuf::ShutdownProtobufLibrary();
    return 0;
}
