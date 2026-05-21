#pragma once

// audio/audio_engine.h — declarative audio runtime for declgl-desktop.
//
// Mirrors the JS backend (`ml-regl-js/src/audio.js`) one-for-one but
// runs natively against an SDL3 audio device and a pull-style mixer
// in the audio callback thread.
//
// Threading model
// ---------------
// Three threads touch the audio engine:
//
//   1. The GL/main thread runs [exec_audio_cmd], [register_buffer]
//      and [unregister_buffer]. Pushes onto the command ring.
//   2. The asset-loader worker thread DOES NOT touch this object.
//      It produces a [DecodedAudio]; the GL thread calls
//      [register_buffer] to install it.
//   3. The SDL audio thread invokes [audio_callback], reads the ring,
//      mixes whatever voices are live, emits stereo f32 frames.
//
// Buffer lifetime is single-ownership: the GL thread owns
// [Buffer]s, the audio thread reads them through raw pointers
// captured in StartSound commands. Unload is two-phase: GL
// thread enqueues a [ReleaseBuffer] command carrying the
// [unique_ptr<Buffer>] by move, the audio thread stops every
// voice referencing it and then drops the pointer (freeing
// memory after every read has happened in mix-call ordering).
//
// Time domain
// -----------
// Times in the proto / OCaml side are absolute milliseconds in the
// same clock OCaml reports via [now_ms] (the bridge passes that
// through). The audio thread maintains its own engine-frames
// counter; ms → device frames conversion uses
// [(t_ms - now_ms_at_cmd) * sample_rate / 1000 + frames_at_cmd]
// to land timeline events on the right output frame regardless of
// audio buffer scheduling drift.

#include <SDL3/SDL.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "audio/audio_decoder.h"

namespace declgl
{

// Bridge → engine sink for serialized AudioBackendEvent payloads.
// Same shape as the BackendEvent sink on the GL side — see engine.h.
using AudioEventSink =
	std::function<void(const uint8_t *bytes, std::size_t len)>;

class AudioEngine {
    public:
	AudioEngine();
	~AudioEngine();

	AudioEngine(const AudioEngine &) = delete;
	AudioEngine &operator=(const AudioEngine &) = delete;

	// Bring up the SDL audio device. Sample rate comes from SDL's
	// idea of the default playback device; the ml-regl wire format
	// reports it back to OCaml via [audio_context_ready]. Returns
	// false on failure (call [SDL_GetError()] for details).
	//
	// Idempotent — a second call returns true with no side effects.
	bool ensure_open();

	// Wire the OCaml-side ship-callback for AudioBackendEvent. If
	// the device is already open and we haven't sent
	// [audio_context_ready] yet (because the sink wasn't wired the
	// first time), it's shipped here.
	void set_event_sink(AudioEventSink sink);

	// Register a freshly decoded PCM buffer. Returns the assigned
	// [buffer_id] (monotonic from 0). Called from the GL thread by
	// the engine's drain_ready_assets path. Also ships
	// [audio_load_success { audio_url, buffer_id, duration }].
	int32_t register_buffer(std::string audio_url, DecodedAudio buffer);

	// Inverse of [register_buffer]. Stops every voice that's
	// playing this buffer and frees the underlying PCM. The free
	// is deferred until the audio thread has observed the unload
	// command, so no in-flight read references torn-down memory.
	void unregister_buffer(const std::string &audio_url);

	// Ship an [audio_load_failed { audio_url, error }] event. Used
	// by the GL thread when the AssetLoader returns a failed audio
	// decode.
	void emit_load_failed(const std::string &audio_url,
			      AudioDecodeError err);

	// Decode + dispatch an entire AudioCommandBatch. Each action
	// becomes one entry on the command ring. Called from the GL
	// thread. [now_ms] is the same wall-clock OCaml gave for
	// [update]; passed through verbatim so [start_time] semantics
	// line up with JS's [Date.now()].
	bool exec_audio_cmd(const uint8_t *bytes, std::size_t len,
			    double now_ms);

	uint32_t device_sample_rate() const
	{
		return device_sample_rate_;
	}

    private:
	// ---- types ---------------------------------------------------------

	struct Buffer {
		std::string audio_url;
		std::unique_ptr<float[]> samples;
		uint32_t frames = 0;
		uint8_t channels = 2;
		double duration_seconds = 0.0;
	};

	struct TimelinePoint {
		// Absolute time in OCaml-clock ms (per JS audio.js
		// semantics). Converted to device-frame index lazily
		// inside the mixer using the per-cmd ms→frames anchor.
		double time_ms = 0.0;
		float volume = 1.0f;
	};

	struct Voice {
		uint32_t node_group_id = 0;
		Buffer *buffer = nullptr; // not owned

		// Floating cursor in source-frame space. Advances by
		// [playback_rate] per output frame.
		double cursor = 0.0;

		bool loop_enabled = false;
		double loop_start_frames = 0.0; // in source frames
		double loop_end_frames = 0.0;

		float playback_rate = 1.0f;
		float volume = 1.0f;

		// Per-axis volume timelines; product is applied. Times
		// are in absolute OCaml ms and converted to engine
		// frame indices via [time_anchor_*] below.
		std::vector<std::vector<TimelinePoint> > volume_timelines;

		// Anchor pair so we can convert OCaml-ms timeline times
		// into engine-frame indices: when this voice was
		// installed, the GL thread reported [now_ms] and we
		// noted [frames_produced_] at that moment. So:
		//   frame_idx(t_ms) = (t_ms - anchor_ms)
		//                    * sample_rate / 1000 + anchor_frame
		double anchor_ms = 0.0;
		uint64_t anchor_frame = 0;

		bool finished = false;
	};

	enum class CmdKind : uint8_t {
		StartSound,
		StopSound,
		SetVolume,
		SetVolumeAt,
		SetLoopConfig,
		SetPlaybackRate,
		ReleaseBuffer, // carries an owned Buffer to free
	};

	struct Cmd {
		CmdKind kind;
		uint32_t node_group_id = 0;
		Buffer *buffer = nullptr; // borrowed for StartSound

		// Borrowed → owned for ReleaseBuffer. Cmd takes
		// ownership; audio thread drops on apply.
		std::unique_ptr<Buffer> release;

		double now_ms = 0.0;        // OCaml clock at issue time
		uint64_t now_frame = 0;     // engine frame at issue time

		// StartSound payload.
		double start_time_ms = 0.0;
		double start_at_ms = 0.0;
		float volume = 1.0f;
		float playback_rate = 1.0f;
		bool loop_enabled = false;
		double loop_start_ms = 0.0;
		double loop_end_ms = 0.0;
		std::vector<std::vector<TimelinePoint> > timelines;

		// SetVolume / SetPlaybackRate single-value payload.
		// Reuses [volume] and [playback_rate] above respectively.
	};

	// ---- audio-thread entry point -------------------------------------
	static void SDLCALL audio_callback_thunk(void *userdata,
						 SDL_AudioStream *stream,
						 int additional_amount,
						 int total_amount);
	void audio_callback(SDL_AudioStream *stream, int additional_amount);

	// Drain the ring and apply every queued command. Runs on the
	// audio thread.
	void drain_commands();

	void apply_cmd(Cmd &cmd);

	// Mix one voice into [out] for [out_frames] frames. Updates
	// [v.cursor], may set [v.finished].
	void mix_voice(Voice &v, float *out, uint32_t out_frames);

	// Convert an OCaml-clock ms time into an engine frame index
	// using the voice's anchor pair. Returns a [double] so
	// fractional alignment works for fade ramps that span
	// sub-frame boundaries.
	double ms_to_frame(double t_ms, double anchor_ms,
			   uint64_t anchor_frame) const;

	// Sampled timeline gain (product of all timeline gains) at the
	// given engine frame index. Linear interpolation between
	// breakpoints; clamps to the first/last point outside the range.
	float timeline_gain_at(const Voice &v, double frame) const;

	// ---- members ------------------------------------------------------
	SDL_AudioStream *stream_ = nullptr;
	SDL_AudioSpec device_spec_{};
	uint32_t device_sample_rate_ = 0;

	bool ready_event_shipped_ = false;
	AudioEventSink event_sink_;

	// Buffer table. Slots become nullptr after unload but keep
	// their index forever, so [buffer_id]s remain stable from the
	// OCaml side's point of view (matches JS's monotonic
	// [audioBuffers.push] semantics).
	std::vector<std::unique_ptr<Buffer> > buffers_;
	std::unordered_map<std::string, int32_t> by_url_;
	// Both [buffers_] and [by_url_] are touched only by the GL
	// thread, so no mutex protects them.

	// Command ring. Locked because we don't actually need lock-free
	// here — audio commands are issued at most a few times per
	// frame, and the audio thread holds the lock only briefly to
	// move-out the queue under [drain_commands].
	std::mutex cmd_mu_;
	std::vector<Cmd> cmd_queue_;

	// Live voices, audio-thread owned.
	std::unordered_map<uint32_t, Voice> voices_;

	// Engine frame counter, audio-thread-owned. Increments by
	// [out_frames] each callback. Used to anchor time conversions.
	uint64_t frames_produced_ = 0;
};

} // namespace declgl
