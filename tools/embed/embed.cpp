// uniapi_embed_tool: writes the C++ file uniapi_embed() compiles in (cmake/UniapiEmbed.cmake).
// It runs at build time on the machine that builds, in one pass over each file, so embedding
// costs milliseconds per megabyte (the CMake-only way, still the fallback when cross-compiling,
// costs seconds).
//
//   uniapi_embed_tool <name> <directory> <list> <output.cpp> [--minify <esbuild>]
//                     [--minify-markup <minify>] [--esbuild <esbuild>] [--bundle <entry>]...
//                     [--keep <pattern>]... [--compress] [--incbin]
//
// <list> names the files under <directory>, one per line. The output defines the AssetTable
// <name> (uniapi/assets.hpp) and uniapi_assets_<name>() for C.
//   --minify    .js and .css files are minified with this esbuild first (into <output.cpp>.min;
//               no bundling, no new syntax: it keeps what the files are, smaller): the web engine
//               parses less, and less is stored. esbuild runs on batches of files, without a shell.
//   --minify-markup .html/.htm, .svg and .json files are minified with this tdewolff/minify (one
//               run per file, into the same place); HTML keeps its document and end tags.
//   --bundle    <entry> (a .js, .mjs or .css file of the list) is bundled with esbuild: the entry
//               and everything it imports become one file, embedded under the entry's path (a
//               script as an ES module), minified if --minify is given. The listed files that
//               went into a bundle are not embedded by themselves: the bundle holds them. May be
//               given several times. Needs esbuild: --minify's, or the one --esbuild names.
//   --keep      files whose name matches <pattern> (* any characters, ? one, [a-z] a set) are
//               embedded as they are, not minified. May be given several times.
//   --compress  files that shrink by a tenth or more are stored zlib-compressed (Asset::packedSize);
//               the library inflates them once when first used (uniapi::assetData)
//   --incbin    the bytes come in through the assembler's .incbin (GCC and Clang on ELF, i.e.
//               Linux) instead of as C++ byte arrays: no compile time per megabyte. Compressed
//               files are written next to <output.cpp> for it.

#include "miniz.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>
#include <vector>

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {

// Paths are UTF-8 (CMake's); u8path keeps every character on Windows too.
std::filesystem::path PathOf(const std::string& utf8) { return std::filesystem::u8path(utf8); }

bool ReadFile(const std::string& path, std::vector<unsigned char>& out) {
    std::ifstream in(PathOf(path), std::ios::binary);
    if (!in) return false;
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}

bool WriteFile(const std::string& path, const void* data, size_t size) {
    std::ofstream out(PathOf(path), std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
    return static_cast<bool>(out.flush());
}

bool Exists(const std::string& path) {
    std::error_code error;
    return std::filesystem::is_regular_file(PathOf(path), error);
}

// Runs a program with these arguments, no shell between (spaces and quotes in paths stay as they
// are); its exit code, or -1 if it couldn't start.
int Run(const std::vector<std::string>& args) {
#ifdef _WIN32
    // One command line, each argument quoted as CommandLineToArgvW reads it back.
    auto wide = [](const std::string& text) {
        std::wstring out(MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), static_cast<int>(out.size()));
        return out;
    };
    std::wstring line;
    for (const std::string& arg : args) {
        const std::wstring w = wide(arg);
        if (!line.empty()) line += L' ';
        if (!w.empty() && w.find_first_of(L" \t\n\v\"") == std::wstring::npos) {
            line += w;
            continue;
        }
        line += L'"';
        size_t backslashes = 0;
        for (wchar_t c : w) {
            if (c == L'\\') {
                ++backslashes;
                continue;
            }
            line.append(c == L'"' ? backslashes * 2 + 1 : backslashes, L'\\');
            backslashes = 0;
            line += c;
        }
        line.append(backslashes * 2, L'\\');
        line += L'"';
    }
    STARTUPINFOW startup = {};
    startup.cb = sizeof startup;
    PROCESS_INFORMATION process = {};
    if (!CreateProcessW(nullptr, line.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &process)) return -1;
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return static_cast<int>(code);
#else
    std::vector<char*> argv;
    for (const std::string& arg : args) argv.push_back(const_cast<char*>(arg.c_str()));
    argv.push_back(nullptr);
    const pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        execv(argv[0], argv.data());
        _exit(127);
    }
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) return -1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
}

bool EndsWith(const std::string& text, const char* suffix) {
    const size_t n = std::strlen(suffix);
    if (text.size() < n) return false;
    for (size_t i = 0; i < n; ++i) {
        char c = text[text.size() - n + i];
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        if (c != suffix[i]) return false;
    }
    return true;
}

// Whether `name` matches `pattern`: * is any characters, ? one, [abc] or [a-z] one of a set
// ([^...] one not of it); anything else is itself. As Go's path.Match for a file's name.
bool Matches(const char* pattern, const char* name) {
    for (; *pattern; ++pattern, ++name) {
        if (*pattern == '*') {
            while (*pattern == '*') ++pattern;
            if (!*pattern) return true;
            for (; *name; ++name) {
                if (Matches(pattern, name)) return true;
            }
            return false;
        }
        if (!*name) return false;
        if (*pattern == '?') continue;
        if (*pattern == '[') {
            const char* p = pattern + 1;
            const bool negated = *p == '^' || *p == '!';
            if (negated) ++p;
            bool in = false;
            const char* first = p;
            for (; *p && (*p != ']' || p == first); ++p) {
                if (p[1] == '-' && p[2] && p[2] != ']') {
                    if (*name >= p[0] && *name <= p[2]) in = true;
                    p += 2;
                } else if (*name == *p) {
                    in = true;
                }
            }
            if (!*p) return false;  // no closing ]: matches nothing
            if (in == negated) return false;
            pattern = p;
            continue;
        }
        if (*pattern != *name) return false;
    }
    return !*name;
}

// Whether a file is kept as it is (--keep): its name matches one of the patterns.
bool Kept(const std::vector<std::string>& keep, const std::string& path) {
    const size_t slash = path.find_last_of('/');
    const std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
    for (const std::string& pattern : keep) {
        if (Matches(pattern.c_str(), name.c_str())) return true;
    }
    return false;
}

// Minifies the .html/.htm, .svg and .json files among `paths` with tdewolff/minify into `stage`
// (the same relative paths), a run per file with its type given. False if minify failed.
bool MinifyMarkup(const std::string& minify, const std::string& directory, const std::vector<std::string>& paths,
                  const std::vector<std::string>& keep, const std::string& stage) {
    for (const std::string& path : paths) {
        if (Kept(keep, path)) continue;
        const char* type = EndsWith(path, ".html") || EndsWith(path, ".htm") ? "html"
                         : EndsWith(path, ".svg")                             ? "svg"
                         : EndsWith(path, ".json")                            ? "json"
                                                                              : nullptr;
        if (!type) continue;
        const std::string out = stage + "/" + path;
        std::error_code error;
        std::filesystem::create_directories(PathOf(out).parent_path(), error);
        std::vector<std::string> args = {minify, std::string("--type=") + type};
        if (std::strcmp(type, "html") == 0) {
            args.push_back("--html-keep-document-tags");
            args.push_back("--html-keep-end-tags");
        }
        args.insert(args.end(), {"-o", out, directory + "/" + path});
        const int code = Run(args);
        if (code != 0) {
            std::fprintf(stderr, "uniapi_embed_tool: minify failed (%d) on %s/%s\n", code, directory.c_str(), path.c_str());
            return false;
        }
    }
    return true;
}

// Minifies the .js and .css files among `paths` with esbuild into `stage` (the same relative
// paths), in batches that keep each command line short. False if esbuild failed.
bool Minify(const std::string& esbuild, const std::string& directory, const std::vector<std::string>& paths,
            const std::vector<std::string>& bundles, const std::vector<std::string>& keep, const std::string& stage) {
    std::error_code error;
    std::filesystem::create_directories(PathOf(stage), error);
    const std::vector<std::string> base = {esbuild, "--minify", "--charset=utf8", "--log-level=warning",
                                           "--outdir=" + stage, "--outbase=" + directory};
    std::vector<std::string> args = base;
    size_t length = 0;
    auto flush = [&]() {
        if (args.size() == base.size()) return true;
        const int code = Run(args);
        if (code != 0) {
            std::fprintf(stderr, "uniapi_embed_tool: esbuild failed (%d) minifying files under %s\n", code, directory.c_str());
            return false;
        }
        args = base;
        length = 0;
        return true;
    };
    for (const std::string& path : paths) {
        if (!EndsWith(path, ".js") && !EndsWith(path, ".css")) continue;
        if (std::find(bundles.begin(), bundles.end(), path) != bundles.end()) continue;  // bundled: in the stage already
        if (Kept(keep, path)) continue;
        args.push_back(directory + "/" + path);
        length += directory.size() + path.size() + 4;
        if (length > 24000 && !flush()) return false;  // Windows takes 32767 characters
    }
    return flush();
}

// A path as one text that is the same for the same file, however it was written (".." and "."
// taken out, made absolute); the files need not exist.
std::string Canonical(const std::filesystem::path& path) {
    std::error_code error;
    const std::filesystem::path canonical = std::filesystem::weakly_canonical(path, error);
    return (error ? path.lexically_normal() : canonical).generic_u8string();
}

// The files esbuild read for its bundles: the keys of "inputs" in the metafile it wrote
// ({"inputs":{"<path>":{...},...},"outputs":{...}}), which are paths from the folder esbuild ran
// in. Empty if the file can't be read as that.
std::vector<std::string> BundleInputs(const std::string& metafile) {
    std::vector<unsigned char> bytes;
    std::vector<std::string> inputs;
    if (!ReadFile(metafile, bytes)) return inputs;
    const std::string text(bytes.begin(), bytes.end());
    size_t at = text.find("\"inputs\"");
    if (at == std::string::npos) return inputs;
    at = text.find('{', at);
    if (at == std::string::npos) return inputs;
    // Inside the inputs object: a string at depth 1 that a ':' follows is a key.
    int depth = 0;
    for (size_t i = at; i < text.size(); ++i) {
        const char c = text[i];
        if (c == '{' || c == '[') {
            ++depth;
        } else if (c == '}' || c == ']') {
            if (--depth == 0) break;
        } else if (c == '"') {
            std::string value;
            for (++i; i < text.size() && text[i] != '"'; ++i) {
                if (text[i] != '\\' || i + 1 >= text.size()) {
                    value += text[i];
                    continue;
                }
                const char escaped = text[++i];
                if (escaped == 'u' && i + 4 < text.size()) {
                    // \uXXXX: esbuild writes paths as UTF-8, so only control characters come so.
                    const unsigned code = static_cast<unsigned>(std::strtoul(text.substr(i + 1, 4).c_str(), nullptr, 16));
                    if (code < 0x80) value += static_cast<char>(code);
                    i += 4;
                } else {
                    value += escaped == 'n' ? '\n' : escaped == 't' ? '\t' : escaped;
                }
            }
            size_t next = i + 1;
            while (next < text.size() && (text[next] == ' ' || text[next] == '\n' || text[next] == '\r' || text[next] == '\t')) ++next;
            if (depth == 1 && next < text.size() && text[next] == ':') inputs.push_back(value);
        }
    }
    return inputs;
}

// Bundles the entries with esbuild into `stage` (under their own paths): each with everything
// it imports, as one file. The listed files that went into a bundle, other than the entries
// themselves, are taken out of `paths`. False if esbuild failed.
bool Bundle(const std::string& esbuild, bool minify, const std::string& directory, const std::vector<std::string>& bundles,
            std::vector<std::string>& paths, const std::string& stage) {
    std::error_code error;
    std::filesystem::create_directories(PathOf(stage), error);
    const std::string metafile = stage + ".bundles.json";
    std::vector<std::string> args = {esbuild, "--bundle", "--format=esm"};
    if (minify) args.push_back("--minify");
    args.insert(args.end(), {"--charset=utf8", "--log-level=warning", "--outdir=" + stage, "--outbase=" + directory,
                             "--metafile=" + metafile});
    for (const std::string& entry : bundles) args.push_back(directory + "/" + entry);
    const int code = Run(args);
    if (code != 0) {
        std::fprintf(stderr, "uniapi_embed_tool: esbuild failed (%d) bundling files under %s\n", code, directory.c_str());
        return false;
    }
    // The metafile's paths are from the folder esbuild ran in, which is this program's.
    std::vector<std::string> inside;
    const std::filesystem::path here = std::filesystem::current_path(error);
    for (const std::string& input : BundleInputs(metafile)) {
        const std::filesystem::path path = PathOf(input);
        inside.push_back(Canonical(path.is_absolute() ? path : here / path));
    }
    std::filesystem::remove(PathOf(metafile), error);
    std::vector<std::string> kept;
    for (const std::string& path : paths) {
        const bool entry = std::find(bundles.begin(), bundles.end(), path) != bundles.end();
        const bool bundled = std::find(inside.begin(), inside.end(), Canonical(PathOf(directory + "/" + path))) != inside.end();
        if (entry || !bundled) kept.push_back(path);
    }
    paths.swap(kept);
    return true;
}

// Formats that are compressed already: compressing them again wastes build time for nothing.
bool CompressedAlready(const std::string& path) {
    static const char* const kTypes[] = {".png", ".jpg", ".jpeg", ".gif", ".webp", ".avif", ".ico", ".woff", ".woff2",
                                         ".zip", ".gz", ".br", ".zst", ".7z", ".mp3", ".mp4", ".m4a", ".ogg", ".opus",
                                         ".webm", ".flac", ".aac", ".mov", ".pdf"};
    std::string lower = path;
    for (char& c : lower) c = static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
    for (const char* type : kTypes) {
        const size_t n = std::strlen(type);
        if (lower.size() >= n && lower.compare(lower.size() - n, n, type) == 0) return true;
    }
    return false;
}

// A C++ string literal's body for `text` (\ and " escaped).
std::string Literal(const std::string& text) {
    std::string out;
    for (char c : text) {
        if (c == '\\' || c == '"') out += '\\';
        out += c;
    }
    return out;
}

// The bytes as lines of "0x..," (32 per line), for a C++ array.
void AppendBytes(std::string& out, const unsigned char* data, size_t size) {
    static const char kHex[] = "0123456789abcdef";
    out.reserve(out.size() + size * 5 + size / 32 + 16);
    for (size_t i = 0; i < size; ++i) {
        out += "0x";
        out += kHex[data[i] >> 4];
        out += kHex[data[i] & 15];
        out += ',';
        if (i % 32 == 31) out += '\n';
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 5) {
        std::fprintf(stderr, "usage: uniapi_embed_tool <name> <directory> <list> <output.cpp> [--minify <esbuild>] "
                             "[--minify-markup <minify>] [--esbuild <esbuild>] [--bundle <entry>]... [--keep <pattern>]... [--compress] [--incbin]\n");
        return 2;
    }
    const std::string name = argv[1];
    const std::string directory = argv[2];
    const std::string listPath = argv[3];
    const std::string output = argv[4];
    bool compress = false;
    bool incbin = false;
    std::string esbuild;         // minifies .js and .css, if given
    std::string minify;
    std::string bundler;         // the esbuild that bundles, if --minify gave none
    std::vector<std::string> bundles;
    std::vector<std::string> keep;
    for (int i = 5; i < argc; ++i) {
        if (std::strcmp(argv[i], "--compress") == 0) compress = true;
        else if (std::strcmp(argv[i], "--incbin") == 0) incbin = true;
        else if (std::strcmp(argv[i], "--minify") == 0 && i + 1 < argc) esbuild = argv[++i];
        else if (std::strcmp(argv[i], "--minify-markup") == 0 && i + 1 < argc) minify = argv[++i];
        else if (std::strcmp(argv[i], "--esbuild") == 0 && i + 1 < argc) bundler = argv[++i];
        else if (std::strcmp(argv[i], "--bundle") == 0 && i + 1 < argc) bundles.push_back(argv[++i]);
        else if (std::strcmp(argv[i], "--keep") == 0 && i + 1 < argc) keep.push_back(argv[++i]);
        else {
            std::fprintf(stderr, "uniapi_embed_tool: unknown option %s\n", argv[i]);
            return 2;
        }
    }

    std::vector<std::string> paths;
    {
        std::ifstream list(listPath);
        std::string line;
        while (std::getline(list, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (!line.empty()) paths.push_back(line);
        }
    }

    // Minified copies (.js and .css by esbuild, then .html, .svg and .json by minify), embedded
    // instead of the originals. The stage starts empty.
    const std::string stage = output + ".min";
    {
        std::error_code error;
        std::filesystem::remove_all(PathOf(stage), error);
    }
    if (!bundles.empty()) {
        if (!esbuild.empty()) bundler = esbuild;
        if (bundler.empty()) {
            std::fprintf(stderr, "uniapi_embed_tool: --bundle needs esbuild (--minify <esbuild> or --esbuild <esbuild>)\n");
            return 2;
        }
        for (const std::string& entry : bundles) {
            if (std::find(paths.begin(), paths.end(), entry) == paths.end()) {
                std::fprintf(stderr, "uniapi_embed_tool: --bundle %s is not a file under %s\n", entry.c_str(), directory.c_str());
                return 2;
            }
            if (!EndsWith(entry, ".js") && !EndsWith(entry, ".mjs") && !EndsWith(entry, ".css")) {
                std::fprintf(stderr, "uniapi_embed_tool: --bundle %s: a .js, .mjs or .css file\n", entry.c_str());
                return 2;
            }
        }
        if (!Bundle(bundler, !esbuild.empty(), directory, bundles, paths, stage)) return 1;
    }
    if (!esbuild.empty() && !Minify(esbuild, directory, paths, bundles, keep, stage)) return 1;
    if (!minify.empty() && !MinifyMarkup(minify, directory, paths, keep, stage)) return 1;

    std::string top =
        "// Generated by uniapi_embed() from " + directory + ". Do not edit.\n"
        "#include <uniapi/assets.hpp>\n#include <uniapi/uniapi.h>\n\n";
    std::string arrays;
    std::string entries;
    size_t total = 0;
    size_t stored = 0;
    for (size_t index = 0; index < paths.size(); ++index) {
        const std::string& path = paths[index];
        const std::string minified = stage + "/" + path;
        // The stage started empty: what is there is this run's minified or bundled copy.
        const std::string source = Exists(minified) ? minified : directory + "/" + path;
        std::vector<unsigned char> bytes;
        if (!ReadFile(source, bytes)) {
            std::fprintf(stderr, "uniapi_embed_tool: can't read %s\n", source.c_str());
            return 1;
        }
        // Compressed when it pays: at least 256 bytes, not compressed already, a tenth smaller.
        std::vector<unsigned char> packed;
        if (compress && bytes.size() >= 256 && !CompressedAlready(path)) {
            mz_ulong size = mz_compressBound(static_cast<mz_ulong>(bytes.size()));
            packed.resize(size);
            if (mz_compress2(packed.data(), &size, bytes.data(), static_cast<mz_ulong>(bytes.size()), MZ_BEST_COMPRESSION) == MZ_OK &&
                size <= bytes.size() - bytes.size() / 10) {
                packed.resize(size);
            } else {
                packed.clear();
            }
        }
        const unsigned char* data = packed.empty() ? bytes.data() : packed.data();
        const size_t dataSize = packed.empty() ? bytes.size() : packed.size();
        total += bytes.size();
        stored += dataSize;
        const std::string symbol = "uniapi_embed_" + name + "_" + std::to_string(index);

        if (incbin) {
            // The bytes from a file the assembler reads: the original, or the compressed copy.
            std::string file = source;
            if (!packed.empty()) {
                file = output + "." + std::to_string(index) + ".z";
                if (!WriteFile(file, packed.data(), packed.size())) {
                    std::fprintf(stderr, "uniapi_embed_tool: can't write %s\n", file.c_str());
                    return 1;
                }
            }
            // An assembler string inside a C++ string literal: escaped for the assembler, then
            // for C++.
            const std::string asmPath = Literal(Literal(file));
            top += "extern \"C\" const unsigned char " + symbol + "[];\n";
            arrays += "__asm__(\".section .rodata\\n.balign 16\\n.globl " + symbol + "\\n.hidden " + symbol + "\\n" + symbol +
                      ":\\n.incbin \\\"" + asmPath + "\\\"\\n.byte 0\\n.previous\\n\");\n";
        } else {
            // A trailing 0 keeps empty files valid C++; the size excludes it.
            arrays += "const unsigned char " + symbol + "[] = {\n";
            AppendBytes(arrays, data, dataSize);
            arrays += "0};\n";
        }
        entries += "    {\"" + Literal(path) + "\", " + symbol + ", " + std::to_string(bytes.size()) + ", " +
                   std::to_string(packed.empty() ? 0 : packed.size()) + "},\n";
    }

    std::string text = top;
    if (incbin) {
        text += arrays + "\nnamespace {\n";
    } else {
        text += "namespace {\n" + arrays;
    }
    text += "\nconst uniapi::Asset kAssets[] = {\n" + entries + "};\n}  // namespace\n\n"
            "extern const uniapi::AssetTable " + name + " = {kAssets, sizeof(kAssets) / sizeof(kAssets[0])};\n\n"
            "// The same table for C code (uniapi.h): UNIAPI_C_ASSETS(" + name + ") declares this.\n"
            "extern \"C\" const uniapi_assets* uniapi_assets_" + name + "(void) {\n"
            "    return reinterpret_cast<const uniapi_assets*>(&" + name + ");\n}\n";
    // Always written, even when unchanged: with --incbin the compiler must run again when a
    // file it includes changed.
    if (!WriteFile(output, text.data(), text.size())) {
        std::fprintf(stderr, "uniapi_embed_tool: can't write %s\n", output.c_str());
        return 1;
    }
    std::printf("uniapi_embed_tool: %s: %zu files, %zu bytes%s, stored %zu", name.c_str(), paths.size(), total,
                esbuild.empty() && minify.empty() ? "" : " minified", stored);
    if (!bundles.empty()) std::printf(", %zu bundled", bundles.size());
    std::printf("\n");
    return 0;
}
