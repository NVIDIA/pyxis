#!/usr/bin/env bats

load ./common

PYXIS_MPI_CONTAINERS=(
    mpi4py_ubuntu2004
    mpi4py_ubuntu2204
    mpi4py_ubuntu2404
    mpi4py_ubuntu2604
    mpi4py_pytorch2602
    mpi4py_pytorch2603
)

MPI_SCRIPT="${BATS_TEST_DIRNAME:-$(pwd)}/mpi_test.py"

function setup() {
    enroot_cleanup "${PYXIS_MPI_CONTAINERS[@]}" || true
}

function teardown() {
    enroot_cleanup "${PYXIS_MPI_CONTAINERS[@]}" || true
}

function install_deps_ubuntu() {
    local name="$1"
    run_srun --container-name="${name}" --container-writable \
             --no-container-mount-home \
             bash -lc '
        set -eux
        export DEBIAN_FRONTEND=noninteractive
        apt-get update
        apt-get install -y --no-install-recommends \
            python3 python3-pip python3-dev python3-numpy \
            libopenmpi-dev openmpi-bin
        pip3 install --break-system-packages mpi4py 2>/dev/null \
            || pip3 install mpi4py
        python3 -c "import mpi4py; print(mpi4py.__version__)"
    ' >/dev/null
}

function install_deps_pytorch() {
    local name="$1"
    run_srun --container-name="${name}" --container-writable \
	     --no-container-mount-home \
	     pip3 install mpi4py >/dev/null
}

function run_mpi_scenarios() {
    local name="$1"
    local mounts="${MPI_SCRIPT}:/opt/mpi_test.py:ro"

    export PMIX_DEBUG=2
    export PMIX_OUTPUT_DEBUG=1

    run_srun --mpi=pmix --ntasks=4 --container-name="${name}" \
             --container-mounts="${mounts}" \
             python3 /opt/mpi_test.py

    SLURM_PMIX_TMPDIR=/dev/shm \
        run_srun --mpi=pmix --ntasks=4 --container-name="${name}" \
                 --container-mounts="${mounts}" \
                 python3 /opt/mpi_test.py

    SLURM_PMIX_TMPDIR=/var/tmp \
        run_srun --mpi=pmix --ntasks=4 --container-name="${name}" \
                 --container-mounts="${mounts}" \
                 python3 /opt/mpi_test.py
}

@test "mpi4py: ubuntu:20.04" {
    name="mpi4py_ubuntu2004"
    run_srun --container-image=ubuntu:20.04 --container-name=${name} true
    install_deps_ubuntu ${name}
    run_mpi_scenarios ${name}
}

@test "mpi4py: ubuntu:22.04" {
    name="mpi4py_ubuntu2204"
    run_srun --container-image=ubuntu:22.04 --container-name=${name} true
    install_deps_ubuntu ${name}
    run_mpi_scenarios ${name}
}

@test "mpi4py: ubuntu:24.04" {
    name="mpi4py_ubuntu2404"
    run_srun --container-image=ubuntu:24.04 --container-name=${name} true
    install_deps_ubuntu ${name}
    run_mpi_scenarios ${name}
}

@test "mpi4py: ubuntu:26.04" {
    name="mpi4py_ubuntu2404"
    run_srun --container-image=ubuntu:26.04 --container-name=${name} true
    install_deps_ubuntu ${name}
    run_mpi_scenarios ${name}
}

@test "mpi4py: nvcr.io/nvidia/pytorch:26.02-py3" {
    name="mpi4py_pytorch2602"
    run_srun --container-image=nvcr.io/nvidia/pytorch:26.02-py3 --container-name=${name} true
    install_deps_pytorch ${name}
    run_mpi_scenarios ${name}
}

@test "mpi4py: nvcr.io/nvidia/pytorch:26.03-py3" {
    name="mpi4py_pytorch2603"
    run_srun --container-image=nvcr.io/nvidia/pytorch:26.03-py3 --container-name=${name} true
    install_deps_pytorch ${name}
    run_mpi_scenarios ${name}
}
