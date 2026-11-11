#!/usr/bin/env bash
set -euo pipefail

android_api_level="${ANDROID_API_LEVEL:-35}"
android_build_tools="${ANDROID_BUILD_TOOLS:-36.0.0}"
android_ndk_version="${ANDROID_NDK_VERSION:-29.0.14206865}"

# -----------------------------------------------
# ninja
# -----------------------------------------------
# `apt-get install ninja-build` was the original way of doing this, but the
# GitHub runners intermittently lose DNS for archive.ubuntu.com /
# security.ubuntu.com (packages.microsoft.com still resolves, so the box is not
# offline, just those CNAMEs), which aborted every build with `exit code 100`.
# Prefer whatever the image already ships, then fall back to the upstream
# release binary, and only touch apt as a last resort.
if command -v ninja > /dev/null; then
    echo "ninja already present: $(command -v ninja) ($(ninja --version))"
else
    echo "ninja missing, fetching the upstream release binary"
    ninja_zip="${RUNNER_TEMP:-/tmp}/ninja-linux.zip"
    if curl -fsSL --retry 5 --retry-delay 3 --retry-all-errors \
            -o "$ninja_zip" \
            https://github.com/ninja-build/ninja/releases/download/v1.13.2/ninja-linux.zip; then
        unzip -q -o "$ninja_zip" -d "${RUNNER_TEMP:-/tmp}/ninja-bin"
        sudo install -m 0755 "${RUNNER_TEMP:-/tmp}/ninja-bin/ninja" /usr/local/bin/ninja
        echo "installed from release: $(ninja --version)"
    else
        echo "release download failed, retrying through apt"
        sudo apt-get update || true
        sudo apt-get install -y ninja-build
    fi
fi

# -----------------------------------------------
# ccache
# -----------------------------------------------
# hendrikmuhs/ccache-action installs ccache with `apt-get install -y ccache`,
# which hits the same dead ubuntu mirrors as ninja did. Installing it here from
# the upstream static build keeps the action from ever reaching for apt; the
# workflow passes install: false so it just uses what we put on PATH.
if ! command -v ccache > /dev/null; then
    echo "ccache missing, fetching the upstream static build"
    ccache_tgz="${RUNNER_TEMP:-/tmp}/ccache.tar.gz"
    if curl -fsSL --retry 5 --retry-delay 3 --retry-all-errors \
            -o "$ccache_tgz" \
            https://github.com/ccache/ccache/releases/download/v4.14.3/ccache-4.14.3-linux-x86_64-musl-static.tar.gz; then
        tar -xzf "$ccache_tgz" -C "${RUNNER_TEMP:-/tmp}"
        sudo install -m 0755 "${RUNNER_TEMP:-/tmp}"/ccache-4.14.3-linux-x86_64-musl-static/ccache /usr/local/bin/ccache
        echo "ccache installed: $(ccache --version | head -1)"
    else
        echo "::warning::could not fetch ccache - the build will run without the compiler cache"
    fi
else
    echo "ccache already present: $(command -v ccache)"
fi

repo_root="$(cd "$(dirname "$0")/.." && pwd)"

if command -v sdkmanager > /dev/null; then
    set +o pipefail
    yes | sdkmanager --licenses > /dev/null
    set -o pipefail
    sdkmanager --install \
        "platforms;android-${android_api_level}" \
        "build-tools;${android_build_tools}" \
        "ndk;${android_ndk_version}"

    sdk_root="${ANDROID_SDK_ROOT:-${ANDROID_HOME:-}}"
    if [[ -n "${sdk_root:-}" ]]; then
        export ANDROID_NDK_HOME="${sdk_root}/ndk/${android_ndk_version}"
        if [[ -n "${GITHUB_ENV:-}" ]]; then
            echo "ANDROID_NDK_HOME=${ANDROID_NDK_HOME}" >> "${GITHUB_ENV}"
        fi
    fi
fi

# Only arm64-v8a is built (see android/app/build.gradle abiFilters), so the
# x64-android triplet is skipped: vcpkg's OpenSSL cannot assemble its x86
# SM3/SM4 code with NDK 29, and installing it doubled setup time for nothing.
pushd "$repo_root" > /dev/null
for triplet in arm64-android; do
    vcpkg install --triplet "$triplet"
done
popd > /dev/null
