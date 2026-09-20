#!/usr/bin/env bash

set -euo pipefail

readonly rustup_version="1.28.2"

case "$(uname -m)" in
    x86_64)
        rustup_target="x86_64-unknown-linux-gnu"
        rustup_sha256="20a06e644b0d9bd2fbdbfd52d42540bdde820ea7df86e92e533c073da0cdd43c"
        ;;
    aarch64)
        rustup_target="aarch64-unknown-linux-gnu"
        rustup_sha256="e3853c5a252fca15252d07cb23a1bdd9377a8c6f3efa01531109281ae47f841c"
        ;;
    *)
        echo "unsupported rustup host architecture: $(uname -m)" >&2
        exit 2
        ;;
esac

: "${CARGO_HOME:?CARGO_HOME must name the rustup installation directory}"
mkdir -p "${CARGO_HOME}"

rustup_tmp="$(mktemp -d "${TMPDIR:-/tmp}/mako-rustup-init.XXXXXX")"
trap 'rm -rf -- "${rustup_tmp}"' EXIT
rustup_init="${rustup_tmp}/rustup-init"

curl --proto '=https' --tlsv1.2 -fL --retry 3 \
    --output "${rustup_init}" \
    "https://static.rust-lang.org/rustup/archive/${rustup_version}/${rustup_target}/rustup-init"
printf '%s  %s\n' "${rustup_sha256}" "${rustup_init}" \
    | sha256sum --check --strict
chmod 0755 "${rustup_init}"
"${rustup_init}" -y --profile minimal --default-toolchain none --no-modify-path

installed_version="$(cd / && "${CARGO_HOME}/bin/rustup" --version)"
read -r installed_name installed_number _ <<<"${installed_version}"
if [[ "${installed_name}" != "rustup" ||
      "${installed_number}" != "${rustup_version}" ]]; then
    echo "installed unexpected rustup version: ${installed_version}" >&2
    exit 1
fi
