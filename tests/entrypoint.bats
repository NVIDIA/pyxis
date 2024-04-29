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
