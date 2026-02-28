#!/bin/bash

set -euo pipefail

usage() {
    cat << EOF
Usage: $(basename "$0") [options]
Options:
  -k, --ksu [y/N]         Forward to build.sh
  -r, --recovery [y/N]    Forward to build.sh
  -g, --gpu-max [value]   Forward to build.sh
EOF
}

KSU_OPTION=""
RECOVERY_OPTION=""
GPU_MAX=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        --ksu|-k)
            KSU_OPTION="$2"
            shift 2
            ;;
        --recovery|-r)
            RECOVERY_OPTION="$2"
            shift 2
            ;;
        --gpu-max|-g)
            GPU_MAX="$2"
            shift 2
            ;;
        --help|-h)
            usage
            exit 0
            ;;
        *)
            usage
            exit 1
            ;;
    esac
done

pushd "$(dirname "$0")" > /dev/null

MODELS=(
    beyond0lte
    beyond0lteks
    beyond1lte
    beyond1lteks
    beyond2lte
    beyond2lteks
    beyondx
    beyondxks
    d1
    d1xks
    d2s
    d2x
    d2xks
)

ARTIFACT_ROOT="build/out/all-models"
mkdir -p "$ARTIFACT_ROOT"

for model in "${MODELS[@]}"; do
    echo "==============================================="
    echo "Building model: $model"
    echo "==============================================="

    args=(--model "$model")
    if [[ -n "$KSU_OPTION" ]]; then
        args+=(--ksu "$KSU_OPTION")
    fi
    if [[ -n "$RECOVERY_OPTION" ]]; then
        args+=(--recovery "$RECOVERY_OPTION")
    fi
    if [[ -n "$GPU_MAX" ]]; then
        args+=(--gpu-max "$GPU_MAX")
    fi

    ./build.sh "${args[@]}"

    model_out_dir="build/out/$model/zip"

    latest_zip="$(ls -1t "$model_out_dir"/*.zip 2>/dev/null | head -n 1 || true)"
    if [[ -z "$latest_zip" ]]; then
        echo "No zip artifact found for $model in $model_out_dir"
        exit 1
    fi

    cp -f "$latest_zip" "$ARTIFACT_ROOT/$(basename "$latest_zip")"
    echo "Saved artifact: $ARTIFACT_ROOT/$(basename "$latest_zip")"
done

popd > /dev/null
echo "All model builds finished. Artifacts are in $ARTIFACT_ROOT"
