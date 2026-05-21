// audio/audio_decoder.cc — WAV (SDL3) + OGG (stb_vorbis) decode +
// device-rate normalization. See header for the contract.

#include "audio/audio_decoder.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

// stb_vorbis ships as a single .c file that both declares and
// defines its API. The implementation is compiled separately
// (third_party/stb/CMakeLists.txt → stb_vorbis target). Including
// the .c with STB_VORBIS_HEADER_ONLY pulls in just the prototypes.
extern "C" {
#define STB_VORBIS_HEADER_ONLY
#include "stb_vorbis.c"
#undef STB_VORBIS_HEADER_ONLY
}

namespace declgl
{

namespace
{

std::string lower_ext(const std::string &path)
{
	auto dot = path.find_last_of('.');
	if (dot == std::string::npos)
		return {};
	std::string e = path.substr(dot + 1);
	std::transform(e.begin(), e.end(), e.begin(),
		       [](unsigned char c) { return std::tolower(c); });
	return e;
}

// Convert (src_spec, src_bytes) → device-rate f32 stereo. Used by both
// the WAV and OGG paths so the resampling/upmix logic lives in one
// place. Returns false on any SDL conversion error; [out] is left
// in its default-constructed state in that case.
bool convert_to_device(const SDL_AudioSpec &src_spec,
                       const Uint8 *src_data, int src_len,
                       uint32_t device_sample_rate, DecodedAudio &out,
                       std::string &err_message)
{
	SDL_AudioSpec dst_spec{};
	dst_spec.format = SDL_AUDIO_F32;
	dst_spec.channels = 2;
	dst_spec.freq = static_cast<int>(device_sample_rate);

	Uint8 *dst_data = nullptr;
	int dst_len = 0;
	if (!SDL_ConvertAudioSamples(&src_spec, src_data, src_len, &dst_spec,
				     &dst_data, &dst_len)) {
		err_message = std::string("SDL_ConvertAudioSamples: ") +
			      SDL_GetError();
		return false;
	}

	const int bytes_per_frame = 2 * static_cast<int>(sizeof(float));
	const uint32_t frames =
		static_cast<uint32_t>(dst_len / bytes_per_frame);

	// SDL allocates the output buffer with SDL_malloc; we copy into a
	// std::unique_ptr<float[]> (operator new[]) so the rest of the
	// engine can free it normally on shutdown without dragging
	// SDL_free into Texture/Audio destructors.
	auto pcm = std::unique_ptr<float[]>(new float[frames * 2]);
	std::memcpy(pcm.get(), dst_data,
		    static_cast<size_t>(frames) * 2 * sizeof(float));
	SDL_free(dst_data);

	out.samples = std::move(pcm);
	out.frames = frames;
	out.channels = 2;
	out.sample_rate = device_sample_rate;
	return true;
}

DecodedAudio decode_wav(const std::string &path, uint32_t device_sample_rate,
                        AudioDecodeError &err, std::string &err_message)
{
	SDL_AudioSpec src_spec{};
	Uint8 *src_data = nullptr;
	Uint32 src_len = 0;
	if (!SDL_LoadWAV(path.c_str(), &src_spec, &src_data, &src_len)) {
		err = AudioDecodeError::DecodeFailure;
		err_message = std::string("SDL_LoadWAV: ") + SDL_GetError();
		return {};
	}

	DecodedAudio out;
	const bool ok = convert_to_device(src_spec, src_data,
					  static_cast<int>(src_len),
					  device_sample_rate, out, err_message);
	SDL_free(src_data);
	if (!ok) {
		err = AudioDecodeError::DecodeFailure;
		return {};
	}
	err = AudioDecodeError::None;
	return out;
}

DecodedAudio decode_ogg(const std::string &path, uint32_t device_sample_rate,
                        AudioDecodeError &err, std::string &err_message)
{
	int channels = 0;
	int sample_rate = 0;
	short *interleaved = nullptr;
	const int frames = stb_vorbis_decode_filename(
		path.c_str(), &channels, &sample_rate, &interleaved);
	if (frames <= 0 || !interleaved || channels < 1 || channels > 8 ||
	    sample_rate <= 0) {
		if (interleaved)
			std::free(interleaved);
		// stb_vorbis returns -1 on every failure mode — we can't
		// distinguish "file missing" from "bad data" here, so we
		// classify as DecodeFailure. The IO-failure branch is
		// preserved for future use (e.g. a streaming variant).
		err = AudioDecodeError::DecodeFailure;
		err_message = "stb_vorbis_decode_filename failed";
		return {};
	}

	// Wrap as an SDL_AudioSpec and let SDL handle resample +
	// channel-count change. stb_vorbis's pulldata int helper outputs
	// 16-bit signed samples — match that in src_spec.
	SDL_AudioSpec src_spec{};
	src_spec.format = SDL_AUDIO_S16;
	src_spec.channels = channels;
	src_spec.freq = sample_rate;

	const int src_len = frames * channels * static_cast<int>(sizeof(short));

	DecodedAudio out;
	const bool ok = convert_to_device(src_spec,
					  reinterpret_cast<Uint8 *>(interleaved),
					  src_len, device_sample_rate, out,
					  err_message);
	std::free(interleaved);
	if (!ok) {
		err = AudioDecodeError::DecodeFailure;
		return {};
	}
	err = AudioDecodeError::None;
	return out;
}

} // namespace

DecodedAudio decode_audio_file(const std::string &path,
                               uint32_t device_sample_rate,
                               AudioDecodeError &err, std::string &err_message)
{
	err = AudioDecodeError::None;
	err_message.clear();

	if (device_sample_rate == 0) {
		err = AudioDecodeError::DecodeFailure;
		err_message = "device_sample_rate=0";
		return {};
	}

	const std::string ext = lower_ext(path);
	if (ext == "wav") {
		return decode_wav(path, device_sample_rate, err, err_message);
	}
	if (ext == "ogg") {
		return decode_ogg(path, device_sample_rate, err, err_message);
	}
	err = AudioDecodeError::UnsupportedFormat;
	err_message = "unsupported audio extension: ." + ext;
	return {};
}

} // namespace declgl
