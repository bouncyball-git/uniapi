// Copyright (C) 2026 bouncyball
// Licensed under the Apache License, Version 2.0; see the LICENSE file at the project's root.
// SPDX-License-Identifier: Apache-2.0

// What crosses between the app and the rest of the desktop, the platform-independent part:
// where a page's navigation goes, opening things with the system (checks, then the backend's
// SystemOpen/SystemReveal), and one instance per user (AppOptions::singleInstance).

#include "desktop.hpp"

#include "backend.hpp"

#include <uniapi/uniapi.hpp>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace uniapi {
namespace detail {

namespace fs = std::filesystem;

Destination DestinationOf(const std::string& origin, const std::string& url) {
    if (url.compare(0, origin.size(), origin) == 0 && (url.size() == origin.size() || url[origin.size()] == '/')) {
        return Destination::App;
    }
    const size_t colon = url.find(':');
    // No scheme (a relative URL would have been resolved by now): the page's own.
    if (colon == std::string::npos || colon == 0) return Destination::App;
    std::string scheme = url.substr(0, colon);
    for (char& c : scheme) c = char(std::tolower(static_cast<unsigned char>(c)));
    if (scheme.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789+-.") != std::string::npos) return Destination::App;
    if (scheme == "about" || scheme == "data" || scheme == "blob" || scheme == "javascript") return Destination::App;
    if (scheme == "file") return Destination::File;
    return Destination::Outside;
}

std::vector<FileFilter> DialogFilters(const std::string& name, const std::string& pattern,
                                      const std::vector<FileFilter>& more) {
    auto clean = [](std::string text) {
        std::replace(text.begin(), text.end(), '\n', ' ');
        std::replace(text.begin(), text.end(), '\r', ' ');
        const size_t start = text.find_first_not_of(" \t");
        if (start == std::string::npos) return std::string();
        return text.substr(start, text.find_last_not_of(" \t") + 1 - start);
    };
    std::vector<FileFilter> filters;
    auto add = [&](const std::string& n, const std::string& p) {
        std::string cleaned = clean(p);
        if (!cleaned.empty()) filters.push_back({clean(n), std::move(cleaned)});
    };
    add(name, pattern);
    for (const FileFilter& f : more) add(f.name, f.pattern);
    return filters;
}

std::string WebKitVersion(const std::string& bundleVersion) {
    const size_t dot = bundleVersion.find('.');
    const std::string major = bundleVersion.substr(0, dot);
    if (major.size() <= 3 || major.find_first_not_of("0123456789") != std::string::npos) return bundleVersion;
    return bundleVersion.substr(major.size() - 3);
}

std::string RuntimeVersion(const std::string& browserVersion) {
    const size_t start = browserVersion.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    return browserVersion.substr(start, browserVersion.find_first_of(" \t\r\n", start) - start);
}

int ColorValue(const std::string& color) {
    if (color.empty()) return -1;
    std::string hex = color.substr(color[0] == '#' ? 1 : 0);
    if (hex.size() == 3) hex = {hex[0], hex[0], hex[1], hex[1], hex[2], hex[2]};
    if (color[0] != '#' || hex.size() != 6 || hex.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos) {
        std::fprintf(stderr, "uniapi: backgroundColor \"%s\" is not \"#rrggbb\" or \"#rgb\": ignored\n", color.c_str());
        return -1;
    }
    return int(std::stoul(hex, nullptr, 16));
}

bool IsFolder(const std::string& path) {
    if (path.empty()) return false;
    std::error_code error;
    return fs::is_directory(fs::u8path(path.begin(), path.end()), error);
}

namespace {

// path made absolute (so it can't be read as a command-line option), and there.
std::string Existing(std::string_view path) {
    if (std::all_of(path.begin(), path.end(), [](char c) { return std::isspace(static_cast<unsigned char>(c)); })) {
        throw std::runtime_error("no path given");
    }
    const fs::path given = fs::u8path(path.begin(), path.end());
    std::error_code error;
    fs::path absolute = fs::absolute(given, error).lexically_normal();  // "a/./b/.." as "a/"
    if (error) throw std::runtime_error(std::string(path) + ": " + error.message());
    if (!absolute.has_filename() && absolute != absolute.root_path()) absolute = absolute.parent_path();  // no "/" at the end
    if (!fs::exists(absolute, error)) throw std::runtime_error(absolute.u8string() + ": no such file or folder");
    return absolute.u8string();
}

}  // namespace
}  // namespace detail

void openURL(std::string_view url) {
    const std::string target(url);
    const size_t colon = target.find(':');
    const bool scheme = colon != std::string::npos && colon > 0 && std::isalpha(static_cast<unsigned char>(target[0]))
                        && target.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789+-.", 0)
                               == colon;
    if (!scheme) throw std::runtime_error("can't open \"" + target + "\": not a URL with a scheme");
    try {
        detail::SystemOpen(target);
    } catch (const std::exception& e) {
        throw std::runtime_error("can't open " + target + ": " + e.what());
    }
}

void openPath(std::string_view path) {
    const std::string absolute = detail::Existing(path);
    try {
        detail::SystemOpen(absolute);
    } catch (const std::exception& e) {
        throw std::runtime_error("can't open " + absolute + ": " + e.what());
    }
}

void revealPath(std::string_view path) {
    const std::string absolute = detail::Existing(path);
    try {
        detail::SystemReveal(absolute);
    } catch (const std::exception& e) {
        throw std::runtime_error("can't show " + absolute + ": " + e.what());
    }
}

namespace detail {

// --- One instance ---------------------------------------------------------------------------
// The running app listens on a channel only its user can reach: a Unix-domain socket in the
// user's runtime folder (Linux: $XDG_RUNTIME_DIR, else a private folder in /tmp) or temporary
// folder (macOS: $TMPDIR, per user), a named pipe on Windows. A second launch connects, sends one
// line of JSON ({"args": [...], "dir": "..."}), waits for "ok", and ends.

namespace {

// FNV-1a, 64 bits: a short, stable name for the id (a socket path must stay short).
std::string Hash(const std::string& text) {
    uint64_t hash = 1469598103934665603ull;
    for (unsigned char c : text) {
        hash ^= c;
        hash *= 1099511628211ull;
    }
    char hex[17];
    std::snprintf(hex, sizeof hex, "%016llx", static_cast<unsigned long long>(hash));
    return hex;
}

std::string LaunchMessage() {
    std::error_code error;
    const fs::path dir = fs::current_path(error);
    json launch = {{"args", arguments()}, {"dir", error ? std::string() : dir.u8string()}};
    return launch.dump() + "\n";
}

// Reads one launch line; false if it isn't one.
bool ParseLaunch(const std::string& line, std::vector<std::string>& args, std::string& dir) {
    try {
        const json launch = json::parse(line);
        args = launch.at("args").get<std::vector<std::string>>();
        dir = launch.at("dir").get<std::string>();
        return true;
    } catch (...) {
        return false;
    }
}

}  // namespace

#ifdef _WIN32

namespace {

std::wstring Wide(const std::string& text) {
    if (text.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), int(text.size()), nullptr, 0);
    std::wstring wide(size_t(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), int(text.size()), wide.data(), size);
    return wide;
}

// The pipe for id and this user (pipes are machine-wide; the user's name keeps users apart, and
// the pipe's default security lets only its creator and administrators write to it).
std::wstring PipeName(const std::string& id) {
    wchar_t user[257] = L"";
    DWORD size = 257;
    if (!GetUserNameW(user, &size)) user[0] = L'\0';
    std::string userName;
    if (const int bytes = WideCharToMultiByte(CP_UTF8, 0, user, -1, nullptr, 0, nullptr, nullptr); bytes > 1) {
        userName.resize(size_t(bytes));
        WideCharToMultiByte(CP_UTF8, 0, user, -1, userName.data(), bytes, nullptr, nullptr);
        userName.pop_back();  // the terminating NUL
    }
    return L"\\\\.\\pipe\\uniapi-" + Wide(Hash("uniapi.singleInstance:" + id + ":" + userName));
}

bool HandOver(const std::wstring& pipe, const std::string& message) {
    if (!WaitNamedPipeW(pipe.c_str(), 1000)) return false;
    HANDLE handle = CreateFileW(pipe.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    bool ok = WriteFile(handle, message.data(), DWORD(message.size()), &written, nullptr) && written == message.size();
    char reply[3] = {};
    DWORD read = 0;
    ok = ok && ReadFile(handle, reply, sizeof reply, &read, nullptr) && read == 3 && std::string(reply, 3) == "ok\n";
    CloseHandle(handle);
    return ok;
}

}  // namespace

class InstanceListener {
public:
    InstanceListener(std::wstring pipe, HANDLE first, LaunchHandler received)
        : pipe_(std::move(pipe)), received_(std::move(received)) {
        thread_ = std::thread([this, first] { Serve(first); });
    }
    ~InstanceListener() {
        stop_ = true;
        // Wakes ConnectNamedPipe: a client of its own, which the loop sees and leaves.
        HANDLE wake = CreateFileW(pipe_.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (wake != INVALID_HANDLE_VALUE) CloseHandle(wake);
        if (thread_.joinable()) thread_.join();
    }

private:
    void Serve(HANDLE pipe) {
        while (pipe != INVALID_HANDLE_VALUE) {
            const bool connected = ConnectNamedPipe(pipe, nullptr) || GetLastError() == ERROR_PIPE_CONNECTED;
            if (stop_) {
                CloseHandle(pipe);
                return;
            }
            if (connected) {
                std::string line;
                char chunk[4096];
                DWORD read = 0;
                while (line.find('\n') == std::string::npos && line.size() < (1 << 20)
                       && ReadFile(pipe, chunk, sizeof chunk, &read, nullptr) && read > 0) {
                    line.append(chunk, read);
                }
                std::vector<std::string> args;
                std::string dir;
                if (ParseLaunch(line.substr(0, line.find('\n')), args, dir)) {
                    DWORD written = 0;
                    WriteFile(pipe, "ok\n", 3, &written, nullptr);
                    FlushFileBuffers(pipe);
                    received_(std::move(args), std::move(dir));
                }
                DisconnectNamedPipe(pipe);
            }
        }
    }

    std::wstring pipe_;
    LaunchHandler received_;
    std::atomic<bool> stop_{false};
    std::thread thread_;
};

InstanceHandle StartSingleInstance(const std::string& id, LaunchHandler received, bool& handedOver) {
    handedOver = false;
    const std::wstring pipe = PipeName(id);
    const std::string message = LaunchMessage();
    if (HandOver(pipe, message)) {
        handedOver = true;
        return nullptr;
    }
    // The first instance of this pipe: another app creating it meanwhile makes this fail.
    HANDLE first = CreateNamedPipeW(pipe.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE,
                                    PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 4096,
                                    4096, 0, nullptr);
    if (first == INVALID_HANDLE_VALUE) {
        if (HandOver(pipe, message)) {  // another started listening just now
            handedOver = true;
            return nullptr;
        }
        std::fprintf(stderr, "uniapi: single instance off: no pipe (error %lu)\n", GetLastError());
        return nullptr;
    }
    return InstanceHandle(new InstanceListener(pipe, first, std::move(received)));
}

#else

namespace {

// The socket for id: the user's own folder, named by a hash of id.
std::string SocketPath(const std::string& id, std::string& why) {
    const std::string name = "uniapi-" + Hash("uniapi.singleInstance:" + id) + ".sock";
    std::string dir;
#ifdef __APPLE__
    const char* temp = std::getenv("TMPDIR");
    dir = temp && *temp ? temp : "/tmp";
#else
    if (const char* runtime = std::getenv("XDG_RUNTIME_DIR"); runtime && *runtime) {
        dir = runtime;
    } else {
        dir = "/tmp/uniapi-" + std::to_string(getuid());
        mkdir(dir.c_str(), 0700);
        struct stat info = {};
        if (lstat(dir.c_str(), &info) != 0 || !S_ISDIR(info.st_mode) || info.st_uid != getuid()
            || (info.st_mode & 0777) != 0700) {
            why = dir + " isn't private to this user";
            return {};
        }
    }
#endif
    while (dir.size() > 1 && dir.back() == '/') dir.pop_back();
    const std::string path = dir + "/" + name;
    if (path.size() >= sizeof(sockaddr_un{}.sun_path)) {
        why = "the socket's path is too long: " + path;
        return {};
    }
    return path;
}

sockaddr_un Address(const std::string& path) {
    sockaddr_un address = {};
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
    return address;
}

void Deadline(int fd, int seconds) {
    timeval time = {seconds, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &time, sizeof time);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &time, sizeof time);
}

bool SendAll(int fd, const std::string& text) {
    size_t sent = 0;
    while (sent < text.size()) {
#ifdef MSG_NOSIGNAL
        const ssize_t n = send(fd, text.data() + sent, text.size() - sent, MSG_NOSIGNAL);
#else
        const ssize_t n = send(fd, text.data() + sent, text.size() - sent, 0);
#endif
        if (n <= 0) return false;
        sent += size_t(n);
    }
    return true;
}

std::string ReadLine(int fd) {
    std::string line;
    char chunk[4096];
    while (line.find('\n') == std::string::npos && line.size() < (1 << 20)) {
        const ssize_t n = recv(fd, chunk, sizeof chunk, 0);
        if (n <= 0) break;
        line.append(chunk, size_t(n));
    }
    const size_t end = line.find('\n');
    return end == std::string::npos ? std::string() : line.substr(0, end);
}

int NewSocket() {
    const int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd >= 0) {
        fcntl(fd, F_SETFD, FD_CLOEXEC);
#ifdef SO_NOSIGPIPE
        const int on = 1;
        setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof on);
#endif
    }
    return fd;
}

// Gives the running instance this launch; false if none answers.
bool HandOver(const std::string& path, const std::string& message) {
    const int fd = NewSocket();
    if (fd < 0) return false;
    Deadline(fd, 3);
    const sockaddr_un address = Address(path);
    bool ok = connect(fd, reinterpret_cast<const sockaddr*>(&address), sizeof address) == 0 && SendAll(fd, message)
              && ReadLine(fd) == "ok";
    close(fd);
    return ok;
}

}  // namespace

class InstanceListener {
public:
    InstanceListener(std::string path, int fd, LaunchHandler received)
        : path_(std::move(path)), fd_(fd), received_(std::move(received)) {
        if (pipe(wake_) == 0) {
            fcntl(wake_[0], F_SETFD, FD_CLOEXEC);
            fcntl(wake_[1], F_SETFD, FD_CLOEXEC);
        }
        thread_ = std::thread([this] { Serve(); });
    }
    ~InstanceListener() {
        if (wake_[1] >= 0) {
            const char stop = 1;
            (void)!write(wake_[1], &stop, 1);
        }
        if (thread_.joinable()) thread_.join();
        close(fd_);
        unlink(path_.c_str());
        if (wake_[0] >= 0) close(wake_[0]);
        if (wake_[1] >= 0) close(wake_[1]);
    }

private:
    void Serve() {
        for (;;) {
            pollfd watched[2] = {{fd_, POLLIN, 0}, {wake_[0], POLLIN, 0}};
            if (poll(watched, wake_[0] >= 0 ? 2 : 1, -1) < 0) {
                if (errno == EINTR) continue;
                return;
            }
            if (watched[1].revents) return;  // stopping
            if (!(watched[0].revents & POLLIN)) continue;
            const int client = accept(fd_, nullptr, nullptr);
            if (client < 0) continue;
            fcntl(client, F_SETFD, FD_CLOEXEC);
            Deadline(client, 3);
            std::vector<std::string> args;
            std::string dir;
            if (ParseLaunch(ReadLine(client), args, dir) && SendAll(client, "ok\n")) {
                received_(std::move(args), std::move(dir));
            }
            close(client);
        }
    }

    std::string path_;
    int fd_;
    int wake_[2] = {-1, -1};
    LaunchHandler received_;
    std::thread thread_;
};

InstanceHandle StartSingleInstance(const std::string& id, LaunchHandler received, bool& handedOver) {
    handedOver = false;
    std::string why;
    const std::string path = SocketPath(id, why);
    if (path.empty()) {
        std::fprintf(stderr, "uniapi: single instance off: %s\n", why.c_str());
        return nullptr;
    }
    const std::string message = LaunchMessage();
    if (HandOver(path, message)) {
        handedOver = true;
        return nullptr;
    }
    unlink(path.c_str());  // stale: nobody answered there (an app that ended without removing it)
    const int fd = NewSocket();
    const sockaddr_un address = Address(path);
    const mode_t mask = umask(0077);  // the socket itself private too
    const bool bound = fd >= 0 && bind(fd, reinterpret_cast<const sockaddr*>(&address), sizeof address) == 0;
    umask(mask);
    if (!bound || listen(fd, 8) != 0) {
        const int error = errno;
        if (fd >= 0) close(fd);
        if (HandOver(path, message)) {  // another started listening just now
            handedOver = true;
            return nullptr;
        }
        std::fprintf(stderr, "uniapi: single instance off: %s: %s\n", path.c_str(), std::strerror(error));
        return nullptr;
    }
    return InstanceHandle(new InstanceListener(path, fd, std::move(received)));
}

#endif

void InstanceListenerDeleter::operator()(InstanceListener* listener) const { delete listener; }

}  // namespace detail
}  // namespace uniapi
