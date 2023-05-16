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

extern "C" {
#include <curl/curl.h>
#include <fcntl.h>
#include <pwd.h>
#include <signal.h>
#include <sys/stat.h>
}

#include <chrono>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>

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

fs::path
plugin::tmpdir() const noexcept {
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
    "usage: llmq [-hqivr] [ACTION] [PLUGIN][://[~]CONTEXT] [OPTIONS]... [--] [MSGS]...\n"
    "A query CLI and context manager for LLM-powered shell pipelines.\n"
    "\n"
    "llmq is essentially a wrapper for LLM API plugins that manages command-line\n"
    "arguments, context and configuration files, authentication, and I/O.\n"
    "See the README to develop your own plugins.\n"
    "\n"
    "Each plugin defines paths for configuration and conversation context storage,\n"
    "which are $XDG_CONFIG_HOME/llmq/PLUGIN and $XDG_DATA_DIR/llmq/PLUGIN by default\n"
    "(or ~/.config/llmq/PLUGIN and ~/.local/share/llmq/PLUGIN if XDG dirs not defined).\n"
    "Directories are created if they do not exist.\n"
    "\n"
    "llmq can also create and use temporary context files, which are stored in\n"
    "/tmp/llmq/PLUGIN. Any CONTEXT argument that begins with '~' is stored\n"
    "relative to the temp directory, not the data directory. Additionally,\n"
    "calling init without CONTEXT will create a unique temporary file and\n"
    "print its name.\n"
    "\n"
    "llmq flags:\n"
    "  -h --help      display this help and exit.\n"
    "  -q --quiet     do not print reply to stdout (if chat).\n"
    "  -i --no-stdin  does not read from stdin, even if MSGS is missing (if q|c).\n"
    "  -v --verbose   print cURL and other llmq diagnostics to stderr.\n"
    "  -r --retry     retry at most once if q|c failed (retains context).\n"
    "\n"
    "ACTION:\n"
    "  q query  queries and streams response without modifying the context.\n"
    "  c chat   queries, streams response, and updates context.\n"
    "  i init   (re-)initializes the context file using OPTIONS.\n"
    "  e edit   edits the context file with $EDITOR or vi.\n"
    "  a auth   edits the authfile with $EDITOR or vi.\n"
    "  p path   prints the absolute filepath of the plugin or context.\n"
    "  d del    deletes the CONTEXT file.\n"
    "  k kill   terminates all llmq processes with CONTEXT open, if able.\n"
    "  l list   list all available plugins and their descriptions.\n"
    "  h help   display the llmq or plugin help and exit.\n"
    "\n"
    "notes:\n"
    " - ACTION always required, except when using `-h`\n"
    " - CONTEXT required for c|e|d|k\n"
    " - OPTIONS/MSGS/stdin ignored for e|a|p|d|k|l|h\n"
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

enum class action : uint8_t { unset, query, chat, init, edit, auth, path, del, kill, list, help };

[[nodiscard]] inline static constexpr action
parse_action(std::string_view s) noexcept {
	using namespace std::literals;
	using enum llmq::action;
	constexpr auto opts = std::array{"query"sv, "chat"sv, "init"sv, "edit"sv, "auth"sv,
	                                 "path"sv,  "del"sv,  "kill"sv, "list"sv, "help"sv};

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
		case 6: static_assert(opts[6] == "del"); return del;
		case 7: static_assert(opts[7] == "kill"); return kill;
		case 8: static_assert(opts[8] == "list"); return list;
		case 9: static_assert(opts[9] == "help"); return help;
		default: static_assert(opts.size() == 10); return unset;
	}
}

[[nodiscard]] inline constexpr std::string_view
serialize_action(action v) noexcept {
	using enum llmq::action;
	switch (v) {
		case query: return "query";
		case chat: return "chat";
		case init: return "init";
		case edit: return "edit";
		case auth: return "auth";
		case path: return "path";
		case del: return "del";
		case kill: return "kill";
		case list: return "list";
		case help: return "help";
		default: return "unset";
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
	bool           retry;
	enum action    action;
	unsigned       ofs; // offset for argc after parsing
	struct plugin* plugin;
	std::string    context;
};

// reads all args up to OPTIONS
[[nodiscard]] inline static llmq_args_result
parse_llmq_args(int argc, char** argv) {
	llmq_args_result res{
	    .quiet    = false,
	    .verbose  = false,
	    .no_stdin = false,
	    .retry    = false,
	    .action   = action::unset,
	    .ofs      = 1,
	    .plugin   = nullptr,
	    .context  = "",
	};

	for (; res.ofs < (unsigned)argc; ++res.ofs) {
		if (argv[res.ofs][0] == '-') {
			if (!std::strcmp(argv[res.ofs], "--"))
				die("\"--\" may only be used to separate OPTIONS from MSGS after "
				    "PLUGIN is provided");
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
			if (hasopt(argv[res.ofs], 'r', "--retry"))
				res.retry = true;
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
			if (!ctx.empty() && ctx.back() == '/')
				die("CONTEXT \"", ctx, "\" is not a valid filename");
			res.context = ctx;
			break;
		}
	}

	if (res.action == action::unset)
		std::exit((std::cerr << usage << '\n', 1));

	if (!res.plugin && res.action != action::help && res.action != action::list) {
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
homedir_fallback(plugin const& plug, std::string_view env,
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
compute_datadir(llmq_args_result const& a) noexcept {
	if (!a.plugin->datadir().empty()) {
		auto dir = a.plugin->datadir();
		if (fs::exists(dir) && !fs::is_directory(dir))
			die("configured datadir for plugin <", a.plugin->name(), "> ", dir,
			    " exists and is not a directory");
		return fs::absolute(dir);
	}
	return homedir_fallback(*a.plugin, "XDG_DATA_HOME", ".local/share/llmq");
}

// returns absolute path
[[nodiscard]] inline static fs::path
compute_tmpdir(llmq_args_result const& a) noexcept {
	if (!a.plugin->tmpdir().empty()) {
		auto dir = a.plugin->tmpdir();
		if (fs::exists(dir) && !fs::is_directory(dir))
			die("configured tmpdir for plugin <", a.plugin->name(), "> ", dir,
			    " exists and is not a directory");
		return fs::absolute(dir);
	}
	auto dir = fs::path{"/tmp/llmq"} / a.plugin->name();
	if (fs::exists(dir) && !fs::is_directory(dir))
		die("default tmpdir for plugin <", a.plugin->name(), "> ", dir,
		    " exists and is not a directory");
	return dir;
}

// returns absolute path
[[nodiscard]] inline static fs::path
compute_confdir(llmq_args_result const& a) noexcept {
	if (!a.plugin->confdir().empty()) {
		auto dir = a.plugin->confdir();
		if (fs::exists(dir) && !fs::is_directory(dir))
			die("configured confdir for plugin <", a.plugin->name(), "> ", dir,
			    " exists and is not a directory");
		return fs::absolute(dir);
	}
	return homedir_fallback(*a.plugin, "XDG_CONFIG_HOME", ".config/llmq");
}

// returns absolute path
[[nodiscard]] inline static fs::path
compute_ctxfile(llmq_args_result const& a) noexcept {
	if (a.context.empty())
		die(serialize_action(a.action), " requires CONTEXT");
	fs::path f;
	if (a.context.front() != '~')
		f = compute_datadir(a);
	else
		f = compute_tmpdir(a);
	f /= a.context;
	f += ".yml";
	return f;
}

// finds a new tempfile to use
[[nodiscard]] inline static std::string
compute_tmpctx(llmq_args_result const& a) noexcept {
	fs::path dir;
	dir            = compute_tmpdir(a);
	auto        tt = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
	std::tm*    tm = std::localtime(&tt);
	std::string fname_base;
	{
		std::stringstream ss;
		ss << "~" << std::put_time(tm, "%Y%m%d%H%M%S");
		fname_base = ss.str();
	}

	unsigned idx = 0;
	fs::path f;
	do {
		std::stringstream fname;
		fname << fname_base << "." << idx << ".yml";
		f = dir / fname.str();
		++idx;
	} while (std::filesystem::exists(f));

	return f.filename().replace_extension("");
}

// returns absolute path
[[nodiscard]] inline static fs::path
prepare_ctxfile(llmq_args_result const& a) noexcept {
	fs::path f = compute_ctxfile(a);

	// create the authfile if it does not exist
	if (!fs::exists(f)) {
		mkdir_p(f.parent_path());
		touch(f, S_IRUSR | S_IWUSR);
	}

	return f;
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
compute_authfile(llmq_args_result const& a) noexcept {
	fs::path dir  = compute_confdir(a);
	auto     auth = dir / ".auth";

	if (fs::exists(auth)) {
		if (!fs::is_regular_file(auth))
			die("plugin authfile ", auth, " exists and is not a regular file");
		if (!is_private_file(auth.c_str()))
			warn("plugin authfile ", auth,
			     " has insecure permissions! please set to 400 or 600");
	}

	return auth;
}

// returns absolute path
[[nodiscard]] inline static fs::path
prepare_authfile(llmq_args_result const& a) noexcept {
	fs::path f = compute_authfile(a);

	// create the authfile if it does not exist
	if (!fs::exists(f)) {
		mkdir_p(f.parent_path());
		touch(f, S_IRUSR | S_IWUSR);
	}

	return f;
}

// opens $EDITOR or vi on file
inline static void
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
}

inline static void
print_plugins() noexcept {
	std::size_t max = 0;
	for (auto* v : *registry)
		max = std::max(max, v->name().size());
	for (auto* v : *registry)
		std::cout << std::left << std::setw(max + 1) << v->name() << ": " << v->descr()
			  << std::endl;
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

inline static size_t
onwrite(char* ptr, size_t size, size_t nmemb, void* plug_update_void) {
	auto plug_update =
	    reinterpret_cast<std::function<bool(std::string_view)>*>(plug_update_void);
	size_t len = size * nmemb;
	if ((*plug_update)(std::string_view(ptr, len)))
		return len;
	return 0;
}

// a wrapper for throwable plugin operations
template <class Op>
	requires std::is_invocable_v<Op>
inline static auto
plugop(std::string_view plugname, std::string_view opdescr, Op&& op) noexcept {
	try {
		return op();
	} catch (std::exception const& e) {
		throw std::runtime_error{"failed to " + std::string{opdescr} + " plugin \"" +
		                         std::string{plugname} + "\": " + e.what()};
	} catch (char const* e) {
		throw std::runtime_error{"failed to " + std::string{opdescr} + " plugin \"" +
		                         std::string{plugname} + "\": " + e};
	} catch (...) {
		throw std::runtime_error{"failed to " + std::string{opdescr} + " plugin \"" +
		                         std::string{plugname} + "\": unknown error"};
	}
}

[[nodiscard]] inline static std::string
read_context(fs::path const& ctxfile) noexcept {
	FILE*       f      = open_file(ctxfile.c_str(), "r");
	std::string oldctx = read_file(ctxfile, f);
	std::fclose(f);

	return oldctx;
}

[[nodiscard]] inline static ryml::Tree
parse_context(std::string_view oldctx) noexcept {
	// read the entire context file as YAML (if needed)
	ryml::Tree ctx;
	try {
		ctx = ryml::parse_in_arena(ryml::csubstr{oldctx.data(), oldctx.size()});
	} catch (std::exception const& e) {
		die("could not parse YAML context: ", e.what());
	}

	return ctx;
}

[[nodiscard]] inline std::string
read_auth(fs::path const& authfile) noexcept {
	FILE*       f    = open_file(authfile.c_str(), "r");
	std::string auth = std::string{trim(read_file(authfile, f))};
	std::fclose(f);
	return auth;
}

inline static void
init_plugin(int argc, char** argv, llmq_args_result& a, std::string const& oldctx,
            fs::path const& authfile) noexcept {
	ryml::Tree ctx = parse_context(oldctx);

	// read plugin args, if enabled
	auto args =
	    parse_plugin_args(argc, argv, a.ofs, a.plugin, a.no_stdin || a.action == action::init);

	// load authfile contents
	std::string auth = read_auth(authfile);

	try {
		plugop(a.plugin->name(), "initialize", [&a, &ctx, &args, &auth] {
			a.plugin->init(std::move(ctx), std::move(args), std::move(auth));
		});
	} catch (std::exception const& e) {
		die(e.what());
	}
}

[[nodiscard]] inline static context_writer
init_plugctx(int argc, char** argv, llmq_args_result& a) noexcept {
	fs::path    ctxfile = prepare_ctxfile(a);
	std::string oldctx  = read_context(ctxfile);

	// initialize the plugin
	init_plugin(argc, argv, a, oldctx, prepare_authfile(a));

	// initialize the yaml writer
	return {std::move(ctxfile), std::move(oldctx)};
}

[[nodiscard]] inline static bool
request(plugin* plug, bool verbose, std::function<bool(std::string_view)> plug_update) {
	auto init = ::curl_global_init(CURL_GLOBAL_DEFAULT);
	if (init != CURLE_OK)
		die("cURL error: ", ::curl_easy_strerror(init));
	CURL*              curl;
	std::string        prompt;
	struct curl_slist* headers = NULL;

	std::string_view                url;
	std::optional<std::string_view> post;
	try {
		url = plugop(plug->name(), "get url from", [plug] {
			return plug->url();
		});

		plugop(plug->name(), "append headers from", [plug, &headers] {
			plug->append_headers([&headers](std::string_view h) {
				headers = ::curl_slist_append(headers, h.data());
			});
		});

		post = plugop(plug->name(), "get postdata from", [plug] {
			return plug->post();
		});
	} catch (std::exception const& e) {
		die(e.what());
	}

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

	bool res = true;

	// send the request
	{
		CURLcode _ = ::curl_easy_perform(curl);
		if (_ == CURLE_WRITE_ERROR)
			res = false;
		else if (_ != CURLE_OK)
			die("cURL error: ", ::curl_easy_strerror(_));
	}

	::curl_easy_cleanup(curl);
	::curl_slist_free_all(headers);
	::curl_global_cleanup();

	return res;
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

	// set rapidyaml to use exceptions
	ryml::set_callbacks(ryml_error_handler.callbacks());

	// read plugin, action, and context (and assert validity)
	// note: list and help are handled by parse_llmq_args
	auto a = parse_llmq_args(argc, argv);

	// handle any action that does not require the plugin to be initialized
	switch (a.action) {
		case query: {
			// initialize the plugin
			init_plugin(argc, argv, a,
			            a.context.empty() ? std::string{}
			                              : read_context(prepare_ctxfile(a)),
			            prepare_authfile(a));

			bool retry = a.retry;

			// make the request without saving context
			while (!request(a.plugin, a.verbose, [&a, &retry](std::string_view reply) {
				try {
					// update plugin and print deltas
					plugop(a.plugin->name(), "process reply using",
					       [&a, &reply] {
						       a.plugin->onreply(reply, true);
					       });
					return true;
				} catch (std::exception const& e) {
					if (retry) {
						retry = false;
						return false;
					} else {
						die(e.what());
					}
				}
			}))
				;

			try {
				// notify the plugin that the request has completed
				plugop(a.plugin->name(), "finalize", [&a] {
					a.plugin->onfinish(true);
				});
			} catch (std::exception const& e) {
				die(e.what());
			}
		} break;

		case chat: {
			context_writer wctx = init_plugctx(argc, argv, a);

			bool retry = a.retry;

			// make the request; save context each reply
			while (!request(
			    a.plugin, a.verbose, [&a, &wctx, &retry](std::string_view reply) {
				    try {
					    // update plugin and print deltas
					    plugop(a.plugin->name(), "process reply using",
					           [&a, &reply] {
							   a.plugin->onreply(reply, !a.quiet);
						   });
				    } catch (std::exception const& e) {
					    if (retry) {
						    retry = false;
						    return false;
					    } else {
						    die(e.what());
					    }
				    }

				    try {
					    wctx.overwrite(
						plugop(a.plugin->name(), "get context from", [&a] {
							return a.plugin->context();
						}));
				    } catch (std::exception const& e) {
					    die(e.what());
				    }

				    return true;
			    }))
				;

			try {
				// notify the plugin that the request has completed
				plugop(a.plugin->name(), "finalize", [&a] {
					a.plugin->onfinish(!a.quiet);
				});
			} catch (std::exception const& e) {
				die(e.what());
			}
		} break;

		case init: {
			if (a.context.empty())
				a.context = compute_tmpctx(a);
			context_writer wctx = init_plugctx(argc, argv, a);

			try {
				wctx.overwrite(plugop(a.plugin->name(), "get context from", [&a] {
					return a.plugin->context();
				}));
			} catch (std::exception const& e) {
				die(e.what());
			}
			std::cout << a.plugin->name() << "://" << a.context << '\n';
		} break;

		case edit: {
			fs::path f = prepare_ctxfile(a);
			return (spawn_editor(f.parent_path().parent_path(), f), 0);
		}

		case auth: {
			fs::path f = prepare_authfile(a);
			return (spawn_editor(f.parent_path().parent_path(), f), 0);
		}

		case path: {
			if (!a.context.empty())
				return (std::cout << compute_ctxfile(a).c_str() << '\n', 0);
			else
				return (std::cout << compute_datadir(a).c_str() << '\n', 0);
		}

		case del: {
			fs::path f = compute_ctxfile(a);
			if (fs::exists(f)) {
				fs::remove(f);
				if (fs::is_empty(f.parent_path())) // <dir>/llmq/PLUGIN
					fs::remove(f.parent_path());
				if (fs::is_empty(f.parent_path().parent_path())) // <dir>/llmq
					fs::remove(f.parent_path().parent_path());
			} else {
				die("invalid context path ", f);
			}
			return 0;
		}

		case kill: {
			fs::path f = compute_ctxfile(a);
			return (kill_ctx(a.verbose, f), 0);
		}

		case list: {
			return (print_plugins(), 0);
		}

		case help: {
			if (a.plugin) {
				return (std::cout << a.plugin->help() << '\n', 0);
			} else {
				return (std::cout << llmq::help << '\n', 0);
			}
		}

		default: break;
	}

	return 0;
}
