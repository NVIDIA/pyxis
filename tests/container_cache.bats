#!/usr/bin/env bats

load ./common

function _test_image() {
    local img="${PYXIS_TEST_SQSH_IMAGE:-/home/horde/vsabavat-code/enroot/ubuntu+latest.sqsh}"
    if [ ! -f "${img}" ]; then
        skip "set PYXIS_TEST_SQSH_IMAGE to a local .sqsh image path"
    fi
    echo "${img}"
}

function _cache_root() {
    if [ -n "${PYXIS_TEST_CACHE_ROOT:-}" ]; then
        echo "${PYXIS_TEST_CACHE_ROOT}"
        return 0
    fi
    if [ -r /opt/slurm-test/etc/plugstack.conf ]; then
        sed -n 's/.*container_cache_data_path=\([^ ]*\).*/\1/p' /opt/slurm-test/etc/plugstack.conf | head -n1
        return 0
    fi
    echo "/tmp/enroot-data"
}

function _gc_high() {
    if [ -n "${PYXIS_TEST_GC_HIGH:-}" ]; then
        echo "${PYXIS_TEST_GC_HIGH}"
        return 0
    fi
    if [ -r /opt/slurm-test/etc/plugstack.conf ]; then
        local v
        v="$(sed -n 's/.*container_cache_gc_high=\([^ ]*\).*/\1/p' /opt/slurm-test/etc/plugstack.conf | head -n1)"
        if [ -n "${v}" ]; then
            echo "${v}"
            return 0
        fi
    fi
    echo "85"
}

function _gc_low() {
    if [ -n "${PYXIS_TEST_GC_LOW:-}" ]; then
        echo "${PYXIS_TEST_GC_LOW}"
        return 0
    fi
    if [ -r /opt/slurm-test/etc/plugstack.conf ]; then
        local v
        v="$(sed -n 's/.*container_cache_gc_low=\([^ ]*\).*/\1/p' /opt/slurm-test/etc/plugstack.conf | head -n1)"
        if [ -n "${v}" ]; then
            echo "${v}"
            return 0
        fi
    fi
    echo "80"
}

function _mkimglink() {
    local target="$1"
    local link
    link="$(mktemp -p /tmp pyxis-cache-img.XXXXXX.sqsh)"
    rm -f "${link}"
    ln -s "${target}" "${link}"
    echo "${link}"
}

function _cache_container_name_for_image() {
    local image="$1"
    local uid
    uid="$(id -u)"
    python3 - "${image}" "${uid}" <<'PY'
import os, sys

image = sys.argv[1]
uid = int(sys.argv[2])

FNV_OFFSET = 1469598103934665603
FNV_PRIME = 1099511628211
MASK = (1 << 64) - 1

def fnv1a64_update(h: int, data: bytes) -> int:
    for b in data:
        h ^= b
        h = (h * FNV_PRIME) & MASK
    return h

h = FNV_OFFSET
h = fnv1a64_update(h, image.encode())

if image and image[0] in ('.', '/'):
    try:
        st = os.stat(image)
        extra = f"|{int(st.st_mtime)}|{int(st.st_size)}".encode()
        h = fnv1a64_update(h, extra)
    except FileNotFoundError:
        pass

print(f"pyxis_cache_u{uid}_{h:016x}")
PY
}

function _cache_container_dir_for_image() {
    local image="$1"
    local uid
    uid="$(id -u)"
    local root
    root="$(_cache_root)"
    echo "${root}/${uid}/$(_cache_container_name_for_image "${image}")"
}

function _cleanup_cache_for_image() {
    local image="$1"
    local dir
    dir="$(_cache_container_dir_for_image "${image}")"
    rm -rf "${dir}" || true
}

function setup() {
    unset PYXIS_DEBUG || true
}

function teardown() {
    unset PYXIS_DEBUG || true
}

@test "--container-cache is incompatible with --container-writable" {
    local img link
    img="$(_test_image)"
    link="$(_mkimglink "${img}")"

    run_srun_unchecked --container-cache --container-image="${link}" --container-writable true
    [ "${status}" -ne 0 ]
    [[ "${output}" == *"--container-cache is incompatible with --container-writable"* ]]

    _cleanup_cache_for_image "${link}"
    rm -f "${link}" || true
}

@test "--container-cache is incompatible with --container-save" {
    local img link
    img="$(_test_image)"
    link="$(_mkimglink "${img}")"

    run_srun_unchecked --container-cache --container-image="${link}" --container-save=/tmp/pyxis-cache-test.sqsh true
    [ "${status}" -ne 0 ]
    [[ "${output}" == *"--container-cache is incompatible with --container-save"* ]]

    _cleanup_cache_for_image "${link}"
    rm -f "${link}" || true
}

@test "--container-cache requires --container-image" {
    run_srun_unchecked --container-cache true
    [ "${status}" -ne 0 ]
    [[ "${output}" == *"--container-cache requires --container-image"* ]]
}

@test "--container-cache is incompatible with --container-name flags" {
    local img link
    img="$(_test_image)"
    link="$(_mkimglink "${img}")"

    run_srun_unchecked --container-cache --container-image="${link}" --container-name=cache-test:exec true
    [ "${status}" -ne 0 ]
    [[ "${output}" == *"--container-cache is incompatible with --container-name flags"* ]]

    _cleanup_cache_for_image "${link}"
    rm -f "${link}" || true
}

@test "--container-cache creates a stable pyxis_cache_u<uid>_* rootfs under container_cache_data_path" {
    local img link uid root name dir
    img="$(_test_image)"
    link="$(_mkimglink "${img}")"

    uid="$(id -u)"
    root="$(_cache_root)"
    name="$(_cache_container_name_for_image "${link}")"
    dir="$(_cache_container_dir_for_image "${link}")"

    rm -rf "${dir}" || true

    run_srun --container-cache --container-image="${link}" true

    [ -d "${dir}" ]
    [ -f "${dir}/.pyxis_cache_lock" ]
    [ "$(stat -c %a "${root}/${uid}")" = "700" ]
    [ "$(stat -c %u "${root}/${uid}")" = "${uid}" ]
    [[ "${name}" == "pyxis_cache_u${uid}_"* ]]
    [[ "${dir}" == "${root}/${uid}/"* ]]

    _cleanup_cache_for_image "${link}"
    rm -f "${link}" || true
}

@test "--container-cache ignores plain --container-name (stable name is still used)" {
    local img link uid root stable_dir ignored_name ignored_dir
    img="$(_test_image)"
    link="$(_mkimglink "${img}")"

    uid="$(id -u)"
    root="$(_cache_root)"

    ignored_name="cache-name-ignored-${RANDOM}"
    ignored_dir="${root}/${uid}/pyxis_${ignored_name}"
    stable_dir="$(_cache_container_dir_for_image "${link}")"

    rm -rf "${ignored_dir}" "${stable_dir}" || true

    run_srun --container-cache --container-image="${link}" --container-name="${ignored_name}" true

    [ -d "${stable_dir}" ]
    [ ! -e "${ignored_dir}" ]

    _cleanup_cache_for_image "${link}"
    rm -f "${link}" || true
}

@test "--container-cache forces a read-only rootfs (even with --container-remap-root)" {
    local img link
    img="$(_test_image)"
    link="$(_mkimglink "${img}")"

    run_srun_unchecked --container-cache --container-image="${link}" --container-remap-root sh -c 'touch /pyxis_ro_test'
    [ "${status}" -ne 0 ]
    [[ "${output}" == *"Read-only file system"* ]]

    _cleanup_cache_for_image "${link}"
    rm -f "${link}" || true
}

@test "container-cache GC does not evict cache entries that are locked/in-use" {
    if ! command -v flock >/dev/null 2>&1; then
        skip "flock not available"
    fi

    local img link uid root cache_dir locked_dir unlocked_dir lock_file lock_pid
    img="$(_test_image)"
    link="$(_mkimglink "${img}")"

    uid="$(id -u)"
    root="$(_cache_root)"
    cache_dir="${root}/${uid}"

    mkdir -p "${cache_dir}"

    local high used_pct
    high="$(_gc_high)"
    used_pct="$(python3 - "${cache_dir}" <<'PY'
import os
import sys

path = sys.argv[1]
st = os.statvfs(path)
total = st.f_blocks * st.f_frsize
avail = st.f_bavail * st.f_frsize
used = total - avail
pct = int((used * 100) // total) if total else 0
print(pct)
PY
)"
    if [ "${used_pct}" -lt "${high}" ]; then
        rm -f "${link}" || true
        skip "cache fs usage ${used_pct}% < ${high}% (GC won't run)"
    fi

    locked_dir="${cache_dir}/pyxis_cache_u${uid}_lockedtest_${RANDOM}"
    unlocked_dir="${cache_dir}/pyxis_cache_u${uid}_unlockedtest_${RANDOM}"

    mkdir -p "${locked_dir}" "${unlocked_dir}"
    lock_file="${locked_dir}/.pyxis_cache_lock"
    : > "${lock_file}"

    touch -d "1970-01-01 00:00:00" "${locked_dir}" 2>/dev/null || true
    touch -d "1970-01-01 00:00:01" "${unlocked_dir}" 2>/dev/null || true

    flock -s "${lock_file}" -c 'sleep 300' &
    lock_pid=$!

    run_srun --container-cache --container-image="${link}" true

    kill "${lock_pid}" >/dev/null 2>&1 || true
    wait "${lock_pid}" >/dev/null 2>&1 || true

    [ -d "${locked_dir}" ]
    [ ! -e "${unlocked_dir}" ]

    rm -rf "${locked_dir}" || true
    _cleanup_cache_for_image "${link}"
    rm -f "${link}" || true
}

@test "container-cache GC can evict entries in other user dirs when space is tight" {
    local img link uid root high used_pct other_uid other_dir victim
    img="$(_test_image)"
    link="$(_mkimglink "${img}")"

    uid="$(id -u)"
    root="$(_cache_root)"
    high="$(_gc_high)"

    used_pct="$(python3 - "${root}" <<'PY'
import os
import sys

path = sys.argv[1]
st = os.statvfs(path)
total = st.f_blocks * st.f_frsize
avail = st.f_bavail * st.f_frsize
used = total - avail
pct = int((used * 100) // total) if total else 0
print(pct)
PY
)"
    if [ "${used_pct}" -lt "${high}" ]; then
        _cleanup_cache_for_image "${link}"
        rm -f "${link}" || true
        skip "cache fs usage ${used_pct}% < ${high}% (GC won't run)"
    fi

    other_uid="9999${RANDOM}"
    other_dir="${root}/${other_uid}"
    victim="${other_dir}/pyxis_cache_u${other_uid}_victim_${RANDOM}"

    cleanup() {
        chmod 700 "${other_dir}" 2>/dev/null || true
        rm -rf "${other_dir}" 2>/dev/null || true
        _cleanup_cache_for_image "${link}" 2>/dev/null || true
        rm -f "${link}" 2>/dev/null || true
    }
    trap cleanup EXIT

    if ! mkdir -p "${victim}"; then
        skip "cannot create ${victim} (cache root not writable?)"
    fi
    : > "${victim}/.pyxis_cache_lock"
    touch -d "1970-01-01 00:00:00" "${victim}" 2>/dev/null || true
    chmod 000 "${other_dir}" 2>/dev/null || true

    run_srun --container-cache --container-image="${link}" true

    chmod 700 "${other_dir}" 2>/dev/null || true
    [ ! -e "${victim}" ]
}

@test "container-cache uses a stable rootfs directory name across runs" {
    local img link container_dir
    img="$(_test_image)"
    link="$(_mkimglink "${img}")"

    container_dir="$(_cache_container_dir_for_image "${link}")"
    rm -rf "${container_dir}" || true

    run_srun --container-cache --container-image="${link}" true
    [ -d "${container_dir}" ]

    run_srun --container-cache --container-image="${link}" true
    [ -d "${container_dir}" ]

    _cleanup_cache_for_image "${link}"
    rm -f "${link}" || true
}

@test "PYXIS_CONTAINER_CACHE=1 enables cache mode (no --container-cache flag needed)" {
    local img link uid root name dir
    img="$(_test_image)"
    link="$(_mkimglink "${img}")"

    uid="$(id -u)"
    root="$(_cache_root)"
    name="$(_cache_container_name_for_image "${link}")"
    dir="$(_cache_container_dir_for_image "${link}")"

    rm -rf "${dir}" || true

    export PYXIS_CONTAINER_CACHE=1
    run_srun --container-image="${link}" true
    unset PYXIS_CONTAINER_CACHE

    [ -d "${dir}" ]
    [[ "${name}" == "pyxis_cache_u${uid}_"* ]]

    _cleanup_cache_for_image "${link}"
    rm -f "${link}" || true
}


