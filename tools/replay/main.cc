// replay — Reads a capture.bin produced by declgl-desktop/tools/capture/
// (the OCaml capture tool) and replays every record through the libdeclgl
// C ABI. This is the M2.5 acceptance gate: any wire-format mismatch
// between the OCaml frontend and the C++ backend will surface here as a
// decode error.
//
// Capture file format (little-endian, written by ../capture/capture.ml):
//   magic  "DGLCAP01"             8 bytes
//   u32    record_count
//   record :=
//     u8   kind  (1=BE_CMD, 2=AU_CMD, 3=VIEW)
//     u64  ts_ms
//     u32  len
//     u8[len] payload                 (protobuf bytes)
//
// Usage:
//   declgl_replay [path-to-capture.bin]   (default: ./capture.bin)

#include "c_api/declgl.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {

constexpr uint8_t  KIND_BE_CMD = 1;
constexpr uint8_t  KIND_AU_CMD = 2;
constexpr uint8_t  KIND_VIEW   = 3;
constexpr char     MAGIC[]     = "DGLCAP01";

struct Record {
    uint8_t              kind;
    uint64_t             ts_ms;
    std::vector<uint8_t> payload;
};

// Replay state shared with the view_cb. Holds the most-recent VIEW bytes;
// the engine asks for them each frame via the view callback.
struct ReplayState {
    std::vector<uint8_t> latest_view;
    int                  view_count    = 0;
    int                  be_count      = 0;
    int                  au_count      = 0;
    int                  decode_errors = 0;
};

declgl_status_t view_cb(void*           userdata,
                        const uint8_t** out_bytes,
                        size_t*         out_len) {
    auto* st = static_cast<ReplayState*>(userdata);
    *out_bytes = st->latest_view.data();
    *out_len   = st->latest_view.size();
    return DECLGL_OK;
}

void event_cb(void* /*userdata*/, declgl_event_kind_t kind,
              const uint8_t* /*bytes*/, size_t len) {
    std::printf("[replay] backend->host event kind=%d bytes=%zu\n",
                static_cast<int>(kind), len);
}

bool read_exact(std::ifstream& f, void* dst, size_t n) {
    f.read(static_cast<char*>(dst), static_cast<std::streamsize>(n));
    return static_cast<size_t>(f.gcount()) == n;
}

bool load_capture(const std::string& path, std::vector<Record>* out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        std::fprintf(stderr, "[replay] cannot open %s\n", path.c_str());
        return false;
    }
    char magic[8];
    if (!read_exact(f, magic, 8) ||
        std::memcmp(magic, MAGIC, 8) != 0) {
        std::fprintf(stderr, "[replay] bad magic in %s\n", path.c_str());
        return false;
    }
    uint32_t record_count = 0;
    if (!read_exact(f, &record_count, 4)) {
        std::fprintf(stderr, "[replay] cannot read record_count\n");
        return false;
    }
    out->reserve(record_count);
    for (uint32_t i = 0; i < record_count; ++i) {
        Record r{};
        if (!read_exact(f, &r.kind,  1) ||
            !read_exact(f, &r.ts_ms, 8)) {
            std::fprintf(stderr, "[replay] truncated record header @%u\n", i);
            return false;
        }
        uint32_t len = 0;
        if (!read_exact(f, &len, 4)) {
            std::fprintf(stderr, "[replay] truncated record len @%u\n", i);
            return false;
        }
        r.payload.resize(len);
        if (len > 0 && !read_exact(f, r.payload.data(), len)) {
            std::fprintf(stderr, "[replay] truncated record payload @%u\n", i);
            return false;
        }
        out->push_back(std::move(r));
    }
    return true;
}

}  // namespace

int main(int argc, char* argv[]) {
    const std::string path = (argc >= 2) ? argv[1] : "capture.bin";

    std::vector<Record> records;
    if (!load_capture(path, &records)) return 1;

    std::printf("[replay] loaded %zu records from %s\n",
                records.size(), path.c_str());

    declgl_init_config_t cfg{};
    cfg.window_title    = "declgl_replay (M2.5)";
    cfg.window_width    = 1280;
    cfg.window_height   = 720;
    cfg.asset_root      = "";
    cfg.io_thread_count = 0;

    declgl_engine_t* eng = declgl_init(&cfg);
    if (!eng) {
        std::fprintf(stderr, "[replay] declgl_init failed: %s\n",
                     declgl_last_error());
        return 1;
    }

    ReplayState state;

    declgl_callbacks_t cb{};
    cb.userdata = &state;
    cb.view     = view_cb;
    cb.event    = event_cb;
    declgl_set_callbacks(eng, &cb);

    for (size_t i = 0; i < records.size() && declgl_should_run(eng); ++i) {
        const Record& r = records[i];
        switch (r.kind) {
        case KIND_BE_CMD: {
            declgl_status_t s = declgl_exec_backend_cmd(
                eng, r.payload.data(), r.payload.size());
            if (s != DECLGL_OK) {
                std::fprintf(stderr,
                             "[replay] BE_CMD #%zu decode failed: %d (%s)\n",
                             i, s, declgl_last_error());
                ++state.decode_errors;
            }
            ++state.be_count;
            break;
        }
        case KIND_AU_CMD: {
            declgl_status_t s = declgl_exec_audio_cmd(
                eng, r.payload.data(), r.payload.size());
            if (s != DECLGL_OK) {
                std::fprintf(stderr,
                             "[replay] AU_CMD #%zu decode failed: %d (%s)\n",
                             i, s, declgl_last_error());
                ++state.decode_errors;
            }
            ++state.au_count;
            break;
        }
        case KIND_VIEW: {
            state.latest_view = r.payload;
            ++state.view_count;
            declgl_run_frame(eng);
            break;
        }
        default:
            std::fprintf(stderr, "[replay] record #%zu unknown kind=%u\n",
                         i, r.kind);
            ++state.decode_errors;
        }
    }

    std::printf(
        "[replay] done: be_cmd=%d au_cmd=%d view=%d decode_errors=%d\n",
        state.be_count, state.au_count, state.view_count,
        state.decode_errors);

    declgl_shutdown(eng);
    return state.decode_errors == 0 ? 0 : 1;
}
