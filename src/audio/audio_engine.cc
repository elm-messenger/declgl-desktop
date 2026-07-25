// audio/audio_engine.cc — see header for the full design.
//
// Implementation notes
// --------------------
// * Output format is fixed: f32 stereo at the device's sample rate.
//   The decoder normalizes every loaded buffer to that shape, so the
//   mixer only has to handle per-voice playback_rate (a fractional
//   read cursor with linear interpolation) and gain.
//
// * Time conversion: every command remembers the OCaml-clock ms and
//   the engine-frame counter at issue time. That pair is the anchor
//   used to project any later [t_ms] (volume timeline points,
//   start_time, etc) into engine-frame space. Since
//   start_time/now_ms come from the same OCaml clock, the relative
//   offset converts cleanly without any drift correction.
//
// * SDL_OpenAudioDeviceStream pulls samples on demand by calling our
//   callback. We respond with [additional_amount] bytes worth of
//   freshly mixed audio. The callback runs on a dedicated SDL thread
//   that NEVER calls into OCaml.

#include "audio/audio_engine.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <utility>

#include "log/log.h"
#include "transport_audio.pb.h"

namespace declgl
{

namespace
{

// AUDIO_LOAD_ERROR_FAILED_TO_DECODE / AUDIO_LOAD_ERROR_NETWORK /
// AUDIO_LOAD_ERROR_UNKNOWN. Keeps the proto<->engine mapping in one
// place so future error categories don't drift.
mlregl::transport::audio::AudioLoadError to_proto_error(AudioDecodeError e)
{
	using P = mlregl::transport::audio::AudioLoadError;
	switch (e) {
	case AudioDecodeError::IoFailure:
		return P::AUDIO_LOAD_ERROR_NETWORK;
	case AudioDecodeError::DecodeFailure:
	case AudioDecodeError::UnsupportedFormat:
		return P::AUDIO_LOAD_ERROR_FAILED_TO_DECODE;
	default:
		return P::AUDIO_LOAD_ERROR_UNKNOWN;
	}
}

void ship(AudioEventSink &sink,
	  const mlregl::transport::audio::AudioBackendEvent &ev)
{
	if (!sink) {
		DECLGL_LOG_WARN(
			"AudioBackendEvent dropped: no event_sink registered");
		return;
	}
	std::string buf;
	if (!ev.SerializeToString(&buf)) {
		DECLGL_LOG_ERROR("AudioBackendEvent serialize failed");
		return;
	}
	sink(reinterpret_cast<const uint8_t *>(buf.data()), buf.size());
}

// audio.js's [interpolate], slightly cleaner: linear blend between
// the two breakpoints surrounding [t]. Caller guarantees
// [a_t < t <= b_t]; degenerate ranges (b_t == a_t) collapse to b.
inline float lerp_volume(double a_t, float a_v, double b_t, float b_v, double t)
{
	const double span = b_t - a_t;
	if (!(span > 0.0))
		return b_v;
	const double k = (t - a_t) / span;
	return static_cast<float>(a_v + k * (b_v - a_v));
}

} // namespace

AudioEngine::AudioEngine() = default;

AudioEngine::~AudioEngine()
{
	if (stream_) {
		SDL_DestroyAudioStream(stream_);
		stream_ = nullptr;
	}
}

void AudioEngine::set_event_sink(AudioEventSink sink)
{
	event_sink_ = std::move(sink);

	// If we already opened the device but couldn't ship the ready
	// event (sink wasn't wired yet), do it now.
	if (device_sample_rate_ > 0 && !ready_event_shipped_ && event_sink_) {
		mlregl::transport::audio::AudioBackendEvent ev;
		ev.mutable_audio_context_ready()->set_sample_rate(
			device_sample_rate_);
		ship(event_sink_, ev);
		ready_event_shipped_ = true;
	}
}

bool AudioEngine::ensure_open()
{
	if (stream_)
		return true;

	// SDL_INIT_AUDIO is already called in [Engine::init_window_and_gl];
	// guard with SDL_InitSubSystem so this works whether the engine
	// was inited yet or not.
	if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
		DECLGL_LOG_WARN("SDL_InitSubSystem(AUDIO): {}", SDL_GetError());
		return false;
	}

	// Request stereo f32 at a sane default; SDL will negotiate the
	// device's preferred sample rate. We learn the actual rate from
	// SDL_GetAudioStreamFormat after open.
	SDL_AudioSpec want{};
	want.format = SDL_AUDIO_F32;
	want.channels = 2;
	want.freq = 48000;

	stream_ = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK,
					    &want, &audio_callback_thunk, this);
	if (!stream_) {
		DECLGL_LOG_WARN("SDL_OpenAudioDeviceStream: {}",
				SDL_GetError());
		return false;
	}

	// Discover the negotiated rate. We mix and resample to this rate
	// at decode time, so it's important to read exactly what the
	// stream's source-side format ended up at.
	SDL_AudioSpec src_spec{};
	SDL_AudioSpec dst_spec{};
	if (!SDL_GetAudioStreamFormat(stream_, &src_spec, &dst_spec)) {
		DECLGL_LOG_WARN("SDL_GetAudioStreamFormat: {}", SDL_GetError());
	}
	device_spec_ = src_spec; // input side — what we feed in
	device_sample_rate_ = src_spec.freq > 0 ?
				      static_cast<uint32_t>(src_spec.freq) :
				      48000;

	DECLGL_LOG_INFO("device opened: src {} Hz x {} ch (we feed {}) "
			"-> dst {} Hz x {} ch",
			src_spec.freq, src_spec.channels,
			src_spec.format == SDL_AUDIO_F32 ? "f32" : "?",
			dst_spec.freq, dst_spec.channels);

	// SDL streams are paused at creation; resume so the callback fires.
	SDL_ResumeAudioStreamDevice(stream_);

	if (!ready_event_shipped_ && event_sink_) {
		mlregl::transport::audio::AudioBackendEvent ev;
		ev.mutable_audio_context_ready()->set_sample_rate(
			device_sample_rate_);
		ship(event_sink_, ev);
		ready_event_shipped_ = true;
	}
	return true;
}

int32_t AudioEngine::register_buffer(std::string audio_url, DecodedAudio buffer)
{
	if (!buffer.ok()) {
		DECLGL_LOG_ERROR("register_buffer: empty buffer for '{}'",
				 audio_url);
		return -1;
	}

	// Reuse a vacated slot if any, otherwise append. JS's audio.js
	// always appends (buffer_ids never come back), but we have
	// UnloadAudio so we *can* recycle. Recycling keeps [buffer_id]s
	// dense which makes the audio-thread vector lookup cheaper.
	auto buf = std::make_unique<Buffer>();
	buf->audio_url = audio_url;
	buf->frames = buffer.frames;
	buf->channels = buffer.channels;
	buf->samples = std::move(buffer.samples);
	buf->duration_seconds = buffer.duration_seconds();

	int32_t id = -1;
	for (size_t i = 0; i < buffers_.size(); ++i) {
		if (!buffers_[i]) {
			id = static_cast<int32_t>(i);
			buffers_[i] = std::move(buf);
			break;
		}
	}
	if (id < 0) {
		id = static_cast<int32_t>(buffers_.size());
		buffers_.push_back(std::move(buf));
	}
	by_url_[audio_url] = id;
	DECLGL_LOG_INFO("audio loaded: url={} buffer_id={} duration={:.3f}s",
			 audio_url, id, buffers_[id]->duration_seconds);

	mlregl::transport::audio::AudioBackendEvent ev;
	auto *ok = ev.mutable_audio_load_success();
	ok->set_audio_url(audio_url);
	ok->set_buffer_id(static_cast<uint32_t>(id));
	ok->set_duration(buffers_[id]->duration_seconds);
	ship(event_sink_, ev);
	return id;
}

void AudioEngine::unregister_buffer(const std::string &audio_url)
{
	auto it = by_url_.find(audio_url);
	if (it == by_url_.end())
		return;
	const int32_t id = it->second;
	by_url_.erase(it);

	if (id < 0 || static_cast<size_t>(id) >= buffers_.size())
		return;
	auto buf = std::move(buffers_[id]); // slot becomes nullptr
	if (!buf)
		return;

	// Hand ownership to the audio thread. It will:
	//  1. Drop any voice still pointing at [buf.get()]
	//  2. Free the unique_ptr at end of [apply_cmd]
	// Until that happens the buffer remains valid for any in-
	// flight read.
	Cmd c;
	c.kind = CmdKind::ReleaseBuffer;
	c.release = std::move(buf);
	{
		std::lock_guard<std::mutex> lk(cmd_mu_);
		cmd_queue_.push_back(std::move(c));
	}
}

void AudioEngine::emit_load_failed(const std::string &audio_url,
				   AudioDecodeError err)
{
	mlregl::transport::audio::AudioBackendEvent ev;
	auto *fail = ev.mutable_audio_load_failed();
	fail->set_audio_url(audio_url);
	fail->set_error(to_proto_error(err));
	ship(event_sink_, ev);
}

bool AudioEngine::exec_audio_cmd(const uint8_t *bytes, std::size_t len,
				 double now_ms)
{
	using namespace mlregl::transport::audio;
	AudioCommandBatch batch;
	if (!batch.ParseFromArray(bytes, static_cast<int>(len))) {
		DECLGL_LOG_ERROR("AudioCommandBatch parse failed");
		return false;
	}

	// Lazy device open on first action. Mirrors JS's [ensureContext]
	// which is also called from inside [applyAudioCommandBatch].
	if (!stream_) {
		ensure_open();
	}

	// Snapshot current frame counter while we hold cmd_mu_, then
	// stamp every command with the same anchor pair.
	std::vector<Cmd> staged;
	staged.reserve(batch.actions_size());

	for (const auto &act : batch.actions()) {
		Cmd c;
		c.now_ms = now_ms;
		c.now_frame = 0; // patched under cmd_mu_

		switch (act.kind_case()) {
		case AudioAction::kStartSound: {
			const auto &s = act.start_sound();
			c.kind = CmdKind::StartSound;
			c.node_group_id = s.node_group_id();
			const uint32_t bid = s.buffer_id();
			if (bid >= buffers_.size() || !buffers_[bid]) {
				DECLGL_LOG_ERROR(
					"start_sound: unknown buffer_id={} (group={})",
					bid, c.node_group_id);
				continue;
			}
			c.buffer = buffers_[bid].get();
			c.start_time_ms = s.start_time();
			c.start_at_ms = s.start_at();
			c.volume = static_cast<float>(s.volume());
			c.playback_rate =
				s.playback_rate() != 0.0 ?
					static_cast<float>(s.playback_rate()) :
					1.0f;
			if (s.has_loop()) {
				c.loop_enabled = true;
				c.loop_start_ms = s.loop().loop_start();
				c.loop_end_ms = s.loop().loop_end();
			}
			c.timelines.reserve(s.volume_timelines_size());
			for (const auto &tl : s.volume_timelines()) {
				std::vector<TimelinePoint> pts;
				pts.reserve(tl.points_size());
				for (const auto &p : tl.points()) {
					pts.push_back({ p.time(),
							static_cast<float>(
								p.volume()) });
				}
				c.timelines.push_back(std::move(pts));
			}
			DECLGL_LOG_INFO(
				"start_sound: group={} buffer_id={} start_time={:.3f} now={:.3f}",
				c.node_group_id, bid, c.start_time_ms, c.now_ms);
			break;
		}
		case AudioAction::kStopSound:
			c.kind = CmdKind::StopSound;
			c.node_group_id = act.stop_sound().node_group_id();
			break;
		case AudioAction::kSetVolume:
			c.kind = CmdKind::SetVolume;
			c.node_group_id = act.set_volume().node_group_id();
			c.volume =
				static_cast<float>(act.set_volume().volume());
			break;
		case AudioAction::kSetVolumeAt: {
			c.kind = CmdKind::SetVolumeAt;
			c.node_group_id = act.set_volume_at().node_group_id();
			const auto &a = act.set_volume_at();
			c.timelines.reserve(a.volume_at_size());
			for (const auto &tl : a.volume_at()) {
				std::vector<TimelinePoint> pts;
				pts.reserve(tl.points_size());
				for (const auto &p : tl.points()) {
					pts.push_back({ p.time(),
							static_cast<float>(
								p.volume()) });
				}
				c.timelines.push_back(std::move(pts));
			}
			break;
		}
		case AudioAction::kSetLoopConfig:
			c.kind = CmdKind::SetLoopConfig;
			c.node_group_id = act.set_loop_config().node_group_id();
			if (act.set_loop_config().has_loop()) {
				c.loop_enabled = true;
				c.loop_start_ms = act.set_loop_config()
							  .loop()
							  .loop_start();
				c.loop_end_ms =
					act.set_loop_config().loop().loop_end();
			} else {
				c.loop_enabled = false;
			}
			break;
		case AudioAction::kSetPlaybackRate:
			c.kind = CmdKind::SetPlaybackRate;
			c.node_group_id =
				act.set_playback_rate().node_group_id();
			c.playback_rate = static_cast<float>(
				act.set_playback_rate().playback_rate());
			break;
		case AudioAction::KIND_NOT_SET:
		default:
			continue;
		}
		staged.push_back(std::move(c));
	}

	if (staged.empty())
		return true;

	{
		std::lock_guard<std::mutex> lk(cmd_mu_);
		// Stamp now_frame at enqueue time. We don't know the
		// audio-thread frame counter precisely from here (race
		// with the callback), but [frames_produced_] is only
		// monotonically increasing and read-during-callback /
		// written-during-callback, so reading it under cmd_mu_
		// gives us a tight lower bound. Any voice anchored at
		// an earlier frame than the audio thread thinks is
		// "now" is fine: the timeline math uses a *difference*
		// between two device frames, and that difference is
		// preserved.
		const uint64_t now_frame = frames_produced_;
		for (auto &c : staged)
			c.now_frame = now_frame;
		cmd_queue_.insert(cmd_queue_.end(),
				  std::make_move_iterator(staged.begin()),
				  std::make_move_iterator(staged.end()));
	}
	return true;
}

// --------------------------------------------------------------------
// Audio thread
// --------------------------------------------------------------------

void SDLCALL AudioEngine::audio_callback_thunk(void *userdata,
					       SDL_AudioStream *stream,
					       int additional_amount,
					       int /*total_amount*/)
{
	auto *self = static_cast<AudioEngine *>(userdata);
	self->audio_callback(stream, additional_amount);
}

void AudioEngine::audio_callback(SDL_AudioStream *stream, int additional_amount)
{
	if (additional_amount <= 0)
		return;

	drain_commands();

	// SDL gives us "additional bytes". We feed in chunks bounded so
	// the temp buffer doesn't balloon.
	constexpr int kMaxChunkBytes = 64 * 1024; // ~16k stereo frames
	const int bytes_per_frame = 2 * static_cast<int>(sizeof(float));

	int remaining = additional_amount;
	float chunk[kMaxChunkBytes / sizeof(float)];

	while (remaining > 0) {
		const int chunk_bytes = std::min(remaining, kMaxChunkBytes);
		const int chunk_frames = chunk_bytes / bytes_per_frame;
		if (chunk_frames <= 0)
			break;

		std::memset(chunk, 0,
			    static_cast<size_t>(chunk_frames) *
				    bytes_per_frame);

		// Mix every live voice into the chunk.
		for (auto it = voices_.begin(); it != voices_.end();) {
			Voice &v = it->second;
			mix_voice(v, chunk,
				  static_cast<uint32_t>(chunk_frames));
			if (v.finished) {
				it = voices_.erase(it);
			} else {
				++it;
			}
		}

		SDL_PutAudioStreamData(stream, chunk,
				       chunk_frames * bytes_per_frame);
		frames_produced_ += static_cast<uint64_t>(chunk_frames);
		remaining -= chunk_frames * bytes_per_frame;
	}
}

void AudioEngine::drain_commands()
{
	std::vector<Cmd> local;
	{
		std::lock_guard<std::mutex> lk(cmd_mu_);
		local.swap(cmd_queue_);
	}
	for (auto &c : local) {
		apply_cmd(c);
	}
	// [local]'s destructor frees any ReleaseBuffer payloads here —
	// safe because we just finished applying every voice tear-down
	// they implied above (apply_cmd nulls dangling voice pointers).
}

void AudioEngine::apply_cmd(Cmd &cmd)
{
	switch (cmd.kind) {
	case CmdKind::StartSound: {
		// Replace any existing voice on this group_id (matches
		// JS, which would just overwrite [audioPlaying[groupId]]).
		Voice v;
		v.node_group_id = cmd.node_group_id;
		v.buffer = cmd.buffer;
		v.playback_rate = cmd.playback_rate;
		v.volume = cmd.volume;
		v.volume_timelines = std::move(cmd.timelines);
		v.anchor_ms = cmd.now_ms;
		v.anchor_frame = cmd.now_frame;

		// Initial cursor: [start_time] is when the source
		// should "have started", in OCaml-clock ms; [start_at]
		// is the offset into the source. Mirroring audio.js's
		// [playSound] start logic but mapped to a frame index.
		const double sr = static_cast<double>(device_sample_rate_);
		const double start_at_frames = cmd.start_at_ms * sr / 1000.0;
		if (cmd.start_time_ms >= cmd.now_ms) {
			// Future start: cursor begins at start_at; the
			// voice will silently advance until start_time.
			// We approximate by setting cursor at the
			// playback offset and skipping mix-in for
			// frames before [start_time]. We track that via
			// a dedicated anchor — see mix_voice.
			v.cursor = start_at_frames;
			// Encode the future start as a negative anchor
			// offset: we won't render until
			// frames_produced_ >= start_frame.
			v.anchor_ms = cmd.start_time_ms;
			v.anchor_frame =
				cmd.now_frame +
				static_cast<uint64_t>(
					(cmd.start_time_ms - cmd.now_ms) * sr /
					1000.0);
		} else {
			// Past start: skip ahead in the source by
			// (now - start_time) seconds + start_at.
			const double late_seconds =
				(cmd.now_ms - cmd.start_time_ms) / 1000.0;
			v.cursor = start_at_frames + late_seconds * sr;
		}

		if (cmd.loop_enabled) {
			v.loop_enabled = true;
			v.loop_start_frames = cmd.loop_start_ms * sr / 1000.0;
			v.loop_end_frames = cmd.loop_end_ms * sr / 1000.0;
			if (v.loop_end_frames <= v.loop_start_frames) {
				v.loop_enabled = false; // sanity
			}
		}

		voices_[cmd.node_group_id] = std::move(v);
		break;
	}
	case CmdKind::StopSound:
		voices_.erase(cmd.node_group_id);
		break;
	case CmdKind::SetVolume: {
		auto it = voices_.find(cmd.node_group_id);
		if (it != voices_.end())
			it->second.volume = cmd.volume;
		break;
	}
	case CmdKind::SetVolumeAt: {
		auto it = voices_.find(cmd.node_group_id);
		if (it == voices_.end())
			break;
		// Re-anchor: the new timeline's times are in OCaml-clock
		// ms, just like the original voice's timeline. Use the
		// command-issue anchor so points line up.
		it->second.anchor_ms = cmd.now_ms;
		it->second.anchor_frame = cmd.now_frame;
		it->second.volume_timelines = std::move(cmd.timelines);
		break;
	}
	case CmdKind::SetLoopConfig: {
		auto it = voices_.find(cmd.node_group_id);
		if (it == voices_.end())
			break;
		const double sr = static_cast<double>(device_sample_rate_);
		if (cmd.loop_enabled) {
			it->second.loop_enabled = true;
			it->second.loop_start_frames =
				cmd.loop_start_ms * sr / 1000.0;
			it->second.loop_end_frames =
				cmd.loop_end_ms * sr / 1000.0;
			if (it->second.loop_end_frames <=
			    it->second.loop_start_frames) {
				it->second.loop_enabled = false;
			}
		} else {
			it->second.loop_enabled = false;
		}
		break;
	}
	case CmdKind::SetPlaybackRate: {
		auto it = voices_.find(cmd.node_group_id);
		if (it != voices_.end())
			it->second.playback_rate = cmd.playback_rate;
		break;
	}
	case CmdKind::ReleaseBuffer: {
		// Tear down every voice still pointing at [release.get()]
		// before we drop the unique_ptr (which actually frees
		// memory at end of switch).
		Buffer *target = cmd.release.get();
		for (auto it = voices_.begin(); it != voices_.end();) {
			if (it->second.buffer == target) {
				it = voices_.erase(it);
			} else {
				++it;
			}
		}
		// cmd.release goes out of scope at the next iteration of
		// drain_commands' loop, freeing the PCM. Up to that
		// point voices have been cleared so no read can race.
		break;
	}
	}
}

double AudioEngine::ms_to_frame(double t_ms, double anchor_ms,
				uint64_t anchor_frame) const
{
	const double sr = static_cast<double>(device_sample_rate_);
	return (t_ms - anchor_ms) * sr / 1000.0 +
	       static_cast<double>(anchor_frame);
}

float AudioEngine::timeline_gain_at(const Voice &v, double frame) const
{
	float prod = 1.0f;
	for (const auto &tl : v.volume_timelines) {
		if (tl.empty())
			continue;
		// Convert the timeline's ms breakpoints to engine-frames
		// on the fly. Cheap (a couple muls + adds per point), and
		// keeps voices migration-friendly if we ever want to
		// re-anchor.
		const double f0 = ms_to_frame(tl.front().time_ms, v.anchor_ms,
					      v.anchor_frame);
		const double fN = ms_to_frame(tl.back().time_ms, v.anchor_ms,
					      v.anchor_frame);
		if (frame <= f0) {
			prod *= tl.front().volume;
			continue;
		}
		if (frame >= fN) {
			prod *= tl.back().volume;
			continue;
		}
		// Linear search; timelines are short (a handful of
		// breakpoints in practice) so this is fine.
		for (size_t i = 1; i < tl.size(); ++i) {
			const double a = ms_to_frame(
				tl[i - 1].time_ms, v.anchor_ms, v.anchor_frame);
			const double b = ms_to_frame(tl[i].time_ms, v.anchor_ms,
						     v.anchor_frame);
			if (frame <= b) {
				prod *= lerp_volume(a, tl[i - 1].volume, b,
						    tl[i].volume, frame);
				break;
			}
		}
	}
	return prod;
}

void AudioEngine::mix_voice(Voice &v, float *out, uint32_t out_frames)
{
	if (v.finished || !v.buffer || v.buffer->frames == 0)
		return;

	const uint32_t src_frames = v.buffer->frames;
	const float *src = v.buffer->samples.get();
	const uint8_t src_ch = v.buffer->channels; // always 2 today
	const float playback_rate = v.playback_rate;

	// Future start handling: if the voice's anchor frame is
	// in the future (set by StartSound when start_time > now),
	// skip silently until we catch up. We render whatever portion
	// of [out_frames] falls at-or-after the anchor.
	uint32_t skip_frames = 0;
	if (v.anchor_frame > frames_produced_) {
		const uint64_t gap = v.anchor_frame - frames_produced_;
		if (gap >= out_frames)
			return; // entire chunk is pre-start
		skip_frames = static_cast<uint32_t>(gap);
	}

	for (uint32_t i = skip_frames; i < out_frames; ++i) {
		// Loop wrap: when [cursor] crosses [loop_end] and looping
		// is on, wrap back to [loop_start]. JS's
		// AudioBufferSourceNode does this in hardware; we do the
		// equivalent of [source.loop = true; source.loopEnd = ...].
		if (v.loop_enabled) {
			while (v.cursor >= v.loop_end_frames) {
				v.cursor -= (v.loop_end_frames -
					     v.loop_start_frames);
			}
		}

		// End-of-source for non-looping voices.
		if (!v.loop_enabled && v.cursor >= src_frames) {
			v.finished = true;
			break;
		}

		// Linear interpolation between adjacent frames.
		double cur = v.cursor;
		if (cur < 0.0)
			cur = 0.0;
		uint32_t i0 = static_cast<uint32_t>(cur);
		uint32_t i1 = i0 + 1;
		if (i1 >= src_frames) {
			i1 = v.loop_enabled ? static_cast<uint32_t>(
						      v.loop_start_frames) :
					      i0;
		}
		const double frac = cur - static_cast<double>(i0);

		float l, r;
		if (src_ch == 2) {
			const float l0 = src[i0 * 2 + 0];
			const float r0 = src[i0 * 2 + 1];
			const float l1 = src[i1 * 2 + 0];
			const float r1 = src[i1 * 2 + 1];
			l = static_cast<float>(l0 + (l1 - l0) * frac);
			r = static_cast<float>(r0 + (r1 - r0) * frac);
		} else {
			// Mono fallback (should not happen post-decoder
			// but kept defensive).
			const float s0 = src[i0];
			const float s1 = src[i1];
			l = r = static_cast<float>(s0 + (s1 - s0) * frac);
		}

		const double abs_frame = static_cast<double>(frames_produced_) +
					 static_cast<double>(i);
		const float g = v.volume * timeline_gain_at(v, abs_frame);

		out[i * 2 + 0] += l * g;
		out[i * 2 + 1] += r * g;

		v.cursor += playback_rate;
	}
}

} // namespace declgl
