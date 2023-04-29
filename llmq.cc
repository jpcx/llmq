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

#include "llmq.h"

extern "C" {
#include <curl/curl.h>
#include <fcntl.h>
#include <pwd.h>
#include <signal.h>
#include <sys/stat.h>
}

#include <fstream>
#include <iostream>

namespace fs = std::filesystem;

namespace llmq {

template <class T>
concept ostreamable = //
    requires(std::ostream& o, T& v) {
	    { o << v } -> std::same_as<std::ostream&>;
    };

template <ostreamable T, ostreamable... Ts>
inline static void
ostream_pack(std::ostream& o, T&& v, Ts&&... vs) noexcept {
	o << v;
	if constexpr (sizeof...(Ts))
		ostream_pack(o, std::forward<Ts>(vs)...);
}

template <ostreamable... Ts>
	requires(sizeof...(Ts) > 0)
[[noreturn]] static void die(Ts&&... vs) noexcept {
	ostream_pack(std::cerr, "[error] ", std::forward<Ts>(vs)...);
	std::cerr << '\n';
	std::exit(1);
}

template <ostreamable... Ts>
	requires(sizeof...(Ts) > 0)
inline static void warn(Ts&&... vs) noexcept {
	ostream_pack(std::cerr, "[warn] ", std::forward<Ts>(vs)...);
	std::cerr << '\n';
}

template <ostreamable... Ts>
	requires(sizeof...(Ts) > 0)
inline static void verbose_log(bool verbose, Ts&&... vs) noexcept {
	if (verbose) {
		ostream_pack(std::cerr, std::forward<Ts>(vs)...);
		std::cerr << '\n';
	}
}

inline static void
mkdir_p(fs::path const& dir) noexcept {
	try {
		fs::create_directories(dir);
	} catch (fs::filesystem_error const& e) {
		die("could not create directory ", dir, ": ", e.what());
	}
}

inline static void
touch(fs::path const& path, mode_t mode) noexcept {
	int fd = ::open(path.c_str(), O_CREAT | O_WRONLY | O_TRUNC, mode);
	if (fd != -1)
		::close(fd);
	else
		die("could not create file ", path, ": ", std::strerror(errno));
}

[[nodiscard]] inline static FILE*
open_file(fs::path const& path, char const* mode) noexcept {
	FILE* res = std::fopen(path.c_str(), mode);
	if (!res)
		die("failed to open FILE* at ", path, ": ", std::strerror(errno));
	return res;
}

inline static void
seek_file(fs::path const& path, FILE* f, int ofs = 0, int origin = SEEK_SET) noexcept {
	if (std::fseek(f, ofs, origin))
		std::fclose(f), die("failed to seek FILE* at ", path, ": ", std::strerror(errno));
}

[[nodiscard]] inline static std::string
read_file(fs::path const& path, FILE* f) noexcept {
	std::string data;
	seek_file(path, f, 0, SEEK_END);
	auto len = std::ftell(f);
	if (len < 0)
		die("ftell fail");
	seek_file(path, f, 0);
	data.resize(len);
	if (std::fread(data.data(), 1, len, f) != (size_t)len || std::ferror(f))
		std::fclose(f), die("failed to read FILE* at ", path);
	return data;
}

inline static void
write_file(fs::path const& path, FILE* f, std::string_view data) noexcept {
	auto n = std::fwrite(data.data(), 1, data.size(), f);
	if (n != data.size() || std::ferror(f))
		std::fclose(f), die("failure while writing to FILE* at ", path);
}

inline static void
kill_ctx(bool verbose, fs::path const& ctxfile) noexcept {
	fs::path proc{"/proc"};
	::pid_t  our_pid = ::getpid();
	fs::path our_exe = fs::read_symlink(proc / std::to_string(our_pid) / "exe").filename();
	verbose_log(verbose, "[kill] searching for PID");
	bool killed = false;
	for (auto it = fs::directory_iterator{proc}; it != fs::directory_iterator{}; ++it) {
		bool        found_ctx = false;
		::pid_t     next_pid{};
		std::string spid{};
		try {
			if (!it->is_directory())
				continue;

			std::string spid = it->path().filename();
			if (std::ranges::find_if(spid, [](char ch) {
				    return ch < '0' || ch > '9';
			    }) != spid.end())
				continue;

			next_pid = std::stoi(spid);
			if (our_pid == next_pid) {
				verbose_log(verbose, "[kill] skipping ", it->path(),
				            ": this is our PID");
				continue;
			}

			auto dir  = proc / spid;
			auto file = dir / "exe";

			if (!fs::exists(file))
				continue;

			if (!fs::is_symlink(file))
				continue;

			if (fs::read_symlink(file).filename() != our_exe)
				continue;

			verbose_log(verbose, "[kill] found llmq process ", spid);

			dir /= "fd";

			if (!fs::exists(dir))
				continue;

			if (!fs::is_directory(dir))
				continue;

			for (auto jt = fs::directory_iterator{dir}; jt != fs::directory_iterator{};
			     ++jt) {
				if (jt->is_symlink()) {
					auto link = fs::read_symlink(jt->path());
					if (fs::read_symlink(jt->path()) == ctxfile) {
						found_ctx = true;
						break;
					}
				}
			}
		} catch (...) {
			continue;
		}

		if (found_ctx) {
			if (killed)
				warn("killing another llmq process for this context. this is "
				     "unusual; locks usually prevent this from being possible");
			killed = true;
			verbose_log(verbose, "[kill] attempting to kill ", next_pid);
			if (::kill(next_pid, SIGTERM) < 0)
				die("could not terminate process ", spid, " for context ", ctxfile,
				    ": ", std::strerror(errno));
		}
	}
	if (!killed)
		die("could not locate llmq process for context ", ctxfile);
}

fs::path
plugin::confdir() const noexcept {
	return "";
}

fs::path
plugin::datadir() const noexcept {
	return "";
}

std::string_view
plugin::shortopts() const noexcept {
	return "";
}

option const*
plugin::longopts() const noexcept {
	return NULL;
}

std::optional<std::string_view>
plugin::post() const {
	return std::nullopt;
}

void
plugin::onfinish(bool print) {
	if (print) {
		// by default, output a newline
		std::cout << '\n';
	}
}

inline static std::vector<plugin*>* registry{nullptr};
inline static bool                  main_started{false};

plugin::plugin(std::source_location loc) noexcept {
	// construct on first use
	[[maybe_unused]] static std::nullptr_t _ = ([] {
		static std::vector<plugin*> reg{};
		registry     = &reg;
		main_started = false;
		return nullptr;
	})();
	if (main_started)
		die("invalid implementation for plugin at \"", loc.file_name(), ':', loc.line(),
		    "\". all plugin instances must be static");
	registry->push_back(this);
}

inline static constexpr std::string_view help =
    "usage: llmq [-hqiv] [ACTION] [PLUGIN][://CONTEXT] [OPTIONS]... [MSGS]...\n"
    "A query CLI, plugin framework, and I/O manager for conversational AIs.\n"
    "\n"
    "The llmq executable is essentially a wrapper for LLM API plugins that manages\n"
    "command-line arguments, context and config files, authentication, and I/O.\n"
    "See the README for plugin development guidelines.\n"
    "\n"
    "Each plugin defines paths for configuration and conversation context storage,\n"
    "which are $XDG_CONFIG_HOME/llmq/PLUGIN and $XDG_DATA_DIR/llmq/PLUGIN by default\n"
    "(or ~/.config/... and ~/.local/share/... if XDG dirs not defined).\n"
    "Directories are created if they do not exist.\n"
    "\n"
    "llmq flags:\n"
    "  -h --help      display this help and exit.\n"
    "  -q --quiet     do not print reply to stdout (if chat).\n"
    "  -i --no-stdin  does not read from stdin, even if MSGS is missing (if q|c).\n"
    "  -v --verbose   print cURL and other llmq diagnostics to stderr.\n"
    "\n"
    "ACTION:\n"
    "  q query  queries and streams response without modifying the context.\n"
    "  c chat   queries, streams response, and updates context.\n"
    "  i init   (re-)initializes the context file using OPTIONS.\n"
    "  e edit   edits the context file with $EDITOR or vi.\n"
    "  a auth   edits the authfile with $EDITOR or vi.\n"
    "  p path   prints the absolute filepath of the plugin or context.\n"
    "  r rm     removes the CONTEXT file.\n"
    "  k kill   terminates all llmq processes with CONTEXT open, if able.\n"
    "  l list   list all available plugins and their descriptions.\n"
    "  h help   display the llmq or plugin help and exit.\n"
    "\n"
    "notes:\n"
    " - ACTION always required, except when using `-h`\n"
    " - CONTEXT required for c|i|e|r|k\n"
    " - OPTIONS/MSGS/stdin ignored for e|a|p|r|k|l|h\n"
    " - stdin ignored for i\n"
    "\n"
    "PLUGIN:\n"
    "  At present, gpt is the only plugin available.\n"
    "  See `llmq help gpt` for more info.\n"
    "\n"
    "CONTEXT:\n"
    "  A YAML-encoded query/chat context file (e.g. model parameters, messages).\n"
    "  CONTEXT omits the \".yml\" suffix present on all context files.\n"
    "\n"
    "OPTIONS:\n"
    "  Named arguments or flags to pass to the plugin.\n"
    "\n"
    "MSGS:\n"
    "  Positional plugin args. Typically messages, but depends on the plugin.\n"
    "  If ACTION is query or chat (without -i), reads stdin into one MSG.";

inline static constexpr std::string_view usage = help.substr(0, help.find('\n'));

enum class action : uint8_t { unset, query, chat, init, edit, auth, path, rm, kill, list, help };

[[nodiscard]] inline static constexpr action
parse_action(std::string_view s) noexcept {
	using namespace std::literals;
	using enum llmq::action;
	constexpr auto opts = std::array{"query"sv, "chat"sv, "init"sv, "edit"sv, "auth"sv,
	                                 "path"sv,  "rm"sv,   "kill"sv, "list"sv, "help"sv};

	constexpr std::size_t npos = std::numeric_limits<std::size_t>::max();
	std::size_t           found{npos};
	for (std::size_t i = 0; i < opts.size(); ++i) {
		if ((s.size() == 1 && s[0] == opts[i][0]) || (s == opts[i])) {
			if (found != npos)
				return unset;
			found = i;
		}
	}

	if (found == npos)
		return unset;

	switch (found) {
		case 0: static_assert(opts[0] == "query"); return query;
		case 1: static_assert(opts[1] == "chat"); return chat;
		case 2: static_assert(opts[2] == "init"); return init;
		case 3: static_assert(opts[3] == "edit"); return edit;
		case 4: static_assert(opts[4] == "auth"); return auth;
		case 5: static_assert(opts[5] == "path"); return path;
		case 6: static_assert(opts[6] == "rm"); return rm;
		case 7: static_assert(opts[7] == "kill"); return kill;
		case 8: static_assert(opts[8] == "list"); return list;
		case 9: static_assert(opts[9] == "help"); return help;
		default: static_assert(opts.size() == 10); return unset;
	}
}

[[nodiscard]] inline static constexpr std::pair<std::string_view, std::string_view>
parse_plug_ctx_arg(std::string_view s) noexcept {
	auto plugend = s.find("://");
	if (plugend == s.npos)
		return {s, {s.end(), s.end()}};
	return {
	    {s.begin(), s.begin() + plugend},
	    {s.begin() + plugend + 3, s.end()},
	};
}

static_assert(parse_plug_ctx_arg("").first == "");
static_assert(parse_plug_ctx_arg("").second == "");
static_assert(parse_plug_ctx_arg("plugin://foo").first == "plugin");
static_assert(parse_plug_ctx_arg("plugin://foo").second == "foo");
static_assert(parse_plug_ctx_arg("plugin://foo/bar").first == "plugin");
static_assert(parse_plug_ctx_arg("plugin://foo/bar").second == "foo/bar");
static_assert(parse_plug_ctx_arg("plugin://").first == "plugin");
static_assert(parse_plug_ctx_arg("plugin://").second == "");
static_assert(parse_plug_ctx_arg("plugin").first == "plugin");
static_assert(parse_plug_ctx_arg("plugin").second == "");

[[nodiscard]] inline static fs::path
get_homedir() noexcept {
	char const* home = std::getenv("HOME");

	if (home) {
		if (!fs::exists(home) || !fs::is_directory(home))
			die("invalid $HOME directory ", home);
		return home;
	}

	errno      = 0;
	passwd* pw = getpwuid(getuid());
	if (!pw || errno)
		die("could not find $XDG_CONFIG_HOME or $HOME, and "
		    "getwpuid fallback failed: ",
		    std::strerror(errno));

	home = pw->pw_dir;
	if (!home)
		die("could not find $XDG_CONFIG_HOME or $HOME, and "
		    "getwpuid fallback failed un an unexpected way");
	if (!fs::exists(home) || !fs::is_directory(home))
		die("invalid getpwuid home directory: ", home);

	return home;
}

// returns absolute path
[[nodiscard]] inline static fs::path
plugdir_fallback(plugin const& plug, std::string_view env,
                 fs::path const& fallback_homerel) noexcept {
	char const* envdir = std::getenv(env.data());

	if (envdir) {
		if (!fs::exists(envdir) || !fs::is_directory(envdir))
			die("invalid $", env, " directory ", envdir);
		return envdir;
	}

	fs::path dir = get_homedir() / fallback_homerel / plug.name();

	if (fs::exists(dir) && !fs::is_directory(dir))
		die("fallback directory for plugin <", plug.name(), "> ", dir,
		    " exists and is not a directory");

	return fs::absolute(dir);
}

// returns absolute path
[[nodiscard]] inline static fs::path
compute_datadir(plugin const& plug) noexcept {
	if (!plug.datadir().empty()) {
		auto dir = plug.datadir();
		if (fs::exists(dir) && !fs::is_directory(dir))
			die("configured datadir for plugin <", plug.name(), "> ", dir,
			    " exists and is not a directory");
		return fs::absolute(dir);
	}
	return plugdir_fallback(plug, "XDG_DATA_HOME", ".local/share/llmq");
}

// returns absolute path
[[nodiscard]] inline static fs::path
compute_confdir(plugin const& plug) noexcept {
	if (!plug.confdir().empty()) {
		auto dir = plug.confdir();
		if (fs::exists(dir) && !fs::is_directory(dir))
			die("configured confdir for plugin <", plug.name(), "> ", dir,
			    " exists and is not a directory");
		return fs::absolute(dir);
	}
	return plugdir_fallback(plug, "XDG_CONFIG_HOME", ".config/llmq");
}

// checks if authfile has 400 or 600 permissions
[[nodiscard]] inline static bool
is_private_file(char const* auth) {
	struct stat stat;
	if (::stat(auth, &stat))
		die("error getting the status of authfile \"", auth, "\": ", std::strerror(errno));
	mode_t perms = stat.st_mode & (S_IRWXU | S_IRWXG | S_IRWXO);
	return (perms == S_IRUSR) || (perms == (S_IRUSR | S_IWUSR));
}

[[nodiscard]] inline static fs::path
compute_authfile(fs::path const& confdir) noexcept {
	auto auth = confdir / ".auth";

	if (fs::exists(auth)) {
		if (!fs::is_regular_file(auth))
			die("plugin authfile ", auth, " exists and is not a regular file");
		if (!is_private_file(auth.c_str()))
			warn("plugin authfile ", auth,
			     " has insecure permissions! please set to 400 or 600");
	}

	return auth;
}

// opens $EDITOR or vi on file
[[noreturn]] inline static void
spawn_editor(fs::path const& dir, fs::path const& file) noexcept {
	char const* editor = std::getenv("EDITOR");
	if (!editor) {
		warn("$EDITOR does not exist, falling back to vi");
		editor = "vi";
	}

	if (::chdir(dir.c_str()))
		die("failed to change directory for editor: ", std::strerror(errno));
	if (std::system((std::string{editor} + " " + std::string{file}).c_str()))
		die("could not edit ", file, ": ", std::strerror(errno));
	std::exit(0);
}

[[nodiscard]] static std::string_view
trim(std::string_view s) noexcept {
	auto begin = s.begin();
	auto end   = s.end();
	while (begin != end && std::isspace(*begin))
		++begin;
	while (begin != end && std::isspace(*(end - 1)))
		--end;
	return {begin, end};
}

struct context_writer {
   public:
	context_writer(fs::path path, std::string content) noexcept
	    : _f{open_file(path, "r+")},
	      _buf{std::move(content)},
	      _path{std::move(path)},
	      _l{} {
		_l.l_type   = F_WRLCK;
		_l.l_whence = SEEK_SET;
		_l.l_start  = 0;
		_l.l_len    = 0;
		_l.l_pid    = ::getpid();

		if (::fcntl(::fileno(_f), F_SETLK, &_l) < 0)
			die("failed to lock the context file ", _path, ": ", std::strerror(errno));
	};

	~context_writer() {
		_l.l_type = F_UNLCK;
		if (::fcntl(::fileno(_f), F_SETLK, &_l) < 0)
			die("failed to unlock the context file ", _path, ": ",
			    std::strerror(errno));
		std::fclose(_f);
	}

	void
	overwrite(ryml::Tree const& tree) noexcept {
		std::string cur = ryml::emitrs_yaml<std::string>(tree);
		size_t      bi  = 0;
		size_t      ci  = 0;

		seek_file(_path, _f, 0);

		while (bi < _buf.size() && ci < cur.size()) {
			if (_buf[bi] == cur[ci]) { // iterate over like chars
				++bi;
				++ci;
			} else { // find the end of this diff string and write the changes
				size_t dbegin = ci;
				for (; bi < _buf.size() && ci < cur.size() && _buf[bi] != cur[ci];
				     ++bi, ++ci)
					;
				size_t dend = ci;
				seek_file(_path, _f, dbegin);
				write_file(_path, _f, {cur.data() + dbegin, dend - dbegin});
			}
		}

		if (ci < cur.size()) {
			seek_file(_path, _f, ci);
			write_file(_path, _f, {cur.data() + ci, cur.size() - ci});
		}

		_buf = std::move(cur);

		std::fflush(_f);
	}

   private:
	FILE*       _f;
	std::string _buf;
	fs::path    _path;
	::flock     _l;
};

[[nodiscard]] inline static bool
hasopt(char const* s, char shortopt, char const* longopt) noexcept {
	return std::strcmp(s, longopt) == 0 ||
	       (s[0] == '-' && std::isalnum(s[1]) && std::strchr(s + 1, shortopt));
}

struct llmq_args_result {
	bool           quiet;
	bool           verbose;
	bool           no_stdin;
	enum action    action;
	unsigned       ofs; // offset for argc after parsing
	struct plugin* plugin;
	fs::path       context;
};

// reads all args up to OPTIONS
[[nodiscard]] inline static llmq_args_result
parse_llmq_args(int argc, char** argv) {
	llmq_args_result res{
	    .quiet    = false,
	    .verbose  = false,
	    .no_stdin = false,
	    .action   = action::unset,
	    .ofs      = 1,
	    .plugin   = nullptr,
	    .context  = "",
	};

	bool found_ddash = false;

	for (; res.ofs < (unsigned)argc; ++res.ofs) {
		if (!std::strcmp(argv[res.ofs], "--")) {
			found_ddash = true;
			continue;
		}
		if (!found_ddash && argv[res.ofs][0] == '-') {
			if (hasopt(argv[res.ofs], 'h', "--help"))
				std::exit((std::cout << help << '\n', 0));
			if (hasopt(argv[res.ofs], 'q', "--quiet")) {
				res.quiet = true;
				if (res.action != action::unset && res.action != action::chat)
					die("quiet flag only supported for chat mode");
			}
			if (hasopt(argv[res.ofs], 'i', "--no-stdin"))
				res.no_stdin = true;
			if (hasopt(argv[res.ofs], 'v', "--verbose"))
				res.verbose = true;
			continue;
		}

		if (res.action == action::unset) {
			if ((res.action = parse_action(argv[res.ofs])) == action::unset)
				die("invalid action \"", argv[res.ofs], "\"");
			if (res.quiet && res.action != action::chat)
				die("quiet flag only supported for chat mode");
		} else if (!res.plugin) {
			auto [plg, ctx] = parse_plug_ctx_arg(argv[res.ofs]);
			{
				auto it = std::ranges::find(*registry, plg, &plugin::name);
				if (it == registry->end())
					die("plugin \"", plg, "\" not found\n");
				res.plugin = *it;
			}
			res.context = ctx;
			break;
		}
	}

	if (res.action == action::unset)
		std::exit((std::cerr << usage << '\n', 1));

	if (res.action == action::list) {
		std::size_t max = 0;
		for (auto* v : *registry)
			max = std::max(max, v->name().size());
		for (auto* v : *registry)
			std::cout << std::left << std::setw(max + 1) << v->name() << ": "
				  << v->descr() << std::endl;
		std::exit(0);
	}

	if (!res.plugin) {
		if (res.action == action::help)
			std::exit((std::cout << help << '\n', 0));
		std::exit((std::cerr << usage << '\n', 1));
	}

	return res;
}

[[nodiscard]] static std::vector<plugin::arg>
parse_plugin_args(int argc, char** argv, unsigned ofs, plugin* plugin, bool no_stdin) {
	if (plugin->shortopts().empty() && !plugin->longopts()) {
		if (ofs < (unsigned)argc)
			die("plugin \"", plugin->name(),
			    "\" does not accept arguments, but some were provided");
		return {};
	}

	// seek argc/argv to plugin opts/args
	argc -= ofs;
	argv += ofs;

	std::vector<plugin::arg> args;

	int opt;
	while ((opt = ::getopt_long(argc, argv, plugin->shortopts().data(), plugin->longopts(),
	                            nullptr)) != -1)
		args.push_back(plugin::arg{opt, optarg ? optarg : ""});

	if (optind >= argc) {
		// no MSGS passed; read from stdin if not using -i
		if (!no_stdin) {
			std::ostringstream oss;
			oss << std::cin.rdbuf();
			args.push_back(plugin::arg{0, oss.str()});
		}
	} else {
		while (optind < argc)
			args.push_back(plugin::arg{0, argv[optind++]});
	}

	return args;
}

inline static size_t
onwrite(char* ptr, size_t size, size_t nmemb, void* plug_update_void) {
	auto plug_update =
	    reinterpret_cast<std::function<void(std::string_view)>*>(plug_update_void);
	size_t len = size * nmemb;
	(*plug_update)(std::string_view(ptr, len));
	return len;
}

// a wrapper for throwable plugin operations
template <class Op>
	requires std::is_invocable_v<Op>
inline static auto
plugop(std::string_view plugname, std::string_view opdescr, Op&& op) noexcept {
	try {
		return op();
	} catch (std::exception const& e) {
		die("failed to ", opdescr, " plugin \"", plugname, "\": ", e.what());
	} catch (char const* e) {
		die("failed to ", opdescr, " plugin \"", plugname, "\": ", e);
	} catch (...) {
		die("failed to ", opdescr, " plugin \"", plugname, "\": unknown error");
	}
}

inline static void
request(plugin* plug, bool verbose, std::function<void(std::string_view)> plug_update,
        std::function<void()> plug_finish) {
	auto init = ::curl_global_init(CURL_GLOBAL_DEFAULT);
	if (init != CURLE_OK)
		die("cURL error: ", ::curl_easy_strerror(init));
	CURL*              curl;
	std::string        prompt;
	struct curl_slist* headers = NULL;

	auto url = plugop(plug->name(), "get url from", [plug] {
		return plug->url();
	});

	plugop(plug->name(), "append headers from", [plug, &headers] {
		plug->append_headers([&headers](std::string_view h) {
			headers = ::curl_slist_append(headers, h.data());
		});
	});

	auto post = plugop(plug->name(), "get postdata from", [plug] {
		return plug->post();
	});

	curl = ::curl_easy_init();
	if (!curl)
		die("could not initialize cURL");

	curl_easy_setopt(curl, post ? CURLOPT_POST : CURLOPT_HTTPGET, 1L);
	curl_easy_setopt(curl, CURLOPT_URL, url.data());
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
	curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
	curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, onwrite);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &plug_update);
	if (verbose) {
		curl_easy_setopt(curl, CURLOPT_VERBOSE, 1L);
		if (post)
			std::cerr << "\nloading postdata:\n" << post->data() << "\n\n";
	}
	if (post) {
		curl_easy_setopt(curl, CURLOPT_POSTFIELDS, post->data());
		curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, post->size());
	}

	// send the request
	{
		CURLcode _ = ::curl_easy_perform(curl);
		if (_ != CURLE_OK)
			die("cURL error: ", ::curl_easy_strerror(_));
	}

	::curl_easy_cleanup(curl);
	::curl_slist_free_all(headers);
	::curl_global_cleanup();

	plug_finish();
}

inline static struct ryml_error_handler {
	void
	on_error(const char* msg, size_t len, ryml::Location loc) {
		throw std::runtime_error(
		    ryml::formatrs<std::string>("{}:{}:{} ({}B): ERROR: {}", loc.name, loc.line,
		                                loc.col, loc.offset, ryml::csubstr(msg, len)));
	}

	static void
	s_error(const char* msg, size_t len, ryml::Location loc, void* this_) {
		return ((ryml_error_handler*)this_)->on_error(msg, len, loc);
	}

	ryml::Callbacks
	callbacks() {
		return ryml::Callbacks(this, nullptr, nullptr, ryml_error_handler::s_error);
	}
} ryml_error_handler;

} // namespace llmq

int
main(int argc, char** argv) {
	using namespace llmq;
	using enum llmq::action;

	// prevent any new plugins from being constructed
	main_started = true;

	// read plugin, action, and context (and assert validity)
	auto a = parse_llmq_args(argc, argv);

	// print plugin help if requested
	if (a.action == help)
		return std::cout << a.plugin->help() << '\n', 0;

	// compute the datadir using the preferences and defaults
	fs::path datadir = compute_datadir(*a.plugin);

	// at this point, context may still be empty
	fs::path ctxfile = datadir;
	if (!a.context.empty()) {
		ctxfile /= a.context;
		ctxfile += ".yml";
	}

	// if action is path, print the full context path (which may or may not have context)
	if (a.action == path)
		return std::cout << ctxfile.c_str() << '\n', 0;

	// compute the other plugin paths
	fs::path confdir  = compute_confdir(*a.plugin);
	fs::path authfile = compute_authfile(confdir);

	// create the authfile if it does not exist
	if (!fs::exists(authfile)) {
		mkdir_p(authfile.parent_path());
		touch(authfile, S_IRUSR | S_IWUSR);
	}

	// if action is auth, open editor on context file and exit
	if (a.action == auth)
		spawn_editor(confdir.parent_path(), authfile);

	// for all other actions (except query), context is required
	if (a.action != query && a.context.empty())
		die("CONTEXT required for chat, init, edit, rm, and kill");

	// if action is rm, remove the context and exit
	if (a.action == rm) {
		if (fs::exists(ctxfile))
			fs::remove(ctxfile);
		std::exit(0);
	}

	// if action is kill, find the owning llmq process of the ctxfile and exit
	if (a.action == kill) {
		kill_ctx(a.verbose, ctxfile);
		std::exit(0);
	}

	// create the context file if needed
	if (!a.context.empty() && !fs::exists(ctxfile)) {
		mkdir_p(ctxfile.parent_path());
		touch(ctxfile, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
	}

	// if action is edit, open editor on context file and exit
	if (a.action == edit)
		spawn_editor(datadir.parent_path(), ctxfile);

	std::string oldctx;
	{ // plugin preparation

		// read plugin args, if enabled
		auto pargs =
		    parse_plugin_args(argc, argv, a.ofs, a.plugin, a.no_stdin || a.action == init);

		// set rapidyaml to use exceptions
		ryml::set_callbacks(ryml_error_handler.callbacks());

		// read the entire context file as YAML (if needed)
		ryml::Tree ctx;
		if (!a.context.empty()) {
			FILE* f = open_file(ctxfile.c_str(), "r");
			oldctx  = read_file(ctxfile, f);
			try {
				ctx = ryml::parse_in_arena(
				    ryml::csubstr{oldctx.data(), oldctx.size()});
			} catch (std::exception const& e) {
				die("could not parse YAML context: ", e.what());
			}
			std::fclose(f);
		}

		{
			// load authfile contents
			std::string authstr;
			{
				FILE* f = open_file(authfile.c_str(), "r");
				authstr = trim(read_file(authfile, f));
				std::fclose(f);
			}

			// initialize the plugin
			plugop(a.plugin->name(), "initialize", [&a, &ctx, &pargs, &authstr] {
				a.plugin->init(std::move(ctx), std::move(pargs),
				               std::move(authstr));
			});
		}
	}

	if (a.action == query) {
		// make the request without saving context
		request(
		    a.plugin, a.verbose,
		    [a](std::string_view reply) {
			    // update plugin and print deltas
			    plugop(a.plugin->name(), "process reply using", [&a, &reply] {
				    a.plugin->onreply(reply, true);
			    });
		    },
		    [a] {
			    // notify the plugin that the request has completed
			    plugop(a.plugin->name(), "finalize", [&a] {
				    a.plugin->onfinish(true);
			    });
		    });
	} else {
		// efficient yaml writer that manages locking
		context_writer wctx{std::move(ctxfile),
		                    std::move(oldctx)}; // warn: ctxfile and oldctx invalidated

		if (a.action == init) {
			wctx.overwrite(plugop(a.plugin->name(), "get context from", [&a] {
				return a.plugin->context();
			}));
		} else if (a.action == chat) {
			// make the request; save context each reply
			request(
			    a.plugin, a.verbose,
			    [a, &wctx](std::string_view reply) {
				    // update plugin and print deltas
				    plugop(a.plugin->name(), "process reply using", [&a, &reply] {
					    a.plugin->onreply(reply, !a.quiet);
				    });

				    wctx.overwrite(
					plugop(a.plugin->name(), "get context from", [&a] {
						return a.plugin->context();
					}));
			    },
			    [a] {
				    // notify the plugin that the request has completed
				    plugop(a.plugin->name(), "finalize", [&a] {
					    a.plugin->onfinish(!a.quiet);
				    });
			    });
		} else {
			die("internal error in action handling logic");
		}
	}

	return 0;
}
