#!/usr/bin/env bash
# Builds uniapi on Linux or macOS, and cleans, tests, installs or uninstalls it.
#
#   ./build.sh [action...] [--prefix=<dir>] [--config=Release|Debug] [--gtk=auto|4|3]
#              [--build-dir=<dir>]
#
# Runs the actions given, in order; with none, it builds:
#
#   clean      deletes the build folders: "build" and every "build-*" holding a CMake build
#              (build-gtk3, or one made by hand, e.g. build-asan). With --gtk or
#              --build-dir: only that build's folder
#   build      configures (if needed) and builds everything: library, examples, tests
#   test       runs the tests (ctest; builds first)
#   install    installs the example apps (builds first). Linux: to <prefix>/bin, uniapi's web
#              process extension to <prefix>/lib/<app>/uniapi-webext, and each app's desktop
#              entry and icon to <prefix>/share. macOS: each as <prefix>/<app>.app
#   uninstall  removes what install put in <prefix>
#   clean-profiles
#              deletes the web engine profiles (caches, storage) of the programs this
#              project builds, also under old program names. Linux: ~/.local/share/<program>
#              and ~/.cache/<program> (XDG_DATA_HOME, XDG_CACHE_HOME if set). macOS:
#              ~/Library/WebKit/<program> and ~/Library/Caches/<program>, and the same for
#              org.uniapi.<program> (the installed .app's). It lists them and asks first (--yes
#              doesn't ask). A folder holding anything WebKit doesn't create is skipped: it may
#              be another app's. Each program makes a fresh profile when
#              it next starts.
#   help       shows this help (also -h, --help)
#
# install keeps a list of the files it installed in <prefix>/uniapi-install-manifest.txt;
# uninstall removes exactly those, then the folders left empty. So uninstall works after
# clean too, given the same --prefix.
#
# Options:
#   --prefix=<dir>     where install puts the apps; default: "install" in the repository.
#                      On macOS, ~/Applications or /Applications puts the apps where
#                      Launchpad and Spotlight find them.
#                      For a system folder like /usr/local, install and uninstall ask for
#                      sudo themselves (the build still runs as you).
#   --config=<c>       Release (default) or Debug
#   --gtk=<v>          Linux: auto (default: GTK 4 + webkitgtk-6.0 if installed, else GTK 3), 4 or 3
#   --build-dir=<dir>  default: "build", or "build-gtk3" with --gtk=3
#   --yes              clean-profiles deletes without asking
#
# Examples:
#   ./build.sh
#   ./build.sh clean build test
#   ./build.sh --gtk=3 build test
#   ./build.sh install --prefix=/usr/local
#   ./build.sh uninstall --prefix=/usr/local
#   ./build.sh clean-profiles
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
manifest_name="uniapi-install-manifest.txt"
# The programs this project builds, whose profiles clean-profiles removes.
programs=(hello hello_c uniapi_stream uniapi_notify uniapi_tray uniapi_logo uniapi_test
          uniapi_test_c uniapi_windows_test uniapi_lifecycle_test uniapi_desktop_test uniapi_placement_test uniapi_tray_test uniapi_strict_test uniapi_modal_test
          uniapi_checker uniapi_checker_c)
# What WebKitGTK puts in a profile's data and cache folders; a folder with anything else is
# left alone.
webkit_names=(WebKitCache CacheStorage applications offlineapps localstorage databases storage
              indexeddb mediakeys hsts-storage.sqlite deviceidhashsalts itp
              resourceloadstatistics serviceworkers cookies.sqlite)
# macOS's WebKit: WebsiteData (the default profile), WebsiteDataStore (those named by
# AppOptions::dataDirectory) and, in the cache folder, WebKit.
if [[ "$(uname -s)" == Darwin ]]; then
    os=macos
    webkit_names=(WebsiteData WebsiteDataStore WebKit)
else
    os=linux
fi
webkit_entries=" ${webkit_names[*]} "  # one line, a space around each name, for matching
actions=()
prefix=""
config="Release"
gtk="auto"
build_dir=""
named=0  # --gtk or --build-dir given: clean deletes only that build's folder
yes=0

usage() { sed -n '2,/^set -euo/p' "${BASH_SOURCE[0]}" | sed '$d' | sed 's/^# \{0,1\}//'; }

for arg in "$@"; do
    case "$arg" in
        clean|build|test|install|uninstall|clean-profiles) actions+=("$arg") ;;
        --prefix=*) prefix="${arg#*=}" ;;
        --config=*) config="${arg#*=}" ;;
        --gtk=*) gtk="${arg#*=}"; named=1 ;;
        --build-dir=*) build_dir="${arg#*=}"; named=1 ;;
        --yes) yes=1 ;;
        help|-h|--help) usage; exit 0 ;;
        *) echo "unknown argument: $arg" >&2; usage >&2; exit 2 ;;
    esac
done
[[ ${#actions[@]} -eq 0 ]] && actions=(build)
case "$config" in Release|Debug) ;; *) echo "--config must be Release or Debug" >&2; exit 2 ;; esac
case "$gtk" in auto|4|3) ;; *) echo "--gtk must be auto, 4 or 3" >&2; exit 2 ;; esac
if [[ -z "$build_dir" ]]; then
    if [[ "$gtk" == 3 ]]; then build_dir="$root/build-gtk3"; else build_dir="$root/build"; fi
fi
[[ -z "$prefix" ]] && prefix="$root/install"
# An absolute path, . and .. resolved, whether it exists or not (as GNU realpath -m; macOS's
# realpath has no -m).
absolute() {
    local path="$1" part out=() parts=()
    [[ "$path" == /* ]] || path="$PWD/$path"
    IFS=/ read -r -a parts <<< "$path"
    for part in ${parts[@]+"${parts[@]}"}; do
        case "$part" in
            ''|.) ;;
            ..) [[ ${#out[@]} -gt 0 ]] && unset "out[$(( ${#out[@]} - 1 ))]" ;;
            *) out+=("$part") ;;
        esac
    done
    if [[ ${#out[@]} -eq 0 ]]; then echo /; else printf '/%s' "${out[@]}"; echo; fi
}
# Absolute paths (relative ones are relative to the current folder).
build_dir="$(absolute "$build_dir")"
prefix="$(absolute "$prefix")"

step() { printf '\033[36m==> %s\033[0m\n' "$*"; }

# Runs a command with sudo if the folder it writes to (or its nearest existing parent) isn't
# writable by us, e.g. /usr/local.
as_needed() {
    local dir="$prefix"
    while [[ ! -e "$dir" ]]; do dir="$(dirname "$dir")"; done
    if [[ -w "$dir" ]]; then "$@"; else sudo "$@"; fi
}

# The folder named (--gtk, --build-dir), else every build folder in the repository: "build",
# and each "build-*" that holds a CMake build (CMakeCache.txt), so a folder that only happens
# to have such a name stays.
do_clean() {
    local dirs=() dir
    if [[ $named == 1 ]]; then
        [[ -e "$build_dir" ]] && dirs=("$build_dir")
    else
        for dir in "$root/build" "$root"/build-*; do
            [[ -d "$dir" ]] || continue
            [[ "$dir" == "$root/build" || -f "$dir/CMakeCache.txt" ]] && dirs+=("$dir")
        done
    fi
    if [[ ${#dirs[@]} -eq 0 ]]; then
        step "clean: no build folder to delete"
        return
    fi
    for dir in "${dirs[@]}"; do
        step "clean: deleting $dir"
        rm -rf -- "$dir"
    done
}

do_build() {
    local generator=()
    # Ninja when installed; otherwise CMake's default (make). An existing build folder keeps
    # the generator it was made with.
    if [[ ! -f "$build_dir/CMakeCache.txt" ]] && command -v ninja >/dev/null; then generator=(-G Ninja); fi
    local toolkit=(-DUNIAPI_GTK="$gtk") what="GTK $gtk"
    [[ $os == macos ]] && { toolkit=(); what="macOS"; }
    step "configure ($config, $what) in $build_dir"
    cmake -S "$root" -B "$build_dir" ${generator[@]+"${generator[@]}"} -DCMAKE_BUILD_TYPE="$config" ${toolkit[@]+"${toolkit[@]}"}
    step "build"
    cmake --build "$build_dir" --parallel
}

do_test() {
    step "test"
    ctest --test-dir "$build_dir" --output-on-failure
}

do_install() {
    step "install to $prefix"
    as_needed cmake --install "$build_dir" --prefix "$prefix"
    # Keep CMake's list of installed files with the install, for uninstall.
    local manifest="$build_dir/install_manifest.txt"
    [[ -f "$manifest" ]] || { echo "CMake wrote no $manifest" >&2; exit 1; }
    as_needed cp "$manifest" "$prefix/$manifest_name"
    sed 's/^/    /' "$manifest"
    [[ -z "$(tail -c 1 "$manifest")" ]] || echo  # CMake ends the list without a newline
}

do_uninstall() {
    local manifest="$prefix/$manifest_name"
    if [[ ! -f "$manifest" ]]; then
        step "uninstall: nothing installed in $prefix (no $manifest_name)"
        return
    fi
    step "uninstall from $prefix"
    local file folders=()
    while IFS= read -r file || [[ -n "$file" ]]; do
        [[ -z "$file" ]] && continue
        if [[ -f "$file" || -L "$file" ]]; then
            as_needed rm -f -- "$file"
            echo "    removed $file"
        fi
        folders+=("$(dirname "$file")")
    done < "$manifest"
    as_needed rm -f -- "$manifest"
    # Remove the folders the install created, if now empty: deepest first, up to the prefix.
    local folder all=()
    for folder in "${folders[@]}" "$prefix"; do
        while [[ "$folder" == "$prefix" || "$folder" == "$prefix"/* ]]; do
            all+=("$folder")
            [[ "$folder" == "$prefix" ]] && break
            folder="$(dirname "$folder")"
        done
    done
    while IFS= read -r folder; do
        if [[ -d "$folder" && -z "$(ls -A -- "$folder")" ]]; then
            as_needed rmdir -- "$folder"
            echo "    removed folder $folder"
        fi
    done < <(printf '%s\n' "${all[@]}" | sort -u | awk '{ print length, $0 }' | sort -rn | cut -d' ' -f2-)
}

do_clean_profiles() {
    local data_home="${XDG_DATA_HOME:-$HOME/.local/share}"
    local cache_home="${XDG_CACHE_HOME:-$HOME/.cache}"
    local name dir entry found=() foreign candidates
    for name in "${programs[@]}"; do
        if [[ $os == macos ]]; then
            candidates=("$HOME/Library/WebKit/$name" "$HOME/Library/Caches/$name"
                        "$HOME/Library/WebKit/org.uniapi.$name" "$HOME/Library/Caches/org.uniapi.$name")
        else
            candidates=("$data_home/$name" "$cache_home/$name")
        fi
        for dir in "${candidates[@]}"; do
            [[ -d "$dir" ]] || continue
            foreign=""
            while IFS= read -r entry; do
                [[ "$webkit_entries" == *" $entry "* ]] || foreign="$entry"
            done < <(ls -A -- "$dir")
            if [[ -z "$(ls -A -- "$dir")" ]]; then
                echo "    skipped $dir (empty: doesn't look like a WebKit profile)"
                continue
            elif [[ -n "$foreign" ]]; then
                echo "    skipped $dir (has $foreign: doesn't look like a WebKit profile)"
                continue
            fi
            found+=("$dir")
        done
    done
    if [[ ${#found[@]} -eq 0 ]]; then
        step "clean-profiles: no profiles found"
        return
    fi
    step "clean-profiles"
    for dir in "${found[@]}"; do echo "    $dir  ($(du -sh -- "$dir" | cut -f1))"; done
    if [[ $yes != 1 ]]; then
        local answer=""
        read -r -p "Delete these? [y/N] " answer || true
        if [[ ! "$answer" =~ ^([yY]|[yY][eE][sS])$ ]]; then
            echo "    nothing deleted"
            return
        fi
    fi
    for dir in "${found[@]}"; do
        if rm -rf -- "$dir"; then echo "    removed $dir"; else echo "    could not remove $dir"; fi
    done
}

built=0
for action in "${actions[@]}"; do
    case "$action" in
        clean) do_clean; built=0 ;;
        build) do_build; built=1 ;;
        test) [[ $built == 1 ]] || { do_build; built=1; }; do_test ;;
        install) [[ $built == 1 ]] || { do_build; built=1; }; do_install ;;
        uninstall) do_uninstall ;;
        clean-profiles) do_clean_profiles ;;
    esac
done
step "done"
