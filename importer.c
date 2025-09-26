/*
 * Copyright (c) 2025, NVIDIA CORPORATION. All rights reserved.
 */

#include <sys/types.h>
#include <sys/wait.h>

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <slurm/spank.h>

#include "importer.h"
#include "common.h"

static int importer_child_wait(pid_t pid, int *log_fd, const char *cmd)
{
	int status;
	int ret;

	do {
		ret = waitpid(pid, &status, 0);
	} while (ret < 0 && errno == EINTR);

	if (ret < 0) {
		slurm_error("pyxis: could not wait for importer %s: %s", cmd, strerror(errno));
		return (-1);
	}

	if (WIFSIGNALED(status)) {
		slurm_error("pyxis: importer %s terminated with signal %d", cmd, WTERMSIG(status));
		memfd_print_log(log_fd, true, "importer");
		return (-1);
	}

	if (WIFEXITED(status) && WEXITSTATUS(status) != 0) {
		slurm_error("pyxis: importer %s failed with exit code %d", cmd, WEXITSTATUS(status));
		memfd_print_log(log_fd, true, "importer");
		return (-1);
	}

	return (0);
}

static pid_t importer_exec(const char *importer_path, uid_t uid, gid_t gid,
			   int stdout_fd, int stderr_fd, child_cb callback, char *const argv[])
{
	int ret;
	int null_fd = -1;
	int oom_score_fd = -1;
	pid_t pid;
	char *argv_str;

	argv_str = join_strings(argv, " ");
	if (argv_str != NULL) {
		slurm_verbose("pyxis: running importer command: %s", argv_str);
		free(argv_str);
	}

	pid = fork();
	if (pid < 0) {
		slurm_error("pyxis: fork error: %s", strerror(errno));
		return (-1);
	}

	if (pid == 0) {
		null_fd = open("/dev/null", O_RDONLY);
		if (null_fd < 0)
			_exit(EXIT_FAILURE);

		ret = dup2(null_fd, STDIN_FILENO);
		if (ret < 0)
			_exit(EXIT_FAILURE);

		ret = dup2(stdout_fd, STDOUT_FILENO);
		if (ret < 0)
			_exit(EXIT_FAILURE);

		ret = dup2(stderr_fd, STDERR_FILENO);
		if (ret < 0)
			_exit(EXIT_FAILURE);

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

		execve(importer_path, argv, environ);
		_exit(EXIT_FAILURE);
	}

	return (pid);
}


int importer_exec_get(const char *importer_path, uid_t uid, gid_t gid,
		      child_cb callback, const char *image_uri, char **squashfs_path)
{
	char *argv[4];
	int log_fd = -1;
	int pipe_fds[2] = {-1, -1};
	pid_t child;
	FILE *pipe_file = NULL;
	char *line = NULL;

	log_fd = pyxis_memfd_create("importer-log", MFD_CLOEXEC);
	if (log_fd < 0) {
		slurm_error("pyxis: couldn't create in-memory log file: %s", strerror(errno));
		return (-1);
	}

	/* Create pipe for reading stdout */
	if (pipe(pipe_fds) < 0) {
		slurm_error("pyxis: could not create pipe: %s", strerror(errno));
		xclose(log_fd);
		return (-1);
	}

	argv[0] = (char *)importer_path;
	argv[1] = "get";
	argv[2] = (char *)image_uri;
	argv[3] = NULL;

	child = importer_exec(importer_path, uid, gid, pipe_fds[1], log_fd, callback, argv);
	xclose(pipe_fds[1]);  /* Close write end in parent */

	if (child < 0) {
		xclose(pipe_fds[0]);
		xclose(log_fd);
		return (-1);
	}

	/* Convert pipe fd to FILE* for get_line_from_file */
	pipe_file = fdopen(pipe_fds[0], "r");
	if (pipe_file == NULL) {
		slurm_error("pyxis: could not fdopen pipe: %s", strerror(errno));
		xclose(pipe_fds[0]);
		xclose(log_fd);
		return (-1);
	}

	/* Read the squashfs path from stdout */
	line = get_line_from_file(pipe_file);
	fclose(pipe_file);  /* This also closes pipe_fds[0] */

	/* Wait for child to complete */
	if (importer_child_wait(child, &log_fd, "get") < 0) {
		free(line);
		xclose(log_fd);
		return (-1);
	}

	/* Validate we got a path */
	if (line == NULL || strlen(line) == 0) {
		slurm_error("pyxis: importer did not return a squashfs path");
		memfd_print_log(&log_fd, true, "importer");
		free(line);
		xclose(log_fd);
		return (-1);
	}

	/* Return the path */
	*squashfs_path = line;

	slurm_verbose("pyxis: importer squashfs path: %s", *squashfs_path);

	xclose(log_fd);

	return (0);
}

int importer_exec_release(const char *importer_path, uid_t uid, gid_t gid,
			  child_cb callback)
{
	int ret;
	int log_fd;
	char *argv[3];
	pid_t child;

	log_fd = pyxis_memfd_create("importer-log", MFD_CLOEXEC);
	if (log_fd < 0) {
		slurm_error("pyxis: couldn't create in-memory log file: %s", strerror(errno));
		return (-1);
	}

	argv[0] = (char *)importer_path;
	argv[1] = "release";
	argv[2] = NULL;

	child = importer_exec(importer_path, uid, gid, log_fd, log_fd, callback, argv);
	if (child < 0) {
		xclose(log_fd);
		return (-1);
	}

	ret = importer_child_wait(child, &log_fd, "release");
	if (ret < 0) {
		xclose(log_fd);
		return (-1);
	}

	xclose(log_fd);

	return (0);
}
