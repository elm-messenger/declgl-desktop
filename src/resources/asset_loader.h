#pragma once

// resources/asset_loader.h — async file decode for textures + fonts.
//
// Why this exists
// ---------------
// LoadTexture / LoadFont commands can arrive at any time — including
// mid-game in response to a level transition or an in-game asset bundle
// becoming reachable. Doing the PNG decode + premultiply + JSON parse
// synchronously on the GL thread inside [Engine::dispatch_backend_command]
// would stall the frame for tens to hundreds of ms on a multi-MB
// texture, producing a visible hitch. This module moves that work to a
// dedicated I/O worker thread and hands the GL thread back a fully-
// decoded RGBA8 buffer (and, for fonts, a parsed [Font]) ready to be
// uploaded with a single [glTexImage2D] call.
//
// Threading model
// ---------------
//   GL thread                          Worker thread
//   ─────────                          ─────────────
//   enqueue_texture(...)  ──push──►   decode PNG
//                                      premultiply (if requested)
//   enqueue_font(...)     ──push──►   parse BMFont JSON
//                                      decode atlas PNG
//                                      (font atlas is *never* premultiplied)
//                                              │
//                                              ▼
//                          ◄──push──    ready_queue_
//   drain_ready(N)
//      ├─ glTexImage2D
//      ├─ register in TextureRegistry / FontRegistry
//      └─ ship_event(texture_loaded / font_loaded / *_loadfail)
//
// Invariants
// ----------
//   - Exactly one worker thread. FIFO load order preserved.
//   - No GL calls off the GL thread (macOS doesn't share contexts
//     freely; we stay portable and avoid the headache).
//   - Both queues are protected by the same mutex; one condvar wakes
//     the worker when new jobs arrive or shutdown is requested.
//   - On shutdown, any jobs still in [decode_queue_] are dropped on
//     the floor (they were never observed by the OCaml side as
//     "loaded"; their corresponding events simply never ship).
//
// Failure handling
// ----------------
// Decode/parse failures are captured in the [ReadyAsset] (see
// [error] field). The GL-thread drain ships a {texture,font}_loadfail
// event in that case, mirroring the synchronous path's behaviour.

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

#include "resources/image_decoder.h"
#include "audio/audio_decoder.h"

namespace declgl
{

class Font;

// What kind of asset a job represents. Drives the worker's decode
// strategy and the GL-thread drain's upload + register logic.
enum class AssetKind {
	Texture,
	Font,
	Audio,
	File,
	KvLoad,
	KvSave,
};

// One job submitted by the GL thread to the worker.
struct DecodeJob {
	AssetKind kind = AssetKind::Texture;

	// Logical name used to register the asset (e.g. "enemy" or
	// "custom"). Round-trips back into the corresponding _loaded event.
	std::string name;

	// For Texture: the image file on disk.
	// For Font:    the atlas PNG on disk.
	std::string image_url;

	// Font only: the BMFont JSON file on disk.
	std::string json_url;

	// Texture only: optional crop region (zeros = full image).
	ImageCrop crop{};

	// Texture only: should the worker premultiply RGB by alpha after
	// decode? Forced false for Font (SDF data must be linear). Mirrors
	// the LoadTexture proto's negated `no_premultiply_alpha` flag (the
	// negation is unwound by the engine before enqueueing).
	bool premultiply_alpha = true;

	// Texture only: filter / mipmap selections (resolved at upload
	// time on the GL thread). Forwarded verbatim through the worker.
	int min_filter_enum = 0; // proto TextureMinOption
	int mag_filter_enum = 0; // proto TextureMagOption

	// Audio only: file path is in [image_url] (we re-use that field
	// to avoid bloating the job struct). [audio_sample_rate] is the
	// device rate the worker should resample to. Set by the engine
	// from [AudioEngine::device_sample_rate()] at enqueue time.
	uint32_t audio_sample_rate = 0;

	// KvSave only: snapshot to persist as JSON. [image_url] is the target path.
	std::unordered_map<std::string, std::string> kv_values;
};

// What the worker hands back to the GL thread once the job is done.
struct ReadyAsset {
	AssetKind kind = AssetKind::Texture;
	std::string name;
	std::string
		image_url; // also serves as the texture-registry key for fonts

	// Empty on success; short human-readable explanation on failure.
	std::string error;

	// Decoded image (always present on success, regardless of kind —
	// for fonts this is the atlas atlas RGBA8). On failure, [.ok()] is
	// false.
	DecodedImage image;

	// Font-only: the parsed [Font]. nullptr for texture jobs and for
	// failed font jobs.
	std::unique_ptr<Font> font;

	// Audio-only: decoded + device-rate-normalized PCM. Empty for
	// texture/font jobs and for failed audio jobs.
	DecodedAudio audio;

	// Audio-only: classification of the failure (mirrors the proto's
	// AudioLoadError enum). [None] on success.
	AudioDecodeError audio_error = AudioDecodeError::None;

	// File only: loaded text bytes. KvLoad only: values parsed from JSON.
	std::string file_data;
	std::unordered_map<std::string, std::string> kv_values;

	// Echo of the job's filter selections (texture only).
	int min_filter_enum = 0;
	int mag_filter_enum = 0;
	bool premultiply_alpha = true;
};

class AssetLoader {
    public:
	AssetLoader();
	~AssetLoader();

	AssetLoader(const AssetLoader &) = delete;
	AssetLoader &operator=(const AssetLoader &) = delete;

	// Push a new job. Returns immediately. Wake the worker if it was
	// idle. Safe to call from the GL thread.
	void enqueue(DecodeJob job);

	// Pop up to [max_items] ready assets in FIFO order; 0 means unlimited.
	// Returns the
	// number actually moved into [out]. Bounded to avoid one-frame
	// hitches when many large assets land at once.
	std::size_t drain_ready(std::vector<ReadyAsset> &out,
				std::size_t max_items);

	// Drop any pending or ready jobs matching [kind] + [name]. Called
	// from the engine's UnloadTexture / UnloadFont path so that an
	// unload arriving while a load is still in flight (worker hasn't
	// run yet, or worker is done but GL hasn't drained yet) doesn't
	// result in a zombie texture being uploaded after the user thinks
	// it's gone. Texture identity is [name]; font identity is [name]
	// (its atlas texture lives in TextureRegistry under image_url and
	// is unregistered separately by the engine).
	//
	// Note: this can't cancel a job that the worker is *currently*
	// running. That job will still complete, push to [ready_queue_],
	// and then be dropped on the next [drain_ready] thanks to the
	// ready-queue sweep below. Net result: zero leaks, at most one
	// wasted decode.
	void cancel_pending(AssetKind kind, std::string_view name);

	// Optional: stop the worker proactively. Called automatically by
	// the destructor; engine shutdown calls it explicitly so the
	// worker doesn't outlive the GL context.
	void stop();

    private:
	void worker_main();
	void process(DecodeJob &job, ReadyAsset &out);

	// Asset paths from the wire are confined to this root (which is
	// SDL_GetBasePath() — the directory containing the running
	// executable). Captured once at construction; never mutated so
	// the worker thread reads it without synchronization. Empty if
	// SDL couldn't determine the base path; in that case the worker
	// rejects every relative-path request rather than silently
	// reading the cwd.
	std::filesystem::path asset_root_;

	std::thread worker_;
	std::mutex mu_;
	std::condition_variable cv_;
	std::deque<DecodeJob> decode_queue_;
	std::deque<ReadyAsset> ready_queue_;
	std::atomic<bool> stop_{ false };
};

} // namespace declgl
