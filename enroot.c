/*
 * Copyright (c) 2020, NVIDIA CORPORATION. All rights reserved.
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

	ret = waitpid(pid, &status, 0);
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

void enroot_print_log(int log_fd)
{
	int ret;
	FILE *fp;
	char *line;

	ret = lseek(log_fd, 0, SEEK_SET);
	if (ret < 0) {
		slurm_info("pyxis: couldn't rewind log file: %s", strerror(errno));
		return;
	}

	fp = fdopen(log_fd, "r");
	if (fp == NULL) {
		slurm_info("pyxis: couldn't open in-memory log for printing: %s", strerror(errno));
		return;
	}

	slurm_error("pyxis: printing enroot log file:");
	while ((line = get_line_from_file(fp)) != NULL) {
		slurm_error("pyxis:     %s", line);
		free(line);
	}

	return;
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
		enroot_print_log(log_fd);
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
