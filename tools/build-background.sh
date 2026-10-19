#!/usr/bin/env bash
# spot5 - Builds the background player payload (background/background.cpp) with
# the plain payload SDK toolchain, not the app's: a payload runs outside the
# app's sandbox, so it gets its own mbedTLS, bell and cspot objects from the same
# vendored sources and settings as tools/build-deps.sh.
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Output: build/spot5-background.elf (tools/build.sh copies it into assets/).
# Needs tools/build-deps.sh's mbedTLS source download (make deps runs it first).
# ponytail: rebuilds everything each run (~1 min), like build-deps.sh.

set -euo pipefail

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
sdk="$root/.deps/native/ps5-payload-sdk"
bell="$root/third_party/cspot/cspot/bell"
cspot="$root/third_party/cspot/cspot"
gen="$root/third_party/cspot/generated"
mbedtls="$root/.deps/src/mbedtls-3.6.4"
out="$root/build/background"
elf="$root/build/spot5-background.elf"

[[ -x $sdk/bin/prospero-clang++ ]] || { echo "payload SDK missing: run make deps" >&2; exit 2; }
[[ -f $mbedtls/include/mbedtls/ssl.h ]] || { echo "mbedTLS source missing: run make deps" >&2; exit 2; }

export PS5_PAYLOAD_SDK="$sdk"
cc="$sdk/bin/prospero-clang"
cxx="$sdk/bin/prospero-clang++"

# Same defines as the app (Makefile APP_DEFINITIONS, build-deps.sh common_defs):
# they change class layouts. The app's mbedTLS settings fit a payload too (its
# entropy comes from KERN_ARND, src/platform/entropy_ps5.c).
common_defs=(-DBELL_TASK_MIN_STACK=1048576 -DCSPOT_CDN_CHUNK_SIZE=262144 -DBELL_DISABLE_FMT -DBELL_DISABLE_REGEX
    -DPB_ENABLE_MALLOC -DPB_FIELD_32BIT
    "-DMBEDTLS_USER_CONFIG_FILE=\"$root/src/platform/mbedtls_ps5_config.h\"")
includes=(-I"$mbedtls/include" -I"$bell/main/io/include" -I"$bell/main/utilities/include"
    -I"$bell/main/platform" -I"$bell/external/nanopb" -I"$bell/external/nlohmann_json/include"
    -I"$cspot/include" -I"$gen" -I"$bell/external/tremor" -I"$bell/external/civetweb/include")
c_flags=(-std=c11 -O2 -w -ffunction-sections -fdata-sections -D__unix__ "${common_defs[@]}" "${includes[@]}")
cxx_flags=(-std=c++17 -O2 -w -fexceptions -frtti -ffunction-sections -fdata-sections
    "${common_defs[@]}" "${includes[@]}")

# Compiles $3.. into objects under $1 with compiler $2 and the flags after "--".
compile() {
    local dir=$1 compiler=$2
    shift 2
    local flags=()
    while [[ $1 != -- ]]; do flags+=("$1"); shift; done
    shift
    mkdir -p "$dir"
    local pids=() status=0
    for source in "$@"; do
        "$compiler" "${flags[@]}" -c "$source" -o "$dir/$(basename "$source").o" &
        pids+=($!)
        if (( ${#pids[@]} >= $(nproc) )); then
            wait "${pids[0]}" || status=1
            pids=("${pids[@]:1}")
        fi
    done
    for pid in "${pids[@]}"; do wait "$pid" || status=1; done
    return $status
}

rm -rf "$out"
echo "==> [background] mbedTLS" >&2
compile "$out/mbedtls" "$cc" "${c_flags[@]}" -- "$mbedtls"/library/*.c "$root/src/platform/entropy_ps5.c"

echo "==> [background] bell and cspot" >&2
bell_sources=(
    "$bell/main/io/TLSSocket.cpp"
    "$bell/main/io/SocketStream.cpp"
    "$bell/main/io/URLParser.cpp"
    "$bell/main/io/HTTPClient.cpp"
    "$bell/main/io/X509Bundle.cpp"
    "$bell/main/utilities/BellLogger.cpp"
    "$bell/main/utilities/BellUtils.cpp"
    "$bell/main/utilities/Crypto.cpp"
    "$bell/main/utilities/NanoPBHelper.cpp"
    "$bell/main/utilities/NanoPBExtensions.cpp"
    "$bell/main/platform/linux/WrappedSemaphore.cpp"
    "$bell/main/io/BellHTTPServer.cpp"
    "$bell/external/civetweb/CivetServer.cpp"
)
for name in AccessKeyFetcher ApResolve AuthChallenges LoginBlob MercurySession \
    PlainConnection Session ShannonConnection Shannon TimeProvider Utils \
    TrackQueue TrackReference CDNAudioFile PlaybackState SpircHandler TrackPlayer; do
    bell_sources+=("$cspot/src/$name.cpp")
done
compile "$out/bell" "$cxx" "${cxx_flags[@]}" -- "${bell_sources[@]}"
compile "$out/bell-c" "$cc" "${c_flags[@]}" -- "$bell/main/io/picohttpparser.c" "$bell/main/utilities/aes.c" \
    "$bell/external/civetweb/civetweb.c" \
    "$bell/external/nanopb/pb_common.c" "$bell/external/nanopb/pb_decode.c" \
    "$bell/external/nanopb/pb_encode.c" "$gen"/protobuf/*.pb.c \
    $(ls "$bell"/external/tremor/*.c | grep -v ivorbisfile_example)

echo "==> [background] payload" >&2
"$cxx" "${cxx_flags[@]}" -Wall \
    "-DCERT_BUNDLE_PATH=\"$root/assets/x509_bundle.bin\"" "-DTILE_ICON_PATH=\"$root/sce_sys/icon0.png\"" \
    -o "$elf" "$root/background/background.cpp" \
    -Wl,--start-group "$out"/bell/*.o "$out"/bell-c/*.o "$out"/mbedtls/*.o -Wl,--end-group \
    -lpthread -ldl
echo "background: $elf" >&2
