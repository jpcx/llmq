#ifndef LLMQ_PLUGINS_GPT_H_INCLUDED
#define LLMQ_PLUGINS_GPT_H_INCLUDED
//  oooo  oooo
//  `888  `888
//   888   888  ooo. .oo.  .oo.    .ooooo oo
//   888   888  `888P"Y88bP"Y88b  d88' `888
//   888   888   888   888   888  888   888
//  o888o o888o o888o o888o o888o `V8bod888
//  ┌─────────────────────────────────┐ 888
//  │ a query CLI and context manager │ 888.
//  │ for LLM-powered shell pipelines │ 8P'
//  └─────────────────────────────────┘ "
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

#include "llmq.h"

#include <iostream>
#include <sstream>

namespace llmq {

// usage: llmq ARGS... gpt[://CONTEXT] [OPTIONS]... [-sgu TAGMSG]... [USRMSG]...
// a llmq plugin for the OpenAI Chat Completions endpoint.
inline struct gpt : plugin {
	[[nodiscard]] std::string_view name() const noexcept override;
	[[nodiscard]] std::string_view shortopts() const noexcept override;
	[[nodiscard]] option const*    longopts() const noexcept override;
	[[nodiscard]] std::string_view help() const noexcept override;
	[[nodiscard]] std::string_view usage() const noexcept override;
	[[nodiscard]] std::string_view descr() const noexcept override;
	void init(ryml::Tree context, std::span<arg const> args, std::string auth) override;
	[[nodiscard]] ryml::Tree const& context() const noexcept override;
	[[nodiscard]] std::string_view  url() const noexcept override;
	void append_headers(std::function<void(std::string_view)> append) const noexcept override;
	[[nodiscard]] std::optional<std::string_view> post() const override;
	void onreply(std::string_view reply, bool print) override;
	void onfinish(bool print) override;

   protected:
	ryml::Tree    ctx;
	ryml::NodeRef add_message(std::string_view role, std::string_view content);
} gpt;

} // namespace llmq

#endif
