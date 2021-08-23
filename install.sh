#!/usr/bin/env sh

# Copyright 2020 (c) Vladislav Punko <iam.vlad.punko@gmail.com>

set -o errexit

# Exit if this script is run as the superuser.
if [ "$(id -u)" -ne 0 ]; then
  echo 'This program needs to be run as a superuser on the current operating system.' >&2
  # Stop this program runtime and return the exit status code.
  exit 13
fi

workdir="$(mktemp -d)"

# Step -- 1.
git clone --depth=1 --branch=master -- https://github.com/vladpunko/detach-process.git "${workdir}" > /dev/null 2>&1 || {
  echo 'An error occurred while downloading the source code to the current machine.' >&2
  # Stop this program runtime and return the exit status code.
  exit 1
}

# Step -- 2.
gcc -Wall -Wextra -Werror -Wno-unused-parameter "${workdir}/detach.c" -o "${workdir}/detach"

# Step -- 3.
install -m 755 "${workdir}/detach" /usr/local/bin/detach

_cleanup() {
  rm -f -r -- "${workdir}"
}
trap _cleanup 0 1 2 3 6
