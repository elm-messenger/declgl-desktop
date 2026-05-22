#pragma once

// audio/audio_decoder.h — file → device-rate f32 stereo PCM.
//
// Both supported formats (WAV via SDL3 builtin, OGG Vorbis via the
// vendored stb_vorbis) are decoded *off* the audio thread and *off*
// the GL thread, on the existing AssetLoader worker. The output is
// always normalized to:
//
//   - sample format: SDL_AUDIO_F32 (32-bit float, native endian)
//   - channels:      2 (stereo; mono sources are upmixed by duplication)
//   - sample rate:   the device sample rate selected by AudioEngine
//
// This means the runtime mixer never has to do device-rate resampling
// or channel expansion per voice. Per-voice playback_rate is still
// honoured at mix time as a fractional read cursor (linear lerp), the
// way Web Audio's AudioBufferSourceNode.playbackRate works.
//
// Format dispatch is by (lower-cased) file extension. Anything other
// than `.wav` or `.ogg` returns an empty buffer; the engine surfaces
// that as AudioLoadFailed { AUDIO_LOAD_ERROR_FAILED_TO_DECODE }.

#include <cstdint>
#include <memory>
#include <string>

namespace declgl
{

// Owned PCM buffer. [samples] holds [frames * channels] interleaved
// floats; [channels] is always 2 for buffers produced by this module
// (kept as a field for future-proofing). [sample_rate] is the device
// rate the buffer was normalized to — always equal to the rate passed
// to [decode_audio_file] on success.
struct DecodedAudio {
	std::unique_ptr<float[]> samples;
	uint32_t frames = 0;
	uint8_t channels = 2;
	uint32_t sample_rate = 0;

	bool ok() const
	{
		return samples != nullptr && frames > 0;
	}

	double duration_seconds() const
	{
		return sample_rate > 0 ?
			       static_cast<double>(frames) / sample_rate :
			       0.0;
	}
};

// Decoder error categories. Mirrors the proto's [AudioLoadError] so
// callers can surface the right variant without re-classifying.
enum class AudioDecodeError {
	None = 0,
	IoFailure, // file missing / unreadable
	UnsupportedFormat, // extension or magic isn't WAV/OGG
	DecodeFailure, // parser rejected the bytes
};

// Decode [path] (WAV or OGG Vorbis) and resample to f32 stereo at
// [device_sample_rate]. On any failure returns a default-constructed
// DecodedAudio (.ok() == false) and writes the reason to [err]; a
// short human-readable message is also written to [err_message] for
// logging.
DecodedAudio decode_audio_file(const std::string &path,
			       uint32_t device_sample_rate,
			       AudioDecodeError &err, std::string &err_message);

} // namespace declgl
