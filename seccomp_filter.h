#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/seccomp.h>

#include <sys/syscall.h>

#include "common.h"

/*
 * This seccomp filter is necessary to allow users to install packages inside the
 * container. Another common option is to assign each user a range of subordinate uids
 * and gids, but this can prove challenging to setup in a cluster environment with a
 * large number of users:
 * - it requires careful setup to avoid overlap between users on different machines
 * - it requires using setuid binaries newuidmap(1) and newgidmap(1)
 * - writing to a shared filesystem with a subordinate uid/gid can create surprising
 *   results for the user.
 * This code was taken from enroot: https://github.com/NVIDIA/enroot/blob/c0043113e23eccc5128b44427914d14448d61f0a/bin/enroot-unshare.c#L64-L95
 */
static struct sock_filter filter[] = {
	/* Check for the syscall architecture (x86_64 and x32 ABIs). */
	BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, arch)),
	BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, AUDIT_ARCH_X86_64, 1, 0),
	BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL),

	/* Load the syscall number. */
	BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, nr)),

	/* Return success on all the following syscalls. */
	BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_setuid, 15, 0),
	BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_setgid, 14, 0),
	BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_setreuid, 13, 0),
	BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_setregid, 12, 0),
	BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_setresuid, 11, 0),
	BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_setresgid, 10, 0),
	BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_setgroups, 9, 0),
	BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_fchownat, 8, 0),
	BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_chown, 7, 0),
	BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_fchown, 6, 0),
	BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_lchown, 5, 0),

	/* For setfsuid/setfsgid we only return success if the uid/gid argument is not -1. */
	BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_setfsuid, 1, 0),
	BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_setfsgid, 0, 2),
	BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, args[0])),
	BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, (uint32_t)-1, 0, 1),

	/* Execute the syscall as usual otherwise. */
	BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
	BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | (SECCOMP_RET_DATA & 0x0)),
};

static inline int seccomp_set_filter(void)
{
	return syscall(SYS_seccomp, SECCOMP_SET_MODE_FILTER, SECCOMP_FILTER_FLAG_SPEC_ALLOW,
		       &(const struct sock_fprog){ ARRAY_SIZE(filter), filter });
}
