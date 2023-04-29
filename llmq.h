#ifndef LLMQ_H_INCLUDED
#define LLMQ_H_INCLUDED
//
//  oooo  oooo
//  `888  `888
//   888   888  ooo. .oo.  .oo.    .ooooo oo
//   888   888  `888P"Y88bP"Y88b  d88' `888
//   888   888   888   888   888  888   888
//  o888o o888o o888o o888o o888o `V8bod888
//                                      888.
//  a query CLI, plugin framework, and  8P'
//  I/O manager for conversational AIs  "
//
//  Copyright (C) 2023 Justin Collier <m@jpcx.dev>
//
//    This program is free software: you can redistribute it and/or modify
//    it under the terms of the GNU Affero General Public License as
//    published by the Free Software Foundation, either version 3 of the
//    License, or (at your option) any later version.
//
//    This program is distributed in the hope that it will be useful,
//    but WITHOUT ANY WARRANTY; without even the implied warranty of
//    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//    GNU Affero General Public License for more details.
//
//  You should have received a copy of the GNU Affero General Public License
//  along with this program.  If not, see <https://www.gnu.org/licenses/>.

#include "3rdparty/ryml.hpp"

#include <getopt.h>

#include <filesystem>
#include <functional>
#include <optional>
#include <source_location>
#include <span>
#include <sstream>
#include <string_view>
#include <unordered_map>
#include <variant>

namespace llmq {

// base class for plugins. create a static instance to register it with the executable.
struct plugin {
	struct arg {
		int         name;  // shortopt char or longopt val, 0 if unnamed
		std::string value; // empty if no value (flag)
	};

	// name of the plugin. called before init.
	[[nodiscard]] virtual std::string_view name() const noexcept = 0;

	// path to the plugin configuration directory. called before init.
	// if not overridden (or empty), uses $XDG_CONFIG_HOME/llmq/PLUGIN
	// (or ~/.config/llmq/PLUGIN if XDG_CONFIG_HOME is not found).
	// the confdir will be created if not found.
	[[nodiscard]] virtual std::filesystem::path confdir() const noexcept;

	// path to the plugin context storage. called before init.
	// if not overridden (or empty), uses $XDG_DATA_HOME/llmq/PLUGIN
	// (or ~/.local/share/llmq/PLUGIN if XDG_DATA_HOME is not found).
	// the datadir will be created if not found.
	[[nodiscard]] virtual std::filesystem::path datadir() const noexcept;

	// getopt shortopts. empty string_view disables shortopts. called before init.
	[[nodiscard]] virtual std::string_view shortopts() const noexcept;

	// getopt longopts. nullptr disables longopts. called before init.
	[[nodiscard]] virtual option const* longopts() const noexcept;

	// provides the help string. called before init.
	[[nodiscard]] virtual std::string_view help() const noexcept = 0;

	// the plugin usage statement. called before init.
	[[nodiscard]] virtual std::string_view usage() const noexcept = 0;

	// a short one-line description of the plugin. called before init.
	[[nodiscard]] virtual std::string_view descr() const noexcept = 0;

	// registers the plugin. instances must be static.
	plugin(std::source_location loc = std::source_location::current()) noexcept;
	plugin(plugin const&)            = delete;
	plugin(plugin&&)                 = delete;
	plugin& operator=(plugin const&) = delete;
	plugin& operator=(plugin&&)      = delete;
	virtual ~plugin()                = default;

	// initialize the plugin with the context tree, plugin args, and authfile data.
	// guaranteed to be called before context, url, headers, post, and onreply.
	virtual void init(ryml::Tree context, std::span<arg const> args, std::string auth) = 0;

	// provides the current, updated context.
	[[nodiscard]] virtual ryml::Tree const& context() const = 0;

	// provides the endpoint URL.
	[[nodiscard]] virtual std::string_view url() const = 0;

	// appends the request headers.
	virtual void append_headers(std::function<void(std::string_view)> append) const = 0;

	// computes the postdata. if not overridden (or nullopt), llmq uses GET instead.
	[[nodiscard]] virtual std::optional<std::string_view> post() const;

	// integrate a reply into the context.
	// onreply should print content if print is true (if applicable).
	virtual void onreply(std::string_view reply, bool print) = 0;

	// called when the response has completed. prints a newline by default (if print).
	virtual void onfinish(bool print);
};

} // namespace llmq

#endif
