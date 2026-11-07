#!/usr/bin/env bash
set -euo pipefail

OUTPUT_DIR="${1:-}"
if [[ -z "$OUTPUT_DIR" ]]; then
    echo "Usage: $0 <output-dir>" >&2
    exit 1
fi

mkdir -p android/app/assets
rm -rf android/app/assets/data android/app/assets/shaders-builtin
cp -r data android/app/assets/data
cp -r vita3k/shaders-builtin android/app/assets/shaders-builtin

chmod +x android/gradlew

if [[ -e "${SIGNING_STORE_PATH:-}" ]]; then
    BUILD_TYPE="Release"
else
    BUILD_TYPE="Reldebug"
fi

pushd android > /dev/null
./gradlew --stacktrace ":app:assemble${BUILD_TYPE}"
popd > /dev/null

APK_PATH="android/app/build/outputs/apk/${BUILD_TYPE,,}/app-${BUILD_TYPE,,}.apk"

# MT Manager's document provider goes in here rather than in the source tree: it
# is a manifest patch plus an extra dex, and the patch has to match the
# versionName and resource ids of the APK that was just built. Doing it on the
# build machine also keeps the project certificate on the result, so the APK
# still installs over the previous build. Set MT_INJECT=0 to ship without it.
if [[ "${MT_INJECT:-1}" == "1" ]]; then
    if [[ "$BUILD_TYPE" == "Release" && -e "${SIGNING_STORE_PATH:-}" ]]; then
        echo "Injecting the MT document provider"
        python3 ci/mt-inject/inject.py --apk "$APK_PATH" \
            --ks "$SIGNING_STORE_PATH" --alias "${SIGNING_KEY_ALIAS:-vita3kuc}"
    else
        echo "Skipping the MT document provider: unsigned build"
    fi
fi

mkdir -p "$OUTPUT_DIR"
cp "$APK_PATH" "$OUTPUT_DIR/app.apk"
