#include <filesystem>
#include <iostream>
#include <optional>
#include <string>

#include "elm_host/elm_host.h"
#include "log/log.h"
#include "runtime/runtime.h"

namespace
{

struct Options {
	std::filesystem::path script;
	std::string module = "Main";
	std::filesystem::path asset_root;
	std::string app_name;
	bool fullscreen = false;
	std::size_t frames = 0;
};

void usage(const char *program)
{
	std::cerr << "Usage: " << program
		  << " --script app.js [--module Main]"
		     " [--asset-root path] [--app-name name] [--fullscreen]"
		     " [--frames count]\n";
}

std::optional<Options> parse_options(int argc, char **argv)
{
	Options options;
	for (int i = 1; i < argc; ++i) {
		const std::string arg = argv[i];
		if (arg == "--fullscreen") {
			options.fullscreen = true;
			continue;
		}
		if (arg == "--help" || arg == "-h")
			return std::nullopt;
		if (i + 1 >= argc)
			return std::nullopt;
		const std::string value = argv[++i];
		if (arg == "--script")
			options.script = value;
		else if (arg == "--module")
			options.module = value;
		else if (arg == "--asset-root")
			options.asset_root = value;
		else if (arg == "--app-name")
			options.app_name = value;
		else if (arg == "--frames") {
			try {
				options.frames = static_cast<std::size_t>(
					std::stoull(value));
			} catch (...) {
				return std::nullopt;
			}
		} else
			return std::nullopt;
	}
	if (options.script.empty())
		return std::nullopt;
	std::error_code ec;
	options.script = std::filesystem::absolute(options.script, ec);
	if (ec)
		return std::nullopt;
	if (options.asset_root.empty())
		options.asset_root = options.script.parent_path();
	else
		options.asset_root =
			std::filesystem::absolute(options.asset_root, ec);
	if (ec)
		return std::nullopt;
	if (options.app_name.empty())
		options.app_name = options.script.stem().string();
	return options;
}

} // namespace

int main(int argc, char **argv)
{
	auto options = parse_options(argc, argv);
	if (!options) {
		usage(argv[0]);
		return 2;
	}

	declgl::log::init();
	declgl::ElmHostConfig config;
	config.script = options->script;
	config.module = options->module;
	config.app_name = options->app_name;
	config.fullscreen = options->fullscreen;
	config.max_frames = options->frames;
	declgl::ElmHost host(std::move(config));
	if (!host.initialize()) {
		std::cerr << "declgl-player: " << host.error() << '\n';
		return 1;
	}
	declgl::Runtime runtime(host, options->asset_root);
	bool started = false;
	for (const auto &batch : host.take_startup_commands()) {
		started = runtime.dispatch(batch.data(), batch.size()) ||
			  started;
		if (!runtime.last_error().empty())
			break;
	}
	if (!host.ok()) {
		std::cerr << "declgl-player: " << host.error() << '\n';
		return 1;
	}
	if (!started) {
		std::cerr
			<< "declgl-player: "
			<< (runtime.last_error().empty() ?
				    "Elm application did not issue a start command" :
				    runtime.last_error())
			<< '\n';
		return 1;
	}
	runtime.run();
	if (!host.ok()) {
		std::cerr << "declgl-player: " << host.error() << '\n';
		return 1;
	}
	return 0;
}
