#!/bin/bash

set -euo pipefail

readonly cmd="$1"

readonly user_dir="${PYXIS_RUNTIME_PATH}/${SLURM_JOB_UID}"
readonly squashfs_path="${user_dir}/${SLURM_JOB_ID}.${SLURM_STEP_ID}.squashfs"

case "${cmd}" in
    get)
	if [ $# -ne 2 ]; then
	    echo "usage: $0 get URI" >&2
	    exit 1
	fi

        readonly image_uri="$2"

        mkdir -p -m 700 "${user_dir}"

        enroot import --output "${squashfs_path}" "${image_uri}" >&2

        # Output the squashfs path on stdout for pyxis to read
        echo "${squashfs_path}"
        ;;
    release)
	if [ $# -ne 1 ]; then
	    echo "usage: $0 release" >&2
	    exit 1
	fi

	# Remove the squashfs file if it exists (idempotent: safe to call multiple times)
	rm -f "${squashfs_path}"
        ;;
    *)
        echo "error: unknown command: ${cmd}" >&2
        exit 1
        ;;
esac
