// resources/asset_loader.cc — async file decode for textures + fonts.
//
// See [asset_loader.h] for the threading model.

#include "resources/asset_loader.h"

#include <cstdio>
#include <utility>

#include "audio/audio_decoder.h"
#include "resources/font.h"
#include "resources/image_decoder.h"

namespace declgl
{

AssetLoader::AssetLoader()
{
	worker_ = std::thread([this] { worker_main(); });
}

AssetLoader::~AssetLoader()
{
	stop();
}

void AssetLoader::enqueue(DecodeJob job)
{
	{
		std::lock_guard<std::mutex> lk(mu_);
		decode_queue_.push_back(std::move(job));
	}
	cv_.notify_one();
}

std::size_t AssetLoader::drain_ready(std::vector<ReadyAsset> &out,
				     std::size_t max_items)
{
	if (max_items == 0)
		return 0;
	std::size_t moved = 0;
	std::lock_guard<std::mutex> lk(mu_);
	while (moved < max_items && !ready_queue_.empty()) {
		out.push_back(std::move(ready_queue_.front()));
		ready_queue_.pop_front();
		++moved;
	}
	return moved;
}

void AssetLoader::cancel_pending(AssetKind kind, std::string_view name)
{
	const std::string n(name);
	std::lock_guard<std::mutex> lk(mu_);
	// Drop everything queued but not yet decoded.
	for (auto it = decode_queue_.begin(); it != decode_queue_.end();) {
		if (it->kind == kind && it->name == n) {
			it = decode_queue_.erase(it);
		} else {
			++it;
		}
	}
	// Drop everything decoded but not yet uploaded. This catches the
	// race where the worker just finished a load right before the
	// unload arrived.
	for (auto it = ready_queue_.begin(); it != ready_queue_.end();) {
		if (it->kind == kind && it->name == n) {
			it = ready_queue_.erase(it);
		} else {
			++it;
		}
	}
}

void AssetLoader::stop()
{
	if (!worker_.joinable())
		return;
	{
		std::lock_guard<std::mutex> lk(mu_);
		stop_.store(true, std::memory_order_release);
	}
	cv_.notify_all();
	worker_.join();
}

void AssetLoader::worker_main()
{
	for (;;) {
		DecodeJob job;
		{
			std::unique_lock<std::mutex> lk(mu_);
			cv_.wait(lk, [this] {
				return stop_.load(std::memory_order_acquire) ||
				       !decode_queue_.empty();
			});
			if (stop_.load(std::memory_order_acquire) &&
			    decode_queue_.empty()) {
				return;
			}
			job = std::move(decode_queue_.front());
			decode_queue_.pop_front();
		}

		ReadyAsset ready;
		process(job, ready);

		{
			std::lock_guard<std::mutex> lk(mu_);
			ready_queue_.push_back(std::move(ready));
		}
		// No notify here — the GL thread polls per-frame via
		// [drain_ready], it doesn't wait on the condvar.
	}
}

namespace
{

// Premultiply an RGBA8 buffer in-place (rounded). Bit-equivalent to
// the helper in [texture.cc]; we duplicate it here so the worker
// thread doesn't have to take a dependency on Texture (which would
// drag GL headers into a non-GL TU).
void premultiply_rgba_inplace(uint8_t *buf, std::size_t n_pixels)
{
	const std::size_t total = n_pixels * 4;
	for (std::size_t i = 0; i < total; i += 4) {
		const uint8_t a = buf[i + 3];
		buf[i + 0] = static_cast<uint8_t>((buf[i + 0] * a + 127) / 255);
		buf[i + 1] = static_cast<uint8_t>((buf[i + 1] * a + 127) / 255);
		buf[i + 2] = static_cast<uint8_t>((buf[i + 2] * a + 127) / 255);
	}
}

// Read a file into a std::string. Used for BMFont JSON. Returns false
// on any I/O error; the caller fills [error] accordingly.
bool slurp_file(const std::string &path, std::string &out, std::string &err)
{
	FILE *fp = std::fopen(path.c_str(), "rb");
	if (!fp) {
		err = "fopen(" + path + ")";
		return false;
	}
	std::fseek(fp, 0, SEEK_END);
	const long n = std::ftell(fp);
	std::fseek(fp, 0, SEEK_SET);
	if (n < 0) {
		std::fclose(fp);
		err = "ftell(" + path + ")";
		return false;
	}
	out.resize(static_cast<std::size_t>(n));
	const std::size_t r = std::fread(out.data(), 1, out.size(), fp);
	std::fclose(fp);
	if (r != out.size()) {
		err = "fread(" + path + ") short read";
		return false;
	}
	return true;
}

} // namespace

void AssetLoader::process(DecodeJob &job, ReadyAsset &out)
{
	out.kind = job.kind;
	out.name = std::move(job.name);
	out.image_url = job.image_url;
	out.min_filter_enum = job.min_filter_enum;
	out.mag_filter_enum = job.mag_filter_enum;
	out.premultiply_alpha = job.premultiply_alpha;

	if (job.kind == AssetKind::Texture) {
		DecodedImage img = decode_image_file(job.image_url, job.crop);
		if (!img.ok()) {
			out.error = "decode_image_file";
			return;
		}
		if (job.premultiply_alpha) {
			premultiply_rgba_inplace(
				img.pixels.get(),
				static_cast<std::size_t>(img.width) *
					static_cast<std::size_t>(img.height));
		}
		out.image = std::move(img);
		return;
	}

	if (job.kind == AssetKind::Audio) {
		// Audio uses [image_url] as the file path (see DecodeJob
		// docstring). We rely on the engine to set
		// [audio_sample_rate] before enqueueing — if it didn't,
		// the decoder fails fast with a clear message.
		AudioDecodeError err = AudioDecodeError::None;
		std::string err_msg;
		out.audio = decode_audio_file(
			job.image_url, job.audio_sample_rate, err, err_msg);
		if (!out.audio.ok()) {
			out.audio_error = err;
			out.error = err_msg.empty() ? "decode_audio_file" :
						      err_msg;
			return;
		}
		return;
	}

	// ---- Font ----
	// SDF atlases must NOT be premultiplied. The job builder already
	// forces this to false; we re-assert defensively so a future
	// refactor can't silently corrupt SDF data.
	out.premultiply_alpha = false;

	std::string json_bytes;
	{
		std::string err;
		if (!slurp_file(job.json_url, json_bytes, err)) {
			out.error = std::move(err);
			return;
		}
	}

	auto font = std::make_unique<Font>();
	if (!font->parse(json_bytes.data(), json_bytes.size())) {
		out.error = "Font::parse: " + font->error();
		return;
	}

	DecodedImage img = decode_image_file(job.image_url, ImageCrop{});
	if (!img.ok()) {
		out.error = "decode_image_file(atlas)";
		return;
	}
	// (intentionally skip premultiply for fonts — see SDF note above)

	out.font = std::move(font);
	out.image = std::move(img);
}

} // namespace declgl
