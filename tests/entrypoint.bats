#!/usr/bin/env bats

load ./common

function setup() {
    enroot_cleanup nginx-test || true
}

function teardown() {
    enroot_cleanup nginx-test || true
}

@test "--container-entrypoint: docker:dind" {
    if srun bash -c '[ -f /etc/enroot/entrypoint ]'; then
	skip "entrypoint disabled by enroot"
    fi

    run_srun --container-image=docker:26.1.0-dind-rootless --container-entrypoint sh -c '[ -n "${DOCKER_HOST}" ]'
    run_srun --container-image=docker:26.1.0-dind-rootless --no-container-entrypoint sh -c '[ -z "${DOCKER_HOST}" ]'
}

@test "manual execution of entrypoint" {
    # See https://github.com/nginxinc/docker-nginx/blob/1.19.0/mainline/buster/10-listen-on-ipv6-by-default.sh
    run_srun --container-image nginx:1.19.1 --container-name nginx-test --container-remap-root nginx -v
    run_srun --container-name nginx-test cat /etc/nginx/conf.d/default.conf
    grep -q "listen       80;" <<< "${output}"

    run_srun --container-name nginx-test --container-remap-root /docker-entrypoint.sh nginx -v
    run_srun --container-name nginx-test cat /etc/nginx/conf.d/default.conf
    grep -q "listen  \[::]\:80;" <<< "${output}"
}

@test "--container-entrypoint and --container-env" {
    if srun bash -c '[ -f /etc/enroot/entrypoint ]'; then
        skip "entrypoint disabled by enroot"
    fi

    # https://github.com/docker-library/docker/blob/75c73110b6ee739dab28c30b757eec51484968c1/26/cli/docker-entrypoint.sh#L30-L36
    run_srun --container-image=docker:26.1.0-dind-rootless --container-entrypoint sh -c 'echo ${DOCKER_HOST}'
    [ "${lines[-1]}" == "tcp://docker:2375" ]

    export DOCKER_TLS_VERIFY=1
    run_srun --container-image=docker:26.1.0-dind-rootless --container-entrypoint sh -c 'echo ${DOCKER_HOST}'
    # The variable is only passed to the entrypoint when using --container-env.
    [ "${lines[-1]}" == "tcp://docker:2375" ]

    run_srun --container-image=docker:26.1.0-dind-rootless --container-entrypoint --container-env=DOCKER_TLS_VERIFY sh -c 'echo ${DOCKER_HOST}'
    [ "${lines[-1]}" == "tcp://docker:2376" ]
}

@test "--container-entrypoint-log: nvidia/cuda" {
    if srun bash -c '[ -f /etc/enroot/entrypoint ]'; then
	skip "entrypoint disabled by enroot"
    fi

    run_srun --container-image=nvidia/cuda:12.5.1-runtime-ubuntu24.04 --container-entrypoint true
    ! grep -q "== CUDA ==" <<< "${output}"

    run_srun --container-image=nvidia/cuda:12.5.1-runtime-ubuntu24.04 --container-entrypoint --container-entrypoint-log true
    grep -q "== CUDA ==" <<< "${output}"
}
