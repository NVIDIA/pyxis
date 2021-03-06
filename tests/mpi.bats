#!/usr/bin/env bats

load ./common

function setup() {
    enroot_cleanup mpi-test || true
}

function teardown() {
    enroot_cleanup mpi-test || true
}

@test "PMIx mount" {
    run_srun --mpi=pmix --ntasks=4 --container-image=ubuntu:18.04 sh -c '[ -n "${PMIX_SERVER_TMPDIR}" ] && findmnt "${PMIX_SERVER_TMPDIR:-error}"'
}

@test "PMIx environment variables" {
    run_srun --container-name=mpi-test --container-image=ubuntu:20.04 true
    run_srun --mpi=pmix --ntasks=4 --container-name=mpi-test bash -c '[ -n "${PMIX_RANK}" ]'
    run_srun --mpi=pmix --ntasks=4 --container-name=mpi-test bash -c '[ -n "${PMIX_MCA_psec}" ] && [ "${PMIX_MCA_psec}" == "${PMIX_SECURITY_MODE}" ]'
    run_srun --mpi=pmix --ntasks=4 --container-name=mpi-test bash -c '[ -n "${PMIX_MCA_gds}" ] && [ "${PMIX_MCA_gds}" == "${PMIX_GDS_MODULE}" ]'

    # usock was removed in 4.0.0: https://github.com/openpmix/openpmix/releases/tag/v4.0.0
    # In this version PMIX_PTL_MODULE is not set, since only TCP is available.
    if srun --mpi=pmix --ntasks=1 bash -u -c 'echo ${PMIX_PTL_MODULE}' >/dev/null 2>&1; then
        run_srun --mpi=pmix --ntasks=4 --container-name=mpi-test bash -c '[ -n "${PMIX_MCA_ptl}" ] && [ "${PMIX_MCA_ptl}" == "${PMIX_PTL_MODULE}" ]'
    fi

    run_srun --mpi=pmix --ntasks=4 --container-name=mpi-test bash -c 'echo ${PMIX_RANK}'
    [ "$(sort -n <<< ${output})" == "$(seq 0 3)" ]
}

@test "PMI2 file descriptor" {
    run_srun --mpi=pmi2 --ntasks=4 --container-image=ubuntu:18.04 bash -c '[ -n "${PMI_FD}" ] && realpath /proc/self/fd/${PMI_FD} | grep -q socket'
}

@test "PMI2 environment variables" {
    run_srun --mpi=pmi2 --ntasks=4 --container-image=ubuntu:18.04 bash -c '[ -n "${PMI_RANK}" ]'
    run_srun --mpi=pmi2 --ntasks=4 --container-image=ubuntu:18.04 bash -c '[ -n "${PMI_SIZE}" ] && [ "${PMI_SIZE}" -eq 4 ]'
}
