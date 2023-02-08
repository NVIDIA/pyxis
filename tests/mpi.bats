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

    run_srun --mpi=pmix --ntasks=4 --container-name=mpi-test bash -c 'echo ${PMIX_RANK}'
    [ "$(sort -n <<< ${output})" == "$(seq 0 3)" ]
}

@test "PMIx Horovod" {
    run_srun --mpi=pmix --ntasks=8 --container-image=nvcr.io/nvidia/tensorflow:23.01-tf2-py3 \
             python -c "import os ; import horovod.tensorflow as hvd ; hvd.init(); assert(int(os.getenv('PMIX_RANK')) == hvd.rank())"
}

@test "PMI2 file descriptor" {
    run_srun --mpi=pmi2 --ntasks=4 --container-image=ubuntu:18.04 bash -c '[ -n "${PMI_FD}" ] && realpath /proc/self/fd/${PMI_FD} | grep -q socket'
}

@test "PMI2 environment variables" {
    run_srun --mpi=pmi2 --ntasks=4 --container-image=ubuntu:18.04 bash -c '[ -n "${PMI_RANK}" ]'
    run_srun --mpi=pmi2 --ntasks=4 --container-image=ubuntu:18.04 bash -c '[ -n "${PMI_SIZE}" ] && [ "${PMI_SIZE}" -eq 4 ]'
}
