#!/bin/bash

set -e

if [[ $# -lt 3 ]]; then
  echo Usage: $0 [options] repo-url tag-or-branch subdir [commit]
  echo  Options:
  echo    -Dkey=val
  exit 1
fi

MESON_OPTIONS=()

while [[ $1 =~ ^-D ]]; do
  MESON_OPTIONS+=( "$1" )
  shift
done

REPO_URL="$1"
TAG_OR_BRANCH="$2"
SUBDIR="$3"
COMMIT="$4"

REPO_DIR="$(basename ${REPO_URL%.git})"

git clone --depth 1 "$REPO_URL" -b "$TAG_OR_BRANCH"
pushd "$REPO_DIR" || exit 1
pushd "$SUBDIR" || exit 1

if [[ ! -z "$COMMIT" ]]; then
  git fetch origin "$COMMIT"
  git checkout "$COMMIT"
fi

meson setup --prefix=/usr _build "${MESON_OPTIONS[@]}"
meson install -C _build
popd || exit 1
popd || exit 1
rm -rf "$REPO_DIR"
