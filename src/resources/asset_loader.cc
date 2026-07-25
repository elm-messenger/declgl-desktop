// resources/asset_loader.cc — async file decode for textures + fonts.
//
// See [asset_loader.h] for the threading model.

#include "resources/asset_loader.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <optional>
#include <system_error>
#include <utility>

#include <SDL3/SDL.h>
#include <nlohmann/json.hpp>

#include "audio/audio_decoder.h"
#include "resources/font.h"
#include "resources/image_decoder.h"

namespace declgl
{

std::filesystem::path app_kv_store_path(std::string_view app_name)
{
	const std::string app =
		app_name.empty() ? std::string("declgl") : std::string(app_name);
	char *pref = SDL_GetPrefPath(app.c_str(), "");
	if (pref) {
		std::filesystem::path path(pref);
		SDL_free(pref);
		return path / "kv_store.json";
	}
	return "ml_regl_kv_store.json";
}

std::optional<std::string>
read_kv_store_value(const std::filesystem::path &path, std::string_view key)
{
	std::ifstream stream(path, std::ios::binary);
	if (!stream)
		return std::nullopt;
	try {
		nlohmann::json doc;
		stream >> doc;
		const auto values = doc.find("values");
		if (values == doc.end() || !values->is_object())
			return std::nullopt;
		const auto value = values->find(std::string(key));
		if (value == values->end() || !value->is_string())
			return std::nullopt;
		return value->get<std::string>();
	} catch (const std::exception &) {
		return std::nullopt;
	}
}

AssetLoader::AssetLoader(std::filesystem::path asset_root)
{
	// Snapshot the asset root once at construction so the worker
	// thread can resolve paths without grabbing SDL state itself.
	// SDL_GetBasePath returns the directory containing the running
	// executable (with a trailing separator); the buffer is owned
	// by SDL and lives for the lifetime of the process, so we just
	// copy it into our path. We canonicalize so the
	// std::filesystem::relative() containment check below is
	// symbol-comparable across the two paths it sees.
	if (!asset_root.empty()) {
		std::error_code ec;
		auto canon = std::filesystem::weakly_canonical(asset_root, ec);
		asset_root_ = ec ? std::move(asset_root) : std::move(canon);
	} else if (const char *base = SDL_GetBasePath()) {
		std::error_code ec;
		std::filesystem::path p(base);
		auto canon = std::filesystem::weakly_canonical(p, ec);
		asset_root_ = ec ? p : canon;
	}
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
	std::size_t moved = 0;
	std::lock_guard<std::mutex> lk(mu_);
	while ((max_items == 0 || moved < max_items) && !ready_queue_.empty()) {
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

void load_kv_json(const std::string &path, ReadyAsset &out)
{
	std::ifstream in(path, std::ios::binary);
	if (!in) {
		// Missing store is a normal first-run condition.
		return;
	}
	try {
		nlohmann::json doc = nlohmann::json::parse(in);
		const auto it = doc.find("values");
		if (it == doc.end() || !it->is_object()) {
			return;
		}
		for (auto kv = it->begin(); kv != it->end(); ++kv) {
			if (kv.value().is_string()) {
				out.kv_values[kv.key()] =
					kv.value().get<std::string>();
			}
		}
	} catch (const std::exception &e) {
		out.error = std::string("json parse: ") + e.what();
	}
}

void save_kv_json(const std::string &path,
		  const std::unordered_map<std::string, std::string> &values,
		  ReadyAsset &out)
{
	try {
		nlohmann::json json_values = nlohmann::json::object();
		for (const auto &kv : values) {
			json_values[kv.first] = kv.second;
		}
		nlohmann::json doc;
		doc["version"] = 1;
		doc["values"] = std::move(json_values);

		std::ofstream os(path, std::ios::binary | std::ios::trunc);
		if (!os) {
			out.error = "open for write";
			return;
		}
		os << doc.dump(2);
		if (!os) {
			out.error = "write failed";
		}
	} catch (const std::exception &e) {
		out.error = e.what();
	}
}

// Resolve [user_path] under [root] and return the absolute path. On
// policy rejection (empty input, absolute, traversal escapes, missing
// root) returns std::nullopt and writes a "path_rejected: <why>"
// message to [err]; the caller surfaces that prefix on the wire so
// clients can distinguish a policy denial from a regular I/O miss.
//
// We intentionally do NOT call std::filesystem::canonical (which
// requires the file to exist) — failures should still be the result of
// the subsequent fopen / decoder, not this resolver. weakly_canonical
// gives us a stable form for the containment check that works whether
// or not the target exists.
std::optional<std::filesystem::path>
resolve_under_root(const std::filesystem::path &root,
		   const std::string &user_path, std::string &err)
{
	if (root.empty()) {
		err = "path_rejected: asset root unavailable";
		return std::nullopt;
	}
	if (user_path.empty()) {
		err = "path_rejected: empty path";
		return std::nullopt;
	}
	std::filesystem::path raw(user_path);
	if (raw.is_absolute()) {
		err = "path_rejected: absolute path '" + user_path + "'";
		return std::nullopt;
	}

	std::error_code ec;
	auto resolved = std::filesystem::weakly_canonical(root / raw, ec);
	if (ec) {
		err = "path_rejected: weakly_canonical failed for '" +
		      user_path + "': " + ec.message();
		return std::nullopt;
	}

	auto rel = std::filesystem::relative(resolved, root, ec);
	if (ec) {
		err = "path_rejected: relative() failed for '" + user_path +
		      "': " + ec.message();
		return std::nullopt;
	}
	// `relative()` returns a path starting with ".." iff [resolved]
	// is outside [root]. An empty result means the resolved path
	// equals the root itself, which we also treat as a rejection
	// (you can't load the directory).
	const std::string rel_str = rel.generic_string();
	if (rel_str.empty() || rel_str == "." ||
	    rel_str.compare(0, 2, "..") == 0) {
		err = "path_rejected: '" + user_path + "' escapes asset root";
		return std::nullopt;
	}
	return resolved;
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
		std::string err;
		auto resolved =
			resolve_under_root(asset_root_, job.image_url, err);
		if (!resolved) {
			out.error = std::move(err);
			return;
		}
		DecodedImage img =
			decode_image_file(resolved->string(), job.crop);
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
		std::string perr;
		auto resolved =
			resolve_under_root(asset_root_, job.image_url, perr);
		if (!resolved) {
			out.error = std::move(perr);
			return;
		}
		AudioDecodeError err = AudioDecodeError::None;
		std::string err_msg;
		out.audio = decode_audio_file(resolved->string(),
					      job.audio_sample_rate, err,
					      err_msg);
		if (!out.audio.ok()) {
			out.audio_error = err;
			out.error = err_msg.empty() ? "decode_audio_file" :
						      err_msg;
			return;
		}
		return;
	}

	if (job.kind == AssetKind::File) {
		std::string perr;
		auto resolved =
			resolve_under_root(asset_root_, job.image_url, perr);
		if (!resolved) {
			out.error = std::move(perr);
			return;
		}
		std::string err;
		if (!slurp_file(resolved->string(), out.file_data, err)) {
			out.error = std::move(err);
		}
		return;
	}

	if (job.kind == AssetKind::KvLoad) {
		// KV paths are produced by the engine via SDL_GetPrefPath
		// and are intentionally outside the asset root, so they
		// bypass [resolve_under_root].
		load_kv_json(job.image_url, out);
		return;
	}

	if (job.kind == AssetKind::KvSave) {
		save_kv_json(job.image_url, job.kv_values, out);
		return;
	}

	// ---- Font ----
	// SDF atlases must NOT be premultiplied. The job builder already
	// forces this to false; we re-assert defensively so a future
	// refactor can't silently corrupt SDF data.
	out.premultiply_alpha = false;

	std::string json_err;
	auto json_resolved =
		resolve_under_root(asset_root_, job.json_url, json_err);
	if (!json_resolved) {
		out.error = std::move(json_err);
		return;
	}

	std::string json_bytes;
	{
		std::string err;
		if (!slurp_file(json_resolved->string(), json_bytes, err)) {
			out.error = std::move(err);
			return;
		}
	}

	auto font = std::make_unique<Font>();
	if (!font->parse(json_bytes.data(), json_bytes.size())) {
		out.error = "Font::parse: " + font->error();
		return;
	}

	std::string img_err;
	auto img_resolved =
		resolve_under_root(asset_root_, job.image_url, img_err);
	if (!img_resolved) {
		out.error = std::move(img_err);
		return;
	}
	DecodedImage img =
		decode_image_file(img_resolved->string(), ImageCrop{});
	if (!img.ok()) {
		out.error = "decode_image_file(atlas)";
		return;
	}
	// (intentionally skip premultiply for fonts — see SDF note above)

	out.font = std::move(font);
	out.image = std::move(img);
}

} // namespace declgl
