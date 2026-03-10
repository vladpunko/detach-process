#!/usr/bin/env sh

# Created by: Vladislav Punko <iam.vlad.punko@gmail.com>
# Created date: 2021-07-04

set -o errexit

# Exit if this script is run as the superuser.
if [ "$(id -u)" -ne 0 ]; then
  echo 'This program needs to be run as a superuser on the current operating system.' >&2
  # Stop this program runtime and return the exit status code.
  exit 13
fi

workdir="$(mktemp -d)"

_cleanup() {
  rm -f -r -- "${workdir}"
}
trap _cleanup 0 1 2 3 6

# Step -- 1.
git clone --depth=1 --branch=master -- https://github.com/vladpunko/detach-process.git "${workdir}" > /dev/null 2>&1 || {
  echo 'An error occurred while downloading the source code to the current machine.' >&2
  # Stop this program runtime and return the exit status code.
  exit 1
}

# Step -- 2.
gcc -Wall -Wextra -Werror -Wno-unused-parameter "${workdir}/detach.c" -o "${workdir}/detach"

# Step -- 3.
bindir="${BINDIR:-/usr/local/bin}"
install -d -m 755 "${bindir}"
install -m 755 "${workdir}/detach" "${bindir}/detach"

# Step -- 4.
mandir="${MANDIR:-/usr/share/man}"
install -d -m 755 "${mandir}/man1"
install -m 644 "${workdir}/detach.1" "${mandir}/man1/detach.1"
