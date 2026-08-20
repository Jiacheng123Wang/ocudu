# SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
# SPDX-License-Identifier: BSD-3-Clause-Open-MPI

# Shared helpers for docker/scripts/install_*dependencies.sh.

# Return "name=version" if PKG_VERSIONS contains an entry for the given package name,
# otherwise return the bare name. PKG_VERSIONS is a space-separated list of "name=version" pairs.
_pkg_ver() {
    local name="$1"
    local pair
    for pair in $PKG_VERSIONS; do
        case "$pair" in
            "${name}="*) echo "${pair}"; return ;;
        esac
    done
    echo "$name"
}

# Return "name==version" if PKG_VERSIONS contains an entry for the given pip package name,
# otherwise return the bare name. Uses pip's == version pin syntax.
_pip_ver() {
    local name="$1"
    local pair
    for pair in $PKG_VERSIONS; do
        case "$pair" in
            "${name}="*) echo "${name}==${pair#*=}"; return ;;
        esac
    done
    echo "$name"
}

install_ubuntu_pkgs() {
    if ((${#@} == 0)); then
        return 0
    fi

    local -a versioned_pkgs=()
    local pkg
    for pkg in "$@"; do
        versioned_pkgs+=("$(_pkg_ver "$pkg")")
    done

    apt-get update
    apt-get install -y --no-install-recommends "${versioned_pkgs[@]}"
    apt-get autoremove -y && apt-get clean && rm -rf /var/lib/apt/lists/*
}

install_arch_pkgs() {
    if ((${#@} == 0)); then
        return 0
    fi

    local -a versioned_pkgs=()
    local pkg
    for pkg in "$@"; do
        versioned_pkgs+=("$(_pkg_ver "$pkg")")
    done

    pacman -Syu --noconfirm "${versioned_pkgs[@]}"
    pacman -Scc --noconfirm
}

install_rhel_pkgs() {
    if ((${#@} == 0)); then
        return 0
    fi

    local -a versioned_pkgs=()
    local pkg
    for pkg in "$@"; do
        versioned_pkgs+=("$(_pkg_ver "$pkg")")
    done

    dnf -y install "${versioned_pkgs[@]}"
    dnf clean all
}

_enable_centos_repos() {
    if ! rpm -q epel-release >/dev/null 2>&1; then
        dnf install -y dnf-plugins-core epel-release
        dnf config-manager --set-enabled crb >/dev/null 2>&1 || crb enable >/dev/null 2>&1 || true
    fi
}

# Refresh installed RPMs so base-image CVEs are picked up when newer packages
# are available in enabled repos. Callers must source /etc/os-release so ${ID}
# is set.
update_rpm_pkgs() {
    dnf -y update
}

# Install packages via dnf on Fedora and CentOS Stream.
# Optional leading --no-repos skips CentOS repo enablement (base repos only).
install_rpm_pkgs() {
    local enable_repos=1
    if [[ "${1:-}" == "--no-repos" ]]; then
        enable_repos=0
        shift
    fi

    if ((${#@} == 0)); then
        return 0
    fi

    local -a versioned_pkgs=()
    local pkg
    for pkg in "$@"; do
        versioned_pkgs+=("$(_pkg_ver "$pkg")")
    done

    if ((enable_repos)); then
        case "$ID" in
            centos)
                _enable_centos_repos
                ;;
            fedora)
                ;;
            *)
                echo >&2 "OS $ID not supported by install_rpm_pkgs"
                exit 1
                ;;
        esac
    fi

    dnf -y install "${versioned_pkgs[@]}"
    dnf clean all
}

# Install pip packages. Optional leading "--flag" arguments are passed to pip3.
# Without extra flags, falls back to --break-system-packages on failure (Debian/Ubuntu).
install_pip_pkgs() {
    local -a pip_extra_args=()
    local -a pkgs=()

    for arg in "$@"; do
        case "$arg" in
            --*) pip_extra_args+=("$arg") ;;
            *) pkgs+=("$arg") ;;
        esac
    done

    if ((${#pkgs[@]} == 0)); then
        return 0
    fi

    local -a versioned_pip_pkgs=()
    local pkg
    for pkg in "${pkgs[@]}"; do
        versioned_pip_pkgs+=("$(_pip_ver "$pkg")")
    done

    if ((${#pip_extra_args[@]})); then
        pip3 install "${pip_extra_args[@]}" "${versioned_pip_pkgs[@]}"
    else
        pip3 install "${versioned_pip_pkgs[@]}" || pip3 install --break-system-packages "${versioned_pip_pkgs[@]}"
    fi
}
