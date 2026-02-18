/*
 * Copyright (c) 2020-2026, NVIDIA CORPORATION. All rights reserved.
 */

#include <sys/wait.h>

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <slurm/spank.h>

#include "enroot.h"
#include "common.h"

pid_t enroot_exec(uid_t uid, gid_t gid, int log_fd,
		  child_cb callback, char *const argv[])
{
	int ret;
	int null_fd = -1;
	int target_fd = -1;
	int oom_score_fd = -1;
	pid_t pid;
	char *argv_str;

	argv_str = join_strings(argv, " ");
	if (argv_str != NULL) {
		slurm_verbose("pyxis: running enroot command: \"%s\"", argv_str);
		free(argv_str);
	}

	pid = fork();
	if (pid < 0) {
		slurm_error("pyxis: fork error: %s", strerror(errno));
		return (-1);
	}

	if (pid == 0) {
		/*
		 * Move log_fd out of the standard fd range (0-2) if needed.
		 * In some contexts (e.g. SPANK epilog), fd 0 is not open,
		 * so memfd_create can return fd 0. Without this, the dup2
		 * to STDIN_FILENO below would clobber log_fd.
		 */
		if (log_fd >= 0 && log_fd <= 2) {
			int new_fd = fcntl(log_fd, F_DUPFD_CLOEXEC, 3);
			if (new_fd < 0)
				_exit(EXIT_FAILURE);
			close(log_fd);
			log_fd = new_fd;
		}

		null_fd = open("/dev/null", O_RDWR);
		if (null_fd < 0)
			_exit(EXIT_FAILURE);

		ret = dup2(null_fd, STDIN_FILENO);
		if (ret < 0)
			_exit(EXIT_FAILURE);

		/* Redirect stdout/stderr to the log file or /dev/null */
		if (log_fd >= 0)
			target_fd = log_fd;
		else
			target_fd = null_fd;

		ret = dup2(target_fd, STDOUT_FILENO);
		if (ret < 0)
			_exit(EXIT_FAILURE);

		ret = dup2(target_fd, STDERR_FILENO);
		if (ret < 0)
			_exit(EXIT_FAILURE);

		/*
		 * Attempt to set oom_score_adj to 0, as it's often set to -1000 (OOM killing
		 * disabled), inherited from slurmstepd or slurmd.
		 */
		oom_score_fd = open("/proc/self/oom_score_adj", O_CLOEXEC | O_WRONLY | O_APPEND);
		if (oom_score_fd >= 0) {
			dprintf(oom_score_fd, "%d", 0);
			close(oom_score_fd);
		}

		ret = setregid(gid, gid);
		if (ret < 0)
			_exit(EXIT_FAILURE);

		ret = setreuid(uid, uid);
		if (ret < 0)
			_exit(EXIT_FAILURE);

		if (callback != NULL) {
			ret = callback();
			if (ret < 0)
				_exit(EXIT_FAILURE);
		}

		execvpe("enroot", argv, environ);

		_exit(EXIT_FAILURE);
	}

	return (pid);
}

static int child_wait(pid_t pid)
{
	int status;
	int ret;

	do {
		ret = waitpid(pid, &status, 0);
	} while (ret < 0 && errno == EINTR);

	if (ret < 0) {
		slurm_error("pyxis: could not wait for child %d: %s", pid, strerror(errno));
		return (-1);
	}

	if (WIFSIGNALED(status)) {
		slurm_error("pyxis: child %d terminated with signal %d", pid, WTERMSIG(status));
		return (-1);
	}

	if (WIFEXITED(status) && (status = WEXITSTATUS(status)) != 0) {
		slurm_error("pyxis: child %d failed with error code: %d", pid, status);
		return (-1);
	}

	return (0);
}

int enroot_exec_wait(uid_t uid, gid_t gid, int log_fd,
		     child_cb callback, char *const argv[])
{
	int ret;
	pid_t child;

	child = enroot_exec(uid, gid, log_fd, callback, argv);
	if (child < 0)
		return (-1);

	ret = child_wait(child);
	if (ret < 0)
		return (-1);

	return (0);
}

FILE *enroot_exec_output(uid_t uid, gid_t gid,
			 child_cb callback, char *const argv[])
{
	int ret;
	int log_fd = -1;
	FILE *fp = NULL;

	log_fd = pyxis_memfd_create("enroot-log", MFD_CLOEXEC);
	if (log_fd < 0) {
		slurm_error("pyxis: couldn't create in-memory log file: %s", strerror(errno));
		return (NULL);
	}

	ret = enroot_exec_wait(uid, gid, log_fd, callback, argv);
	if (ret < 0) {
		slurm_error("pyxis: couldn't execute enroot command");
		memfd_print_log(&log_fd, true, "enroot");
		goto fail;
	}

	ret = lseek(log_fd, 0, SEEK_SET);
	if (ret < 0) {
		slurm_error("pyxis: couldn't rewind log file: %s", strerror(errno));
		goto fail;
	}

	fp = fdopen(log_fd, "r");
	if (fp == NULL) {
		slurm_error("pyxis: couldn't open in-memory log file: %s", strerror(errno));
		goto fail;
	}
	log_fd = -1;

fail:
	xclose(log_fd);

	return (fp);
}
