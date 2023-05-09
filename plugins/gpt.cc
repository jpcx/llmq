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

#include "gpt.h"

#include <iostream>
#include <sstream>

namespace llmq {

[[nodiscard]] std::string_view
gpt::name() const noexcept {
	return "gpt";
}

[[nodiscard]] std::string_view
gpt::shortopts() const noexcept {
	return "hm:T:p:n:S:X:t:P:F:L:U:s:g:u:";
}

[[nodiscard]] option const*
gpt::longopts() const noexcept {
	static constexpr std::array<option, 16> opts = {{
	    {"help", no_argument, nullptr, 'h'},
	    {"model", required_argument, nullptr, 'm'},
	    {"temperature", required_argument, nullptr, 'T'},
	    {"top-p", required_argument, nullptr, 'p'},
	    {"n", required_argument, nullptr, 'n'},
	    {"stream", required_argument, nullptr, 'S'},
	    {"stop", required_argument, nullptr, 'X'},
	    {"max-tokens", required_argument, nullptr, 't'},
	    {"presence-penalty", required_argument, nullptr, 'P'},
	    {"frequency-penalty", required_argument, nullptr, 'F'},
	    {"logit-bias", required_argument, nullptr, 'L'},
	    {"user", required_argument, nullptr, 'U'},
	    {"sys", required_argument, nullptr, 's'},
	    {"gpt", required_argument, nullptr, 'g'},
	    {"usr", required_argument, nullptr, 'u'},
	    {nullptr, 0, nullptr, 0},
	}};
	return opts.data();
}

namespace impl {
inline static constexpr std::string_view help =
    "usage: llmq ARGS... gpt[://CONTEXT] [OPTIONS]... [-sgu TAGMSG]... [USRMSG]...\n"
    "an llmq plugin for the OpenAI Chat Completions endpoint.\n"
    "authfile must be a YAML map with properties \"key\" and optionally \"org\".\n"
    "\n"
    "context file a 1:1 match with the parameters sent to the endpoint.\n"
    "see https://platform.openai.com/docs/api-reference/chat for details.\n"
    "\n"
    "ARGS:\n"
    "  arguments for llmq. see llmq --help for info.\n"
    "\n"
    "OPTIONS:\n"
    "  -h --help                   display this help and exit\n"
    "  -m --model STR              model endpoint\n"
    "  -T --temperature NUM        sampling temperature to use\n"
    "  -p --top-p NUM              nucleus sampling probability mass\n"
    "  -n --n INT                  number of choices to generate\n"
    "  -S --stream BOOL            enable receiving partial deltas\n"
    "  -X --stop STR               add a stop sequence\n"
    "  -t --max-tokens INT         maximum number of tokens to generate\n"
    "  -P --presence-penalty NUM   penalty for token similarity\n"
    "  -F --frequency-penalty NUM  penalty for token frequency\n"
    "  -L --logit-bias MAP         JSON map of token biases\n"
    "  -U --user STR               unique user identifier\n"
    "\n"
    "note: OPTIONS override CONTEXT\n"
    "\n"
    "TAGMSG:\n"
    "  -s --sys STR  append a system message to the context\n"
    "  -g --gpt STR  append an assistant message to the context\n"
    "  -u --usr STR  append a user message to the context\n"
    "\n"
    "USRMSG:\n"
    "  append a user message to the context (same as -u USRMSG)";

inline static constexpr std::string_view usage = help.substr(0, help.find('\n'));

inline static constexpr std::string_view descr =
    help.substr(usage.size() + 1, help.find('\n', help.find('\n') + 1) - (usage.size() + 1));

inline static std::string                key{};
inline static std::string                org{};
inline static std::vector<ryml::NodeRef> replies{};
inline static std::string                reply_buf{};
inline static std::string                post_buf{};
} // namespace impl

[[nodiscard]] std::string_view
gpt::help() const noexcept {
	return impl::help;
}

[[nodiscard]] std::string_view
gpt::usage() const noexcept {
	return impl::usage;
}

[[nodiscard]] std::string_view
gpt::descr() const noexcept {
	return impl::descr;
}

void
gpt::init(ryml::Tree ctx_, std::span<arg const> args, std::string auth) {
	ctx = std::move(ctx_);
	ryml::Tree authyaml;
	try {
		authyaml = ryml::parse_in_place(ryml::substr{auth.data(), auth.size()});
	} catch (std::exception const& e) {
		throw std::runtime_error("could not parse authentication data: " +
		                         std::string{e.what()});
	}
	auto authroot = authyaml.rootref();
	if (!authroot.is_map())
		throw std::runtime_error{"authfile must be a YAML map with properties \"key\" and "
		                         "optionally \"org\""};
	try {
		authroot["key"] >> impl::key;
		if (!authroot["org"].is_seed())
			authroot["org"] >> impl::org;
	} catch (std::exception const& e) {
		throw std::runtime_error("could not parse authentication data: " +
		                         std::string{e.what()});
	}
	auto root = ctx.rootref();
	if (root.empty())
		root |= ryml::MAP;
	if (!root.is_map())
		throw std::runtime_error{
		    "gpt context must be a YAML map- see `llmq help gpt` for details"};

	for (auto&& [n, v] : args) {
		if (n == 'h')
			exit((std::cout << help() << '\n', 0));

		if (v.empty())
			throw std::runtime_error{"invalid flag: " + (std::isalpha(n)
			                                                 ? std::string{1, (char)n}
			                                                 : std::to_string(n))};

		if (n == 'm') {
			root["model"] << v;
		} else if (n == 'T') {
			root["temperature"] << v;
		} else if (n == 'p') {
			root["top_p"] << v;
		} else if (n == 'n') {
			root["n"] << v;
		} else if (n == 'S') {
			root["stream"] << v;
		} else if (n == 'X') {
			if (root["stop"].is_seed())
				root["stop"] |= ryml::SEQ;
			root["stop"].append_child() << v;
		} else if (n == 't') {
			root["max_tokens"] << v;
		} else if (n == 'P') {
			root["presence_penalty"] << v;
		} else if (n == 'F') {
			root["frequency_penalty"] << v;
		} else if (n == 'L') {
			auto map  = ryml::parse_in_arena({v.data(), v.data() + v.size()});
			auto rmap = map.rootref();
			if (!rmap.is_map())
				throw std::runtime_error{"logit-bias must be a JSON map"};
			auto l = root["logit_bias"];
			if (l.is_seed())
				l |= ryml::MAP;
			for (auto&& v : rmap) {
				auto kv = l.append_child();
				kv[v.key()] << v.val();
			}
		} else if (n == 'U') {
			root["user"] << v;
		} else if (n == 's') {
			add_message("system", v);
		} else if (n == 'g') {
			add_message("assistant", v);
		} else if (n == 'u' || n == 0) {
			add_message("user", v);
		} else {
			throw std::runtime_error{"invalid option: " + (std::isalpha(n)
			                                                   ? std::string{1, (char)n}
			                                                   : std::to_string(n))};
		}
	}
}

[[nodiscard]] ryml::Tree const&
gpt::context() const noexcept {
	return ctx;
}

[[nodiscard]] std::string_view
gpt::url() const noexcept {
	return "https://api.openai.com/v1/chat/completions";
}

void
gpt::append_headers(std::function<void(std::string_view)> append) const noexcept {
	append("Content-Type: application/json");
	append("Authorization: Bearer " + impl::key);
	if (!impl::org.empty())
		append("OpenAI-Organization: " + impl::org);
}

[[nodiscard]] std::optional<std::string_view>
gpt::post() const {
	impl::post_buf = ryml::emitrs_json<std::string>(ctx);
	return {impl::post_buf};
}

[[nodiscard]] inline static constexpr std::string_view
find_json(std::string_view s) noexcept {
	int    depth = 0;
	int    s_esc = 0;
	size_t begin = s.npos;

	for (size_t i = 0; i < s.length(); ++i) {
		char c = s[i];

		if (s_esc) {
			if (c == '\\')
				++i;
			else if (c == '\"')
				s_esc = false;
		} else {
			if (c == '{') {
				if (!depth++)
					begin = i;
			} else if (c == '}') {
				if (--depth == 0)
					return s.substr(begin, i - begin + 1);
			} else if (c == '\"')
				s_esc = true;
		}
	}
	return {s.end(), s.end()};
}

static_assert(find_json("") == "");
static_assert(find_json("{}") == "{}");
static_assert(find_json(" {}") == "{}");
static_assert(find_json("foo: {}") == "{}");
static_assert(find_json(" foo: {}") == "{}");
static_assert(find_json(" foo: {}  bar ") == "{}");
static_assert(find_json(" foo: {\"a\": \"b: {\"}  bar ") == "{\"a\": \"b: {\"}");
static_assert(find_json(" foo: {\"a\": \"b: }\"}  bar ") == "{\"a\": \"b: }\"}");

void
gpt::onreply(std::string_view reply, bool print) {
	impl::reply_buf += reply;

	std::string json;

	{
		auto jview = find_json(impl::reply_buf);
		if (jview.empty())
			return; // wait for more chunks
		json = jview;
		// erase this json (jview invalidated!)
		impl::reply_buf.erase(0, jview.data() + jview.size() - impl::reply_buf.data());
	}

	ryml::Tree    reply_tree = ryml::parse_in_place(ryml::substr{json.data(), json.size()});
	ryml::NodeRef root       = reply_tree.rootref();

	bool actually_print;
	if (print) {
		if (ctx.rootref()["n"].has_val()) {
			unsigned n;
			ctx.rootref()["n"] >> n;
			actually_print = n == 1;
		} else
			actually_print = true;
	} else
		actually_print = false;

	auto choices = root["choices"];
	if (!choices.is_seed() && choices.is_seq() && !choices.empty()) {
		for (std::size_t i = 0; i < choices.num_children(); ++i) {
			std::size_t idx;
			if (choices[i]["index"].is_seed() || !ryml::read(choices[i]["index"], &idx))
				throw std::runtime_error("invalid response: " + std::string{json});

			while (idx >= impl::replies.size())
				impl::replies.push_back(add_message("", ""));

			std::string role;
			std::string content;
			if (choices[i]["message"].is_seed()) {
				auto delta = choices[i]["delta"];
				if (delta.is_seed() || !delta.is_map())
					throw std::runtime_error("invalid response: " +
					                         std::string{json});
				if (!delta["role"].is_seed() && !ryml::read(delta["role"], &role))
					throw std::runtime_error("invalid response: " +
					                         std::string{json});

				if (!delta["content"].is_seed() &&
				    !ryml::read(delta["content"], &content))
					throw std::runtime_error("invalid response: " +
					                         std::string{json});

				if (delta["role"].is_seed()) {
					if (impl::replies[idx]["role"].empty())
						throw std::runtime_error(
						    "never received role; last "
						    "received: " +
						    std::string{json});
					else
						impl::replies[idx]["role"] >> role;
				}
			} else {
				auto msg = choices[i]["message"];
				if (msg.is_seed() || !msg.is_map() ||
				    !ryml::read(msg["role"], &role) ||
				    !ryml::read(msg["content"], &content))
					throw std::runtime_error("invalid response: " +
					                         std::string{json});
			}

			if (actually_print)
				std::cout << content << std::flush;

			if (impl::replies[idx]["role"] != "" &&
			    impl::replies[idx]["role"] != ryml::csubstr{role.data(), role.size()}) {
				throw std::runtime_error("invalid response: " + std::string{json});
			}
			impl::replies[idx]["role"] << role;
			std::string tmp;
			impl::replies[idx]["content"] >> tmp;
			tmp += content;
			impl::replies[idx]["content"] |= ryml::VALQUO;
			impl::replies[idx]["content"] << tmp;
		}
	} else {
		throw std::runtime_error("invalid response: " + std::string{json});
	}
}

void
gpt::onfinish(bool print) {
	if (!print)
		return;

	unsigned n;
	auto     root = ctx.rootref();

	if (!root["n"].has_val()) {
		std::cout << '\n';
		return;
	}

	root["n"] >> n;

	if (n == 1) {
		std::cout << '\n';
		return;
	}

	ryml::Tree    msgs_data;
	ryml::NodeRef msgs = msgs_data.rootref();
	msgs |= ryml::SEQ;
	auto len = root["messages"].num_children();
	if (root["messages"].is_seed() || len < n)
		throw std::runtime_error{"invalid response: expected at least " +
		                         std::to_string(n) + " messages"};
	for (std::size_t i = 0; i < n; ++i) {
		auto node = msgs.append_child();
		auto m    = root["messages"][(len - n) + i];
		if (m.is_seed() || m["role"].is_seed() || m["content"].is_seed())
			throw std::runtime_error{"invalid response: expected messages to have "
			                         "\"role\" and \"content\" "};
		std::string role;
		m["role"] >> role;
		if (role != "assistant")
			throw std::runtime_error{
			    "invalid role: expected \"assistant\", received \"" + role + "\""};
		std::string content;
		m["content"] >> content;
		node << content;
	}

	std::cout << ryml::emitrs_json<std::string>(msgs_data) << '\n';
}

ryml::NodeRef
gpt::add_message(std::string_view role, std::string_view content) {
	auto m = ctx.rootref()["messages"];
	if (m.is_seed())
		m |= ryml::SEQ;
	auto s = m.append_child();
	s |= ryml::MAP;
	s["role"] << ryml::csubstr{role.data(), role.size()};
	s["content"] |= ryml::VALQUO;
	s["content"] << ryml::csubstr{content.data(), content.size()};
	return s;
}

} // namespace llmq
