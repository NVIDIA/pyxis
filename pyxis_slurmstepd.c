/*
 * Copyright (c) 2019, NVIDIA CORPORATION. All rights reserved.
 */

#include <linux/limits.h>

#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>

#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <paths.h>
#include <sched.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <spank.h>

#include "common.h"
#include "seccomp_filter.h"

SPANK_PLUGIN(pyxis, 1)

#define CONTAINER_NAME_FMT "%u.%u"
#define PYXIS_SQUASHFS_FILE PYXIS_USER_RUNTIME_PATH "/" CONTAINER_NAME_FMT ".squashfs"

struct container {
	char *name;
	int userns_fd;
	int mntns_fd;
	int cwd_fd;
};

struct job_info {
	uid_t uid;
	gid_t gid;
	uint32_t jobid;
	uint32_t stepid;
	char **environ;
};

struct plugin_args {
	char *docker_image;
	char **mounts;
	size_t mounts_len;
	char *workdir;
	char *container_name;
	int mount_home;
};

struct plugin_context {
	bool enabled;
	int log_fd;
	struct plugin_args args;
	struct job_info job;
	struct container container;
	int user_init_rv;
};

static struct plugin_context context = {
	.enabled = false,
	.log_fd = -1,
	.args = { .docker_image = NULL, .mounts = NULL, .mounts_len = 0, .workdir = NULL, .container_name = NULL, .mount_home = -1 },
	.job = { .uid = -1, .gid = -1, .jobid = 0, .stepid = 0 },
	.container = { .name = NULL, .userns_fd = -1, .mntns_fd = -1, .cwd_fd = -1 },
	.user_init_rv = 0,
};

static const char *static_mount_entries[] = {
	"none /tmp x-detach,nofail,silent",
	"/tmp /tmp x-create=dir,rw,bind,nosuid,nodev",
	/* PMIX_SERVER_TMPDIR is the only PMIX variable set in the SPANK environment when calling enroot */
	"${PMIX_SERVER_TMPDIR:-/dev/null} ${PMIX_SERVER_TMPDIR:-/dev/null} x-create=dir,rw,bind,nofail,silent",
	NULL
};

static int spank_option_docker_image(int val, const char *optarg, int remote);
static int spank_option_mount(int val, const char *optarg, int remote);
static int spank_option_workdir(int val, const char *optarg, int remote);
static int spank_option_container_name(int val, const char *optarg, int remote);
static int spank_option_container_mount_home(int val, const char *optarg, int remote);

struct spank_option spank_opts[] =
{
	{
		"container-image",
		"[USER@][REGISTRY#]IMAGE[:TAG]",
		"[pyxis] docker image to use, as an enroot URI",
		1, 0, spank_option_docker_image
	},
	{
		"container-mounts",
		"SRC:DST[:FLAGS][,SRC:DST...]",
		"[pyxis] bind mount[s] inside the container. "
		"Mount flags are separated with \"+\", e.g. \"ro+rprivate\"",
		1, 0, spank_option_mount
	},
	{
		"container-workdir",
		"PATH",
		"[pyxis] working directory inside the container",
		1, 0, spank_option_workdir
	},
	{
		"container-name",
		"NAME",
		"[pyxis] name to use for saving and loading the container on the host. "
			"Unnamed containers are removed after the slurm task is complete; named containers are not. "
			"If a container with this name already exists, the existing container is used and the import is skipped.",
		1, 0, spank_option_container_name
	},
	{
		"container-mount-home",
		NULL,
		"[pyxis] bind mount the user's home directory. "
		"System-level enroot settings might cause this directory to be already-mounted.",
		0, 1, spank_option_container_mount_home
	},
	{
		"no-container-mount-home",
		NULL,
		"[pyxis] do not bind mount the user's home directory",
		0, 0, spank_option_container_mount_home
	},
	SPANK_OPTIONS_TABLE_END
};

static int spank_option_docker_image(int val, const char *optarg, int remote)
{
	int ret;

	if (optarg == NULL || *optarg == '\0') {
		slurm_error("pyxis: --container-image: argument required");
		return (-1);
	}

	/* Slurm can call us twice with the same value, check if it's a different value than before. */
	if (context.args.docker_image != NULL) {
		if (strcmp(context.args.docker_image + strlen("docker://"), optarg) == 0)
			return (0);

		slurm_error("pyxis: --container-image specified multiple times");
		return (-1);
	}

	ret = asprintf(&context.args.docker_image, "docker://%s", optarg);
	if (ret < 0) {
		context.args.docker_image = NULL;
		return (-1);
	}

	return (0);
}

static int add_mount_entry(const char *entry)
{
	char **p;

	for (int i = 0; i < context.args.mounts_len; ++i) {
		/* This mount entry already exists, skip it. */
		if (strcmp(context.args.mounts[i], entry) == 0) {
			slurm_info("pyxis: skipping duplicate mount entry: %s", entry);
			return (0);
		}
	}

	p = realloc(context.args.mounts, sizeof(*context.args.mounts) * (context.args.mounts_len + 1));
	if (p == NULL) {
		slurm_error("pyxis: could not allocate memory");
		return (-1);
	}

	p[context.args.mounts_len] = strdup(entry);
	if (p[context.args.mounts_len] == NULL) {
		free(p);
		return (-1);
	}

	context.args.mounts = p;
	context.args.mounts_len += 1;

	return (0);
}

static int add_mount(const char *source, const char *target, const char *flags)
{
	int ret;
	char *entry = NULL;
	int rv = -1;

	if (flags != NULL)
		ret = asprintf(&entry, "%s %s x-create=auto,rbind,%s", source, target, flags);
	else
		ret = asprintf(&entry, "%s %s x-create=auto,rbind", source, target);
	if (ret < 0) {
		slurm_error("pyxis: could not allocate memory");
		goto fail;
	}

	ret = add_mount_entry(entry);
	if (ret < 0)
		goto fail;

	rv = 0;

fail:
	free(entry);

	return (rv);
}

static int spank_option_mount(int val, const char *optarg, int remote)
{
	int ret;
	char *optarg_dup = NULL;
	char *args, *arg, *remainder, *src, *dst, *flags;
	int rv = -1;

	if (optarg == NULL || *optarg == '\0') {
		slurm_error("pyxis: --container-mounts: argument required");
		goto fail;
	}

	optarg_dup = strdup(optarg);
	if (optarg_dup == NULL) {
		slurm_error("pyxis: could not allocate memory");
		goto fail;
	}

	args = optarg_dup;
	while ((arg = strsep(&args, ",")) != NULL) {
		remainder = arg;
		src = strsep(&remainder, ":");
		if (src == NULL || *src == '\0') {
			slurm_error("pyxis: --container-mounts: invalid format %s", optarg);
			goto fail;
		}

		dst = strsep(&remainder, ":");
		if (dst == NULL || *dst == '\0') {
			slurm_error("pyxis: --container-mounts: invalid format %s", optarg);
			goto fail;
		}

		flags = NULL;
		if (remainder != NULL && *remainder != '\0') {
			flags = remainder;
			/*
			 * enroot uses "," as the separator for mount flags, but we already use this character for
			 * separating mount entries, so we use "+" for mount flags and convert to "," here.
			 */
			for (int i = 0; flags[i]; ++i)
				if (flags[i] == '+')
					flags[i] = ',';
		}

		ret = add_mount(src, dst, flags);
		if (ret < 0) {
			slurm_error("pyxis: could not add mount entry: %s:%s", src, dst);
			goto fail;
		}
	}

	rv = 0;

fail:
	free(optarg_dup);

	return (rv);
}

static int spank_option_workdir(int val, const char *optarg, int remote)
{
	if (optarg == NULL || *optarg == '\0') {
		slurm_error("pyxis: --container-workdir: argument required");
		return (-1);
	}

	/* Slurm can call us twice with the same value, check if it's a different value than before. */
	if (context.args.workdir != NULL) {
		if (strcmp(context.args.workdir, optarg) == 0)
			return (0);

		slurm_error("pyxis: --container-workdir specified multiple times");
		return (-1);
	}

	context.args.workdir = strdup(optarg);
	return (0);
}

static int spank_option_container_name(int val, const char *optarg, int remote)
{
	if (optarg == NULL || *optarg == '\0') {
		slurm_error("pyxis: --container-name: argument required");
		return (-1);
	}

	/* Slurm can call us twice with the same value, check if it's a different value than before. */
	if (context.args.container_name != NULL) {
		if (strcmp(context.args.container_name, optarg) == 0)
			return (0);

		slurm_error("pyxis: --container-name specified multiple times");
		return (-1);
	}

	context.args.container_name = strdup(optarg);
	return (0);
}

static int spank_option_container_mount_home(int val, const char *optarg, int remote)
{
	if (context.args.mount_home != -1 && context.args.mount_home != val) {
		slurm_error("pyxis: both --container-mount-home and --no-container-mount-home were specified");
		return (-1);
	}

	context.args.mount_home = val;

	return (0);
}

int slurm_spank_init(spank_t sp, int ac, char **av)
{
	spank_err_t rc;

	/* Slurm bug: see pyxis_slurmd.c */
	if (spank_context() == S_CTX_SLURMD)
		return slurm_spank_slurmd_init(sp, ac, av);

	if (spank_context() != S_CTX_LOCAL && spank_context() != S_CTX_REMOTE)
		return (0);

	if (spank_context() == S_CTX_LOCAL) {
		/*
		 * Show slurm_info() messages by default.
	         * Can get back to default Slurm behavior by setting SLURMD_DEBUG=0.
		 */
		if (setenv("SLURMD_DEBUG", "1", 0) != 0) {
			slurm_error("pyxis: failed to set SLURMD_DEBUG: %s", strerror(errno));
			return (-1);
		}
	}

	for (int i = 0; spank_opts[i].name != NULL; ++i) {
		rc = spank_option_register(sp, &spank_opts[i]);
		if (rc != ESPANK_SUCCESS) {
			slurm_error("pyxis: couldn't register option %s: %s", spank_opts[i].name, spank_strerror(rc));
			return (-1);
		}
	}

	return (0);
}

static int job_get_info(spank_t sp, struct job_info *job)
{
	spank_err_t rc;
	int rv = -1;

	rc = spank_get_item(sp, S_JOB_UID, &job->uid);
	if (rc != ESPANK_SUCCESS) {
		slurm_error("pyxis: couldn't get job uid: %s", spank_strerror(rc));
		goto fail;
	}

	rc = spank_get_item(sp, S_JOB_GID, &job->gid);
	if (rc != ESPANK_SUCCESS) {
		slurm_error("pyxis: couldn't get job gid: %s", spank_strerror(rc));
		goto fail;
	}

	rc = spank_get_item(sp, S_JOB_ID, &job->jobid);
	if (rc != ESPANK_SUCCESS) {
		slurm_error("pyxis: couldn't get job ID: %s", spank_strerror(rc));
		goto fail;
	}

	rc = spank_get_item(sp, S_JOB_STEPID, &job->stepid);
	if (rc != ESPANK_SUCCESS) {
		slurm_error("pyxis: couldn't get job step ID: %s", spank_strerror(rc));
		goto fail;
	}

	rc = spank_get_item(sp, S_JOB_ENV, &job->environ);
	if (rc != ESPANK_SUCCESS) {
		slurm_error("pyxis: couldn't get job environment: %s", spank_strerror(rc));
		goto fail;
	}

	rv = 0;

fail:
	return (rv);
}

/* As root, create the per-user runtime directory where temporary squashfs files are stored. */
static int enroot_create_user_runtime_dir(void)
{
	int ret;
	char path[PATH_MAX];

	ret = snprintf(path, sizeof(path), PYXIS_USER_RUNTIME_PATH, context.job.uid);
	if (ret < 0 || ret >= sizeof(path))
		return (-1);

	ret = mkdir(path, 0700);
	if (ret < 0 && errno != EEXIST) {
		slurm_error("pyxis: couldn't mkdir %s: %s", path, strerror(errno));
		return (-1);
	}
	if (ret < 0 && errno == EEXIST)
		return (0);

	ret = chown(path, context.job.uid, context.job.gid);
	if (ret < 0) {
		slurm_error("pyxis: couldn't chown %s: %s", path, strerror(errno));
		rmdir(path);
		return (-1);
	}

	return (0);
}

int slurm_spank_init_post_opt(spank_t sp, int ac, char **av)
{
	int ret;

	if (context.args.docker_image == NULL && context.args.container_name == NULL) {
		if (context.args.mounts_len > 0)
			slurm_error("pyxis: ignoring --container-mounts because neither --container-image nor --container-name is set");
		if (context.args.workdir != NULL)
			slurm_error("pyxis: ignoring --container-workdir because neither --container-image nor --container-name is set");
		return (0);
	}

	context.enabled = true;

	if (spank_context() != S_CTX_REMOTE)
		return (0);

	/* In slurmstepd context, create the user runtime directory and group cache directory. */
	ret = job_get_info(sp, &context.job);
	if (ret < 0)
		return (-1);

	ret = enroot_create_user_runtime_dir();
	if (ret < 0)
		return (-1);

	return (0);
}

static void enroot_reset_log(void)
{
	xclose(context.log_fd);

	/* We can use CLOEXEC here since we dup2(2) this file descriptor when needed. */
	context.log_fd = pyxis_memfd_create("enroot-log", MFD_CLOEXEC);
	if (context.log_fd < 0)
		slurm_info("pyxis: couldn't create in-memory log file: %s", strerror(errno));

	return;
}

static void enroot_print_last_log(void)
{
	int ret;
	FILE *fp;
	char *line = NULL;
	size_t len = 0;
	ssize_t read;

	ret = lseek(context.log_fd, 0, SEEK_SET);
	if (ret < 0) {
		slurm_info("pyxis: couldn't rewind log file: %s", strerror(errno));
		return;
	}

	fp = fdopen(context.log_fd, "r");
	if (fp == NULL) {
		slurm_info("pyxis: couldn't open in-memory log for printing: %s", strerror(errno));
		return;
	}
	context.log_fd = -1;

	slurm_info("pyxis: printing contents of log file ...");
	while ((read = getline(&line, &len, fp)) != -1) {
		len = strlen(line);
		if (len > 0) {
			if (line[len - 1] == '\n')
				line[len - 1] = '\0'; /* trim trailing newline */
			slurm_error("pyxis:     %s", line);
		}
	}

	free(line);
	fclose(fp);
	return;
}

static const char *enroot_want_env[] = {
	"HOME=",
	"TERM=",
	"NVIDIA_VISIBLE_DEVICES=",
	"MELLANOX_VISIBLE_DEVICES=",
	"ENROOT_CONFIG_PATH=",
	"SLURM_",
	NULL
};

static int enroot_import_job_env(char **env)
{
	for (int i = 0; env[i]; ++i)
		for (int j = 0; enroot_want_env[j]; ++j)
			if (strncmp(env[i], enroot_want_env[j], strlen(enroot_want_env[j])) == 0)
				if (putenv(env[i]) < 0)
					return (-1);

	return (0);
}

static int enroot_set_env(void)
{
	int ret;

	ret = enroot_import_job_env(context.job.environ);
	if (ret < 0)
		return (-1);

	if (context.args.mount_home == 0) {
		ret = setenv("ENROOT_MOUNT_HOME", "n", 1);
	} else if (context.args.mount_home == 1) {
		ret = setenv("ENROOT_MOUNT_HOME", "y", 1);
	} else {
		/* If mount_home was not set by the user, we rely on the setting specified in the enroot config. */
		ret = 0;
	}

	if (ret < 0)
		return (-1);

	return (0);
}

static char* join_strings(char *const strings[])
{
	int len = 0;
	int progress = 0;
	char * result = NULL;

	for (int i=0; strings[i] != NULL; ++i) {
		len += strlen(strings[i]);
		len += 1;
	}
	if (len == 0)
		return NULL;

	result = malloc(len);
	if (result == NULL)
		return NULL;

	for (int i=0; strings[i] != NULL; ++i) {
		if (i != 0)
			result[progress++] = ' ';
		strcpy(&result[progress], strings[i]);
		progress += strlen(strings[i]);
	}
	return result;
}

static pid_t enroot_exec(uid_t uid, gid_t gid, char *const argv[])
{
	int ret;
	int null_fd = -1;
	int target_fd = -1;
	pid_t pid;

	enroot_reset_log();

	char *argv_str = join_strings(argv);
	if (argv_str != NULL) {
		slurm_verbose("pyxis: [verbose] running \"%s\" ...", argv_str);
		free(argv_str);
	}

	pid = fork();
	if (pid < 0) {
		slurm_error("pyxis: fork error: %s", strerror(errno));
		return (-1);
	}

	if (pid == 0) {
		null_fd = open(_PATH_DEVNULL, O_RDWR);
		if (null_fd < 0)
			_exit(EXIT_FAILURE);

		ret = dup2(null_fd, STDIN_FILENO);
		if (ret < 0)
			_exit(EXIT_FAILURE);

		/* Redirect stdout/stderr to the log file or /dev/null */
		if (context.log_fd >= 0)
			target_fd = context.log_fd;
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

		ret = enroot_set_env();
		if (ret < 0)
			_exit(EXIT_FAILURE);

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

static int enroot_exec_wait(uid_t uid, gid_t gid, char *const argv[])
{
	int ret;
	pid_t child;

	child = enroot_exec(uid, gid, argv);
	if (child < 0)
		return (-1);

	ret = child_wait(child);
	if (ret < 0)
		return (-1);

	return (0);
}

static bool enroot_check_container_exists(const char *name)
{
	int ret;
	FILE *fp;
	char *line = NULL;
	size_t len = 0;
	ssize_t read;
	bool rc = false;

	if (name == NULL || strlen(name) == 0)
		return (false);

	ret = enroot_exec_wait(context.job.uid, context.job.gid, (char *const[]){ "enroot", "list", NULL });
	if (ret < 0) {
		slurm_error("pyxis: couldn't get list of existing container filesystems");
		enroot_print_last_log();
		return (false);
	}

	/* Typically, the log is used to show stderr from failed commands. But here we parse the output. */
	ret = lseek(context.log_fd, 0, SEEK_SET);
	if (ret < 0) {
		slurm_info("pyxis: couldn't rewind log file: %s", strerror(errno));
		return (false);
	}

	fp = fdopen(context.log_fd, "r");
	if (fp == NULL) {
		slurm_info("pyxis: couldn't open in-memory log for printing: %s", strerror(errno));
		return (false);
	}
	context.log_fd = -1;

	while ((read = getline(&line, &len, fp)) != -1) {
		len = strlen(line);
		if (len > 0) {
			if (line[len - 1] == '\n')
				line[len - 1] = '\0'; /* trim trailing newline */
			if (strcmp(line, name) == 0) {
				rc = true;
				break;
			}
		}
	}

	free(line);
	fclose(fp);
	return (rc);
}

static int read_proc_environ(pid_t pid, char **result, size_t *size)
{
	int ret;
	char path[PATH_MAX];
	int fd = -1;
	char *buf = NULL;
	size_t len = 0, capacity = 1024;
	char *new_buf = NULL;
	int rv = -1;

	ret = snprintf(path, sizeof(path), "/proc/%d/environ", pid);
	if (ret < 0 || ret >= sizeof(path))
		goto fail;

	fd = open(path, O_RDONLY);
	if (fd < 0)
		goto fail;

	buf = malloc(capacity + 1);
	if (buf == NULL)
		goto fail;

	while ((ret = read(fd, buf + len, capacity - len)) > 0) {
		len += ret;

		if (capacity - len == 0) {
			/* Grow buffer. */
			capacity *= 2;

			new_buf = realloc(buf, capacity + 1);
			if (new_buf == NULL)
				goto fail;

			buf = new_buf;
		}
	}

	if (ret < 0)
		goto fail;

	/* From man 5 proc, there might not be a null byte at the end. */
	if (len > 0 && buf[len - 1] != '\0') {
		buf[len] = '\0';
		len += 1;
	}

	*result = buf;
	*size = len;
	rv = 0;

fail:
	xclose(fd);
	if (rv < 0)
		free(buf);

	return (rv);
}

static int spank_import_container_env(spank_t sp, pid_t pid)
{
	int ret;
	char *proc_environ = NULL;
	size_t size;
	spank_err_t rc;
	int rv = -1;

	ret = read_proc_environ(pid, &proc_environ, &size);
	if (ret < 0) {
		slurm_error("pyxis: couldn't read /proc/<pid>/environ");
		goto fail;
	}

	for (size_t i = 0; i < size;) {
		char *key, *value;

		value = proc_environ + i;
		key = strsep(&value, "=");

		rc = spank_setenv(sp, key, value, 1);
		if (rc != ESPANK_SUCCESS) {
			slurm_error("pyxis: failed to set %s: %s", key, spank_strerror(rc));
			goto fail;
		}
		i += strlen(key) + 1 + strlen(value) + 1;
	}

	rv = 0;

fail:
	free(proc_environ);
	return (rv);
}

static int enroot_container_create(spank_t sp)
{
	int ret;
	char squashfs_path[PATH_MAX] = { 0 };
	int rv = -1;

	ret = snprintf(squashfs_path, sizeof(squashfs_path), PYXIS_SQUASHFS_FILE, context.job.uid, context.job.jobid, context.job.stepid);
	if (ret < 0 || ret >= sizeof(squashfs_path))
		goto fail;

	slurm_info("pyxis: importing docker image ...");

	ret = enroot_exec_wait(context.job.uid, context.job.gid,
			       (char *const[]){ "enroot", "import", "--output", squashfs_path, context.args.docker_image, NULL });
	if (ret < 0) {
		slurm_error("pyxis: failed to import docker image");
		enroot_print_last_log();
		return (-1);
	}

	if (context.args.container_name == NULL) {
		ret = asprintf(&context.container.name, CONTAINER_NAME_FMT, context.job.jobid, context.job.stepid);
		if (ret < 0)
			goto fail;
	} else {
		context.container.name = strdup(context.args.container_name);
	}

	slurm_info("pyxis: creating container filesystem ...");

	ret = enroot_exec_wait(context.job.uid, context.job.gid,
			       (char *const[]){ "enroot", "create", "--name", context.container.name, squashfs_path, NULL });
	if (ret < 0) {
		slurm_error("pyxis: failed to create container filesystem");
		enroot_print_last_log();
		goto fail;
	}

	rv = 0;

fail:
	if (*squashfs_path != '\0') {
		ret = unlink(squashfs_path);
		if (ret < 0)
			slurm_info("pyxis: could not remove squashfs: %s", strerror(errno));
	}

	return (rv);
}

static int container_get_fds(pid_t pid, struct container *container)
{
	int ret;
	char userns_path[PATH_MAX];
	char mntns_path[PATH_MAX];
	char cwd_path[PATH_MAX];
	int rv = -1;

	ret = snprintf(userns_path, sizeof(userns_path), "/proc/%d/ns/user", pid);
	if (ret < 0 || ret >= sizeof(userns_path))
		goto fail;

	container->userns_fd = open(userns_path, O_RDONLY | O_CLOEXEC);
	if (container->userns_fd < 0) {
		slurm_error("pyxis: unable to open user namespace file: %s", strerror(errno));
		goto fail;
	}

	ret = snprintf(mntns_path, sizeof(mntns_path), "/proc/%d/ns/mnt", pid);
	if (ret < 0 || ret >= sizeof(mntns_path))
		goto fail;

	container->mntns_fd = open(mntns_path, O_RDONLY | O_CLOEXEC);
	if (container->mntns_fd < 0) {
		slurm_error("pyxis: unable to open mount namespace file: %s", strerror(errno));
		goto fail;
	}

	ret = snprintf(cwd_path, sizeof(cwd_path), "/proc/%d/cwd", pid);
	if (ret < 0 || ret >= sizeof(cwd_path))
		goto fail;

	container->cwd_fd = open(cwd_path, O_RDONLY | O_CLOEXEC);
	if (container->cwd_fd < 0) {
		slurm_error("pyxis: couldn't open cwd fd");
		goto fail;
	}

	rv = 0;

fail:
	return (rv);
}

static int enroot_create_start_config(void)
{
	int fd = -1;
	int dup_fd = -1;
	FILE *f = NULL;
	char template[] = "/tmp/.enroot_config_XXXXXX";
	int ret;
	int rv = -1;

	fd = mkstemp(template);
	if (fd < 0)
		goto fail;

	dup_fd = dup(fd);
	if (dup_fd < 0)
		goto fail;

	f = fdopen(dup_fd, "a+");
	if (f == NULL)
		goto fail;
	dup_fd = -1;

	ret = fprintf(f, "mounts() {\n");
	if (ret < 0)
		goto fail;

	/* static mount entries */
	for (int i = 0; static_mount_entries[i] != NULL; ++i) {
		ret = fprintf(f, "\techo \"%s\"\n", static_mount_entries[i]);
		if (ret < 0)
			goto fail;
	}

	/* bind mount entries */
	for (int i = 0; i < context.args.mounts_len; ++i) {
		ret = fprintf(f, "\techo \"%s\"\n", context.args.mounts[i]);
		if (ret < 0)
			goto fail;
	}

	ret = fprintf(f, "}\n");
	if (ret < 0)
		goto fail;

	/* print contents */
	if (fseek(f, 0, SEEK_SET) == 0) {
		char * line;
		size_t len;
		slurm_verbose("pyxis: [verbose] enroot start configuration script:");
		while (getline(&line, &len, f) != -1) {
			len = strlen(line);
			if (len > 0) {
				if (line[len - 1] == '\n')
					line[len - 1] = '\0'; /* trim trailing newline */
				slurm_verbose("pyxis:     %s", line);
			}
		}
	}

	rv = fd;

fail:
	xclose(dup_fd);
	if (f != NULL)
		fclose(f);
	if (rv < 0)
		xclose(fd);

	return (rv);
}

static int enroot_container_start(spank_t sp)
{
	int ret;
	int conf_fd = -1;
	char *conf_file = NULL;
	pid_t pid = -1;
	int status;
	int rv = -1;

	slurm_info("pyxis: starting container ...");

	conf_fd = enroot_create_start_config();
	if (conf_fd < 0) {
		slurm_error("pyxis: couldn't create enroot start configuration script");
		goto fail;
	}

	ret = asprintf(&conf_file, "/proc/self/fd/%d", conf_fd);
	if (ret < 0) {
		slurm_error("pyxis: could not allocate memory");
		goto fail;
	}

	/*
	 * The plugin starts the container as a subprocess and acquires handles on the
	 * container's namespaces. We must do this after the container runtime has called
	 * unshare(2) and pivot_root(2). To synchronize the plugin and the container, the
	 * shell inside the container sends itself SIGSTOP through the command "kill -STOP
	 * $$" and the plugin waits for the container to be stopped by calling waitpid(2)
	 * with the WUNTRACED option.
	 * This requires a shell inside the container, but we could do the same with a
	 * small static C program bind-mounted inside the container.
	*/
	pid = enroot_exec(context.job.uid, context.job.gid,
			  (char *const[]){ "enroot", "start", "--root", "--rw", "--conf", conf_file, context.container.name, "sh", "-c",
					   "kill -STOP $$ ; exit 0", NULL });
	if (pid < 0) {
		slurm_error("pyxis: failed to start container");
		goto fail;
	}

	/* Wait for the child to terminate or stop itself (with WUNTRACED). */
	ret = waitpid(pid, &status, WUNTRACED);
	if (ret < 0) {
		slurm_error("pyxis: container start error: %s", strerror(errno));
		goto fail;
	}

	if (WIFEXITED(status) && (ret = WEXITSTATUS(status)) != 0) {
		slurm_error("pyxis: container start failed with error code: %d", ret);
		goto fail;
	}

	if (!WIFSTOPPED(status)) {
		slurm_error("pyxis: container exited too soon, possibly due to an unusual entrypoint in the image");
		goto fail;
	}

	ret = container_get_fds(pid, &context.container);
	if (ret < 0) {
		slurm_error("pyxis: couldn't get container attributes");
		goto fail;
	}

	ret = spank_import_container_env(sp, pid);
	if (ret < 0) {
		slurm_error("pyxis: couldn't read container environment");
		goto fail;
	}

	ret = kill(pid, SIGCONT);
	if (ret < 0) {
		slurm_error("pyxis: couldn't send SIGCONT to container process: %s", strerror(errno));
		goto fail;
	}

	ret = child_wait(pid);
	if (ret < 0) {
		slurm_error("pyxis: container process terminated");
		goto fail;
	}

	rv = 0;

fail:
	if (rv < 0)
		enroot_print_last_log();
	free(conf_file);
	xclose(conf_fd);

	return (rv);
}

int slurm_spank_user_init(spank_t sp, int ac, char **av)
{
	int ret;
	int rv = -1;

	if (!context.enabled)
		return (0);

	if (context.args.container_name != NULL) {
		if (enroot_check_container_exists(context.args.container_name)) {
			slurm_info("pyxis: reusing existing container");
			context.container.name = strdup(context.args.container_name);
		} else if (context.args.docker_image == NULL) {
			slurm_error("pyxis: a container with name \"%s\" does not exist, and --container-image is not set",
				    context.args.container_name);
			goto fail;
		}
	}

	if (context.container.name == NULL) {
		ret = enroot_container_create(sp);
		if (ret < 0)
			goto fail;
	}

	ret = enroot_container_start(sp);
	if (ret < 0)
		goto fail;

	rv = 0;

fail:
	/*
	 * Errors from user_init() are not propagated back to srun. Rather than fail here and have srun
	 * report rc=0 (success), we return 0 here and throw the error in task_init_privileged()
	 * instead, which will properly propagate the error back to srun.
	 *
	 * See https://bugs.schedmd.com/show_bug.cgi?id=7573 for more details.
	 */
	slurm_debug("pyxis: user_init() failed with rc=%d; postponing error for now, will report later", rv);
	context.user_init_rv = rv;
	return (0);
}

static int spank_copy_env(spank_t sp, const char *from, const char *to, int overwrite)
{
	char buf[256];
	spank_err_t rc;

	rc = spank_getenv(sp, from, buf, sizeof(buf));
	if (rc == ESPANK_ENV_NOEXIST)
		return (0);
	else if (rc != ESPANK_SUCCESS)
		return (-1);

	rc = spank_setenv(sp, to, buf, overwrite);
	if (rc != ESPANK_SUCCESS) {
		slurm_error("pyxis: failed to set %s: %s", to, spank_strerror(rc));
		return (-1);
	}

	return (0);
}

static bool pmix_enabled(spank_t sp)
{
	spank_err_t rc;

	rc = spank_getenv(sp, "PMIX_RANK", NULL, 0);
	if (rc == ESPANK_ENV_NOEXIST || rc != ESPANK_NOSPACE)
		return (false);

	return (true);
}

static struct {
	const char *from;
	const char *to;
} pmix_remap_list[] = {
	{ "PMIX_SECURITY_MODE", "PMIX_MCA_psec" },
	{ "PMIX_GDS_MODULE", "PMIX_MCA_gds" },
	{ "PMIX_PTL_MODULE", "PMIX_MCA_ptl" },
	{ NULL, NULL }
};

/*
 * Since the MCA parameters file won't be accessible from inside the container, we need to
 * set the 3 essential PMIx variables: PMIX_MCA_psec, PMIX_MCA_gds and PMIX_MCA_ptl.
 *
 * Note that we can't use the PMIx/PMI hook from enroot since the PMIx environment
 * variables are not set yet when we execute "enroot start".
 */
static int pmix_setup(spank_t sp)
{
	int ret;

	for (int i = 0; pmix_remap_list[i].from != NULL; ++i) {
		ret = spank_copy_env(sp, pmix_remap_list[i].from, pmix_remap_list[i].to, 1);
		if (ret < 0) {
			slurm_error("pyxis: pmix: couldn't remap environment variable %s", pmix_remap_list[i].from);
			return (-1);
		}
	}

	return (0);
}

static bool pytorch_setup_needed(spank_t sp)
{
	spank_err_t rc;

	rc = spank_getenv(sp, "PYTORCH_VERSION", NULL, 0);
	if (rc == ESPANK_ENV_NOEXIST || rc != ESPANK_NOSPACE)
		return (false);

	return (true);
}

static struct {
	const char *from;
	const char *to;
} pytorch_remap_list[] = {
	{ "SLURM_PROCID", "RANK" },
	{ "SLURM_LOCALID", "LOCAL_RANK" },
	{ NULL, NULL }
};

/*
 * Here, we remap a few variables so that Pytorch multi-process and multi-node works well with
 * pyxis, even though PyTorch does not use MPI.
 *
 * Some other variables are handled with an enroot hook, but these are not available yet in
 * slurm_spank_user_init(), and must be remapped later, when initializing specific tasks.
 */
static int pytorch_setup(spank_t sp)
{
	int ret;

	for (int i = 0; pytorch_remap_list[i].from != NULL; ++i) {
		ret = spank_copy_env(sp, pytorch_remap_list[i].from, pytorch_remap_list[i].to, 1);
		if (ret < 0) {
			slurm_error("pyxis: pytorch: couldn't remap environment variable %s", pytorch_remap_list[i].from);
			return (-1);
		}
	}

	return (0);
}

/*
 * We used to join the mount namespace in slurm_spank_task_init(), but had to move it to
 * slurm_spank_task_init_privileged() to overcome a limitation of the SPANK API.
 *
 * Slurm currently uses execve(2) instead of execvpe(2), hence it needs to
 * resolve the full path of the binary to execute. This resolution is done just
 * before the call to the slurm_spank_task_init callback, so it is done against
 * the host filesystem. For Slurm 18.08.7, see function exec_task() in
 * src/slurmd/slurmstepd/task.c, the path resolution function is _build_path()
 *
 * The workaround is to join the mount namespace before the call to
 * _build_path(), and in terms of SPANK API calls, that is slurm_spank_task_init_privileged().
 *
 * See https://bugs.schedmd.com/show_bug.cgi?id=7257
 */
int slurm_spank_task_init_privileged(spank_t sp, int ac, char **av)
{
	int ret;

	if (!context.enabled)
		return (0);

	if (context.user_init_rv != 0)
		return (context.user_init_rv);

	ret = setns(context.container.mntns_fd, CLONE_NEWNS);
	if (ret < 0) {
		slurm_error("pyxis: couldn't join mount namespace: %s", strerror(errno));
		return (-1);
	}

	/* No need to chdir(root) + chroot(".") since enroot does a pivot_root. */
	if (context.args.workdir != NULL) {
		ret = chdir(context.args.workdir);
		if (ret < 0) {
			slurm_error("pyxis: couldn't chdir to %s: %s", context.args.workdir, strerror(errno));
			return (-1);
		}
	} else {
		ret = fchdir(context.container.cwd_fd);
		if (ret < 0) {
			slurm_error("pyxis: couldn't chdir to container cwd: %s", strerror(errno));
			return (-1);
		}
	}

	return (0);
}

int slurm_spank_task_init(spank_t sp, int ac, char **av)
{
	int ret;
	int rv = -1;

	if (!context.enabled)
		return (0);

	if (pmix_enabled(sp)) {
		ret = pmix_setup(sp);
		if (ret < 0)
			goto fail;
	}

	if (pytorch_setup_needed(sp)) {
		ret = pytorch_setup(sp);
		if (ret < 0)
			goto fail;
	}

	ret = setns(context.container.userns_fd, CLONE_NEWUSER);
	if (ret < 0) {
		slurm_error("pyxis: couldn't join user namespace: %s", strerror(errno));
		goto fail;
	}

	/* The user will see themself as (remapped) uid/gid 0 inside the container */
	ret = setgid(0);
	if (ret < 0) {
		slurm_error("pyxis: setgid failed");
		goto fail;
	}

	ret = setuid(0);
	if (ret < 0) {
		slurm_error("pyxis: setuid failed");
		goto fail;
	}

	ret = seccomp_set_filter();
	if (ret < 0) {
		slurm_error("pyxis: seccomp filter failed: %s", strerror(errno));
		goto fail;
	}

	rv = 0;

fail:
	return (rv);
}

int slurm_spank_exit(spank_t sp, int ac, char **av)
{
	int ret;

	if (context.container.name != NULL && context.args.container_name == NULL) {
		slurm_info("pyxis: removing container filesystem ...");

		ret = enroot_exec_wait(context.job.uid, context.job.gid,
				       (char *const[]){ "enroot", "remove", "-f", context.container.name, NULL });
		if (ret < 0) {
			slurm_info("pyxis: failed to remove container filesystem");
			enroot_print_last_log();
		}
	}
	free(context.container.name);

	free(context.args.docker_image);
	for (int i = 0; i < context.args.mounts_len; ++i)
		free(context.args.mounts[i]);
	free(context.args.mounts);
	free(context.args.workdir);
	free(context.args.container_name);

	xclose(context.container.userns_fd);
	xclose(context.container.mntns_fd);
	xclose(context.container.cwd_fd);
	xclose(context.log_fd);

	memset(&context, 0, sizeof(context));

	return (0);
}
