#!/usr/bin/env sh

# Created by: Vladislav Punko <iam.vlad.punko@gmail.com>
# Created date: 2021-07-04

set -e

# Installation writes system directories and requires the superuser.
if [ "$(id -u)" -ne 0 ]; then
  echo 'This program needs to be run as a superuser on the current operating system.' >&2
  exit 13
fi

build_directory="$(mktemp -d)"

# Removes the downloaded source and build output after success or interruption.
cleanup() {
  rm -f -r -- "${build_directory}"
}
trap cleanup EXIT HUP INT QUIT ABRT

git clone --depth=1 --branch=master -- \
  https://github.com/vladpunko/detach-process.git "${build_directory}" > /dev/null 2>&1 || {
  echo 'An error occurred while downloading the source code to the current machine.' >&2
  exit 1
}

gcc -std=c99 -Wall -Wextra -Werror -Wno-unused-parameter \
  "${build_directory}/detach.c" -o "${build_directory}/detach"

umask 022
mkdir -p /usr/local/bin /usr/local/share/man/man1
install -m 755 "${build_directory}/detach" /usr/local/bin/detach
install -m 644 "${build_directory}/detach.1" /usr/local/share/man/man1/detach.1
