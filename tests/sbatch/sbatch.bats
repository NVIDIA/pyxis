#!/usr/bin/env bats

load ../common

@test "sbatch ubuntu:16.04" {
    run_sbatch --container-image=ubuntu:16.04 <<EOF
#!/bin/bash
grep 'Ubuntu 16.04' /etc/os-release
EOF

    run_sbatch <<EOF
#!/bin/bash
#SBATCH --container-image=ubuntu:16.04
grep 'Ubuntu 16.04' /etc/os-release
EOF
}

@test "sbatch centos:8" {
    run_sbatch --container-image=centos:8 <<EOF
#!/bin/bash
grep 'CentOS Linux release 8.' /etc/redhat-release
EOF

    run_sbatch <<EOF
#!/bin/bash
#SBATCH --container-image=centos:8
grep 'CentOS Linux release 8.' /etc/redhat-release
EOF
}
