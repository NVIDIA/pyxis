/*
 * Copyright (c) 2019-2020, NVIDIA CORPORATION. All rights reserved.
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
#include <pthread.h>
#include <sched.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <slurm/spank.h>

#include "pyxis_slurmstepd.h"
#include "common.h"
#include "config.h"
#include "args.h"
#include "seccomp_filter.h"

struct container {
	char *name;
	char *save_path;
	bool reuse_rootfs;
	bool reuse_pid;
	bool temporary;
	int userns_fd;
	int mntns_fd;
	int cgroupns_fd;
	int cwd_fd;
};

struct job_info {
	uid_t uid;
	gid_t gid;
	uint32_t jobid;
	uint32_t stepid;
	uint32_t local_task_count;
	char **environ;
	char cwd[PATH_MAX];
};

struct shared_memory {
	pthread_mutex_t mutex;
	uint32_t init_tasks;
	uint32_t started_tasks;
	pid_t pid;
};

struct plugin_context {
	bool enabled;
	int log_fd;
	struct plugin_config config;
	struct plugin_args *args;
	struct job_info job;
	struct container container;
	int user_init_rv;
	struct shared_memory *shm;
};

static struct plugin_context context = {
	.enabled = false,
	.log_fd = -1,
	.config = { .runtime_path = { 0 } },
	.args = NULL,
	.job = { .uid = -1, .gid = -1, .jobid = 0, .stepid = 0, .environ = NULL, .cwd = { 0 } },
	.container = { .name = NULL, .save_path = NULL, .reuse_rootfs = false, .reuse_pid = false, .temporary = false, .userns_fd = -1, .mntns_fd = -1, .cgroupns_fd = -1, .cwd_fd = -1 },
	.user_init_rv = 0,
};

static bool pyxis_remap_root(void)
{
	return context.args->remap_root == 1 || (context.args->remap_root == -1 && context.config.remap_root == true);
}

static bool pyxis_execute_entrypoint(void)
{
	return context.args->entrypoint == 1 || (context.args->entrypoint == -1 && context.config.execute_entrypoint == true);
}

int pyxis_slurmstepd_init(spank_t sp, int ac, char **av)
{
	int ret;

	ret = pyxis_config_parse(&context.config, ac, av);
	if (ret < 0) {
		slurm_error("pyxis: failed to parse configuration");
		return (-1);
	}

	context.args = pyxis_args_register(sp);
	if (context.args == NULL) {
		slurm_error("pyxis: failed to register arguments");
		return (-1);
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

	rc = spank_get_item(sp, S_JOB_LOCAL_TASK_COUNT, &job->local_task_count);
	if (rc != ESPANK_SUCCESS) {
		slurm_error("pyxis: couldn't get job local task count: %s", spank_strerror(rc));
		goto fail;
	}

	/* This should probably be added to the API as a spank_item */
	rc = spank_getenv(sp, "PWD", job->cwd, sizeof(job->cwd));
	if (rc != ESPANK_SUCCESS)
		slurm_info("pyxis: couldn't get job cwd path: %s", spank_strerror(rc));

	rv = 0;

fail:
	return (rv);
}

static int job_get_env(spank_t sp, struct job_info *job)
{
	spank_err_t rc;
	char **spank_environ = NULL;
	size_t environ_len = 0;
	int rv = -1;

	rc = spank_get_item(sp, S_JOB_ENV, &spank_environ);
	if (rc != ESPANK_SUCCESS) {
		slurm_error("pyxis: couldn't get job environment: %s", spank_strerror(rc));
		goto fail;
	}

	if (job->environ != NULL) {
		for (int i = 0; job->environ[i] != NULL; ++i)
			free(job->environ[i]);
		free(job->environ);
		job->environ = NULL;
	}

	/* We need to make a copy of the environment returned by the SPANK API. */
	for (size_t i = 0; spank_environ[i] != NULL; ++i)
		environ_len += 1;

	job->environ = malloc((environ_len + 1) * sizeof(char*));
	if (job->environ == NULL)
		goto fail;
	job->environ[environ_len] = NULL;

	for (size_t i = 0; i < environ_len; ++i) {
		job->environ[i] = strdup(spank_environ[i]);
		if (job->environ[i] == NULL)
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

	ret = snprintf(path, sizeof(path), "%s/%u", context.config.runtime_path, context.job.uid);
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

int pyxis_slurmstepd_post_opt(spank_t sp, int ac, char **av)
{
	int ret;

	if (!pyxis_args_enabled())
		return (0);

	context.enabled = true;

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
	char *line;

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

	slurm_error("pyxis: printing contents of log file ...");
	while ((line = get_line_from_file(fp)) != NULL) {
		slurm_error("pyxis:     %s", line);
		free(line);
	}

	fclose(fp);
	return;
}

static const char *enroot_want_env[] = {
	"HOME=",
	"TERM=",
	"NVIDIA_VISIBLE_DEVICES=",
	"NVIDIA_DRIVER_CAPABILITIES=",
	"MELLANOX_VISIBLE_DEVICES=",
	"MELLANOX_MOUNT_DRIVER=",
	"ENROOT_CONFIG_PATH=",
	/* Need to limit which SLURM variables are passed, to avoid enroot overriding variables such as SLURM_LOCALID */
	"SLURM_JOB_", "SLURM_STEP_",
	"SLURM_MPI_TYPE=", "SLURM_NODELIST=", "SLURM_NTASKS=",
	"PMIX_SECURITY_MODE=", "PMIX_GDS_MODULE=", "PMIX_PTL_MODULE=",
	NULL
};

static int enroot_import_job_env(char **env)
{
	if (env == NULL)
		return (-1);

	for (int i = 0; env[i]; ++i)
		for (int j = 0; enroot_want_env[j]; ++j)
			if (strncmp(env[i], enroot_want_env[j], strlen(enroot_want_env[j])) == 0)
				if (putenv(env[i]) < 0)
					return (-1);

	return (0);
}

static int enroot_set_env(void)
{
	if (enroot_import_job_env(context.job.environ) < 0)
		return (-1);

	if (context.args->mount_home == 0) {
		if (setenv("ENROOT_MOUNT_HOME", "n", 1) < 0)
			return (-1);
	} else if (context.args->mount_home == 1) {
		if (setenv("ENROOT_MOUNT_HOME", "y", 1) < 0)
			return (-1);
	} else {
		/* If mount_home was not set by the user, we rely on the setting specified in the enroot config. */
	}

	if (pyxis_remap_root()) {
		if (setenv("ENROOT_REMAP_ROOT", "y", 1) < 0)
			return (-1);
	} else {
		if (setenv("ENROOT_REMAP_ROOT", "n", 1) < 0)
			return (-1);
	}

	return (0);
}

static pid_t enroot_exec(uid_t uid, gid_t gid, char *const argv[])
{
	int ret;
	int null_fd = -1;
	int target_fd = -1;
	pid_t pid;
	char *argv_str;

	enroot_reset_log();

	argv_str = join_strings(argv, " ");
	if (argv_str != NULL) {
		slurm_verbose("pyxis: running \"%s\" ...", argv_str);
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

/*
 * Returns -1 if an error occurs.
 * Returns 0 and set *pid==-1 if container doesn't exist.
 * Returns 0 and set *pid==0 if container exists but is not running.
 * Returns 0 and set *pid>0 (the container PID) if the container exists and is running.
 */
static int enroot_container_get(const char *name, pid_t *pid)
{
	int ret;
	FILE *fp;
	char *line;
	char *ctr_name, *ctr_pid, *saveptr;
	unsigned long n;
	int rv = -1;

	if (name == NULL || strlen(name) == 0 || pid == NULL)
		return (-1);

	*pid = -1;

	ret = enroot_exec_wait(context.job.uid, context.job.gid, (char *const[]){ "enroot", "list", "-f", NULL });
	if (ret < 0) {
		slurm_error("pyxis: couldn't get list of existing container filesystems");
		enroot_print_last_log();
		return (-1);
	}

	/* Typically, the log is used to show stderr from failed commands. But here we parse the output. */
	ret = lseek(context.log_fd, 0, SEEK_SET);
	if (ret < 0) {
		slurm_info("pyxis: couldn't rewind log file: %s", strerror(errno));
		return (-1);
	}

	fp = fdopen(context.log_fd, "r");
	if (fp == NULL) {
		slurm_info("pyxis: couldn't open in-memory log for printing: %s", strerror(errno));
		return (-1);
	}
	context.log_fd = -1;

	/* Skip headers line */
	line = get_line_from_file(fp);
	if (line == NULL) {
		slurm_error("pyxis: \"enroot list -f\" did not produce any usable output");
		goto fail;
	}

	while ((line = get_line_from_file(fp)) != NULL) {
		ctr_name = strtok_r(line, " ", &saveptr);
		if (ctr_name == NULL || *ctr_name == '\0')
			goto fail;

		if (strcmp(name, ctr_name) == 0) {
			ctr_pid = strtok_r(NULL, " ", &saveptr);
			if (ctr_pid == NULL || *ctr_pid == '\0') {
				*pid = 0;
			} else {
				errno = 0;
				n = strtoul(ctr_pid, NULL, 10);
				if (errno != 0 || n != (pid_t)n)
					goto fail;

				*pid = (pid_t)n;
			}

			break;
		}

		free(line);
		line = NULL;
	}

	rv = 0;

fail:
	free(line);
	fclose(fp);
	return (rv);
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

static int enroot_container_create(void)
{
	int ret;
	char squashfs_path[PATH_MAX];
	char *enroot_uri = NULL;
	bool should_delete_squashfs = false;
	int rv = -1;

	if (strspn(context.args->image, "./") > 0) {
		/* Assume `image` is a path to a squashfs file. */
		if (strnlen(context.args->image, sizeof(squashfs_path)) >= sizeof(squashfs_path))
			goto fail;

		strncpy(squashfs_path, context.args->image, sizeof(squashfs_path) - 1);
		squashfs_path[sizeof(squashfs_path) - 1] = '\0';
	} else {
		/* Assume `image` is an enroot URI for a docker image. */
		ret = xasprintf(&enroot_uri, "docker://%s", context.args->image);
		if (ret < 0)
			goto fail;

		ret = snprintf(squashfs_path, sizeof(squashfs_path), "%s/%u/%u.%u.squashfs", context.config.runtime_path, context.job.uid, context.job.jobid, context.job.stepid);
		if (ret < 0 || ret >= sizeof(squashfs_path))
			goto fail;

		should_delete_squashfs = true;

		slurm_spank_log("pyxis: importing docker image ...");

		ret = enroot_exec_wait(context.job.uid, context.job.gid,
				       (char *const[]){ "enroot", "import", "--output", squashfs_path, enroot_uri, NULL });
		if (ret < 0) {
			slurm_error("pyxis: failed to import docker image");
			enroot_print_last_log();
			goto fail;
		}
	}

	slurm_spank_log("pyxis: creating container filesystem ...");

	ret = enroot_exec_wait(context.job.uid, context.job.gid,
			       (char *const[]){ "enroot", "create", "--name", context.container.name, squashfs_path, NULL });
	if (ret < 0) {
		slurm_error("pyxis: failed to create container filesystem");
		enroot_print_last_log();
		goto fail;
	}

	rv = 0;

fail:
	free(enroot_uri);
	if (should_delete_squashfs && *squashfs_path != '\0') {
		ret = unlink(squashfs_path);
		if (ret < 0)
			slurm_info("pyxis: could not remove squashfs: %s", strerror(errno));
	}

	return (rv);
}

static int container_get_fds(pid_t pid, struct container *container)
{
	int ret;
	char path[PATH_MAX];
	int rv = -1;

	ret = snprintf(path, sizeof(path), "/proc/%d/ns/user", pid);
	if (ret < 0 || ret >= sizeof(path))
		goto fail;

	container->userns_fd = open(path, O_RDONLY | O_CLOEXEC);
	if (container->userns_fd < 0) {
		slurm_error("pyxis: unable to open user namespace file: %s", strerror(errno));
		goto fail;
	}

	ret = snprintf(path, sizeof(path), "/proc/%d/ns/mnt", pid);
	if (ret < 0 || ret >= sizeof(path))
		goto fail;

	container->mntns_fd = open(path, O_RDONLY | O_CLOEXEC);
	if (container->mntns_fd < 0) {
		slurm_error("pyxis: unable to open mount namespace file: %s", strerror(errno));
		goto fail;
	}

	ret = snprintf(path, sizeof(path), "/proc/%d/ns/cgroup", pid);
	if (ret < 0 || ret >= sizeof(path))
		goto fail;

	container->cgroupns_fd = open(path, O_RDONLY | O_CLOEXEC);
	/* Skip cgroup namespace if not supported */
	if (container->cgroupns_fd < 0 && errno != ENOENT) {
		slurm_error("pyxis: unable to open cgroup namespace file: %s", strerror(errno));
		goto fail;
	}

	ret = snprintf(path, sizeof(path), "/proc/%d/cwd", pid);
	if (ret < 0 || ret >= sizeof(path))
		goto fail;

	container->cwd_fd = open(path, O_RDONLY | O_CLOEXEC);
	if (container->cwd_fd < 0) {
		slurm_error("pyxis: couldn't open cwd fd");
		goto fail;
	}

	rv = 0;

fail:
	return (rv);
}

static int enroot_create_start_config(char (*path)[PATH_MAX])
{
	int ret;
	int fd = -1;
	FILE *f = NULL;
	char template[] = "/tmp/.enroot_config_XXXXXX";
	char *line = NULL;
	int rv = -1;

	fd = mkstemp(template);
	if (fd < 0)
		goto fail;

	f = fdopen(fd, "a+");
	if (f == NULL)
		goto fail;

	if (context.args->mounts_len > 0) {
		ret = fprintf(f, "mounts() {\n");
		if (ret < 0)
			goto fail;

		/* bind mount entries */
		for (int i = 0; i < context.args->mounts_len; ++i) {
			ret = fprintf(f, "\techo \"%s\"\n", context.args->mounts[i]);
			if (ret < 0)
				goto fail;
		}

		ret = fprintf(f, "}\n");
		if (ret < 0)
			goto fail;
	}

	if (!pyxis_execute_entrypoint()) {
		ret = fprintf(f, "hooks() {\n");
		if (ret < 0)
			goto fail;

		/*
		 * /etc/rc.local will be sourced by /etc/rc.
		 * We call 'exec' from there and do not return control to /etc/rc.
		 */
		ret = fprintf(f, "\techo 'exec \"$@\"' > ${ENROOT_ROOTFS}/etc/rc.local\n");
		if (ret < 0)
			goto fail;

		ret = fprintf(f, "}\n");
		if (ret < 0)
			goto fail;
	}

	/* print contents */
	if (fseek(f, 0, SEEK_SET) == 0) {
		slurm_verbose("pyxis: enroot start configuration script:");
		while ((line = get_line_from_file(f)) != NULL) {
			slurm_verbose("pyxis:     %s", line);
			free(line);
		}
	}

	if (memccpy(*path, template, '\0', sizeof(*path)) == NULL)
		goto fail;

	rv = 0;

fail:
	if (f != NULL)
		fclose(f);
	xclose(fd);

	return (rv);
}

static pid_t enroot_container_start(void)
{
	int ret;
	char conf_file[PATH_MAX] = { 0 };
	pid_t pid = -1;
	int status;
	pid_t rv = -1;

	slurm_spank_log("pyxis: starting container ...");

	ret = enroot_create_start_config(&conf_file);
	if (ret < 0) {
		slurm_error("pyxis: couldn't create enroot start configuration script");
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
			  (char *const[]){ "enroot", "start", "--conf", conf_file, context.container.name, "sh", "-c",
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
		slurm_error("pyxis: container exited too soon");
		goto fail;
	}

	rv = pid;

fail:
	if (rv == -1)
		enroot_print_last_log();
	if (conf_file[0] != '\0')
		unlink(conf_file);

	return (rv);
}

static int enroot_container_stop(pid_t pid)
{
	int ret;

	if (pid <= 0)
		return (-1);

	ret = kill(pid, SIGCONT);
	if (ret < 0) {
		slurm_error("pyxis: couldn't send SIGCONT to container process: %s", strerror(errno));
		return (-1);
	}

	return (0);
}

static struct shared_memory *shm_init(void)
{
	struct shared_memory *shm = NULL;
	pthread_mutexattr_t mutex_attr;
	int ret;

	shm = mmap(0, sizeof(*shm), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	if (shm == MAP_FAILED) {
		shm = NULL;
		goto fail;
	}

	ret = pthread_mutexattr_init(&mutex_attr);
	if (ret < 0)
		goto fail;

	ret = pthread_mutexattr_setpshared(&mutex_attr, PTHREAD_PROCESS_SHARED);
	if (ret < 0)
		goto fail;

	ret = pthread_mutex_init(&shm->mutex, &mutex_attr);
	if (ret < 0)
		goto fail;

	shm->init_tasks = 0;
	shm->started_tasks = 0;
	shm->pid = -1;

	return shm;

fail:
	if (shm != NULL)
		munmap(shm, sizeof(*shm));
	return (NULL);
}

static int shm_destroy(struct shared_memory *shm)
{
	int ret;

	if (shm == NULL)
		return (0);

	ret = pthread_mutex_destroy(&shm->mutex);
	if (ret < 0)
		return (-1);

	ret = munmap(shm, sizeof(*shm));
	if (ret < 0)
		return (-1);

	return (0);
}

int slurm_spank_user_init(spank_t sp, int ac, char **av)
{
	int ret;
	char *container_name = NULL;
	pid_t pid;
	int rv = -1;

	if (!context.enabled)
		return (0);

	context.shm = shm_init();
	if (context.shm == NULL)
		goto fail;

	ret = job_get_env(sp, &context.job);
	if (ret < 0)
		goto fail;

	if (context.args->container_name != NULL) {
		ret = xasprintf(&container_name, "pyxis_%u_%s", context.job.jobid, context.args->container_name);
		if (ret < 0)
			goto fail;

		ret = enroot_container_get(container_name, &pid);
		if (ret < 0) {
			slurm_error("pyxis: couldn't get list of containers");
			goto fail;
		}

		if (pid > 0) {
			slurm_spank_log("pyxis: reusing existing container PID");
			context.shm->pid = pid;
			context.container.reuse_pid = true;
			context.container.reuse_rootfs = true;
		} else if (pid == 0) {
			slurm_spank_log("pyxis: reusing existing container filesystem");
			context.container.reuse_rootfs = true;
		} else if (context.args->image == NULL) {
			slurm_spank_log("pyxis: error: a container with name \"%s\" does not exist, and --container-image is not set",
					context.args->container_name);
			goto fail;
		}
		context.container.name = container_name;
		container_name = NULL;
	} else {
		ret = xasprintf(&context.container.name, "pyxis_%u.%u", context.job.jobid, context.job.stepid);
		if (ret < 0)
			goto fail;
		context.container.temporary = true;
	}

	if (context.args->container_save != NULL)
		context.container.save_path = strdup(context.args->container_save);

	rv = 0;

fail:
	/*
	 * Errors from user_init() are not propagated back to srun. Rather than fail here and have srun
	 * report rc=0 (success), we return 0 here and throw the error in task_init()
	 * instead, which will properly propagate the error back to srun.
	 *
	 * See https://bugs.schedmd.com/show_bug.cgi?id=7573 for more details.
	 */
	if (rv != 0)
		slurm_debug("pyxis: user_init() failed with rc=%d; postponing error for now, will report later", rv);
	context.user_init_rv = rv;
	free(container_name);

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
 * Some other variables are handled with an enroot hook, but these must be
 * initialized for each task, not once per node like the container create.
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

static int enroot_start_once(struct container *container, struct shared_memory *shm)
{
	int ret;
	int rv = -1;

	pthread_mutex_lock(&shm->mutex);

	shm->init_tasks += 1;

	/* The first task will create and/or start the enroot container */
	if (shm->init_tasks == 1) {
		if (!container->reuse_pid) {
			if (!container->reuse_rootfs) {
				ret = enroot_container_create();
				if (ret < 0)
					goto fail;
			}

			shm->pid = enroot_container_start();
		}
	}

	if (shm->pid < 0)
		goto fail;

	rv = 0;

fail:
	pthread_mutex_unlock(&shm->mutex);

	return (rv);
}

static int enroot_stop_once(struct container *container, struct shared_memory *shm)
{
	int ret;
	int rv = -1;

	pthread_mutex_lock(&shm->mutex);

	shm->started_tasks += 1;

	/* Last task to start can stop the container process. */
	if (context.shm->started_tasks == context.job.local_task_count) {
		if (!container->reuse_pid) {
			ret = enroot_container_stop(shm->pid);
			if (ret < 0)
				goto fail;
		}
		shm->pid = -1;
	}

	rv = 0;

fail:
	pthread_mutex_unlock(&shm->mutex);

	return (rv);
}

int slurm_spank_task_init(spank_t sp, int ac, char **av)
{
	int ret;
	int rv = -1;

	if (!context.enabled)
		return (0);

	if (context.user_init_rv != 0)
		return (context.user_init_rv);

	/* reload the job's environment in this context, to get PMIx variables */
	ret = job_get_env(sp, &context.job);
	if (ret < 0)
		goto fail;

	ret = enroot_start_once(&context.container, context.shm);
	if (ret < 0) {
		slurm_error("pyxis: couldn't start container");
		slurm_error("pyxis: if the image has an unusual entrypoint, try using --no-container-entrypoint");
		goto fail;
	}

	ret = container_get_fds(context.shm->pid, &context.container);
	if (ret < 0) {
		slurm_error("pyxis: couldn't get container attributes");
		goto fail;
	}

	ret = spank_import_container_env(sp, context.shm->pid);
	if (ret < 0) {
		slurm_error("pyxis: couldn't read container environment");
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

	if (context.container.cgroupns_fd >= 0) {
		ret = setns(context.container.cgroupns_fd, CLONE_NEWCGROUP);
		if (ret < 0) {
			slurm_error("pyxis: couldn't join cgroup namespace: %s", strerror(errno));
			goto fail;
		}
	}

	ret = setns(context.container.mntns_fd, CLONE_NEWNS);
	if (ret < 0) {
		slurm_error("pyxis: couldn't join mount namespace: %s", strerror(errno));
		goto fail;
	}

	/* No need to chdir(root) + chroot(".") since enroot does a pivot_root. */
	if (context.args->workdir != NULL) {
		ret = chdir(context.args->workdir);
		if (ret < 0) {
			slurm_error("pyxis: couldn't chdir to %s: %s", context.args->workdir, strerror(errno));
			goto fail;
		}
	} else {
		ret = fchdir(context.container.cwd_fd);
		if (ret < 0) {
			slurm_error("pyxis: couldn't chdir to container cwd: %s", strerror(errno));
			goto fail;
		}
	}

	if (pyxis_remap_root()) {
		ret = seccomp_set_filter();
		if (ret < 0) {
			slurm_error("pyxis: seccomp filter failed: %s", strerror(errno));
			goto fail;
		}
	}

	ret = enroot_stop_once(&context.container, context.shm);
	if (ret < 0)
		goto fail;

	rv = 0;

fail:
	return (rv);
}

static int enroot_container_export(void)
{
	int ret;
	char path[PATH_MAX];

	if (context.container.save_path[0] == '/') {
		if (memccpy(path, context.container.save_path, '\0', sizeof(path)) == NULL)
			return (-1);
	} else {
		if (context.job.cwd[0] == '\0') {
			slurm_error("pyxis: container export: relative path used, but job's cwd is unset");
			return (-1);
		}

		ret = snprintf(path, sizeof(path), "%s/%s", context.job.cwd, context.container.save_path);
		if (ret < 0 || ret >= sizeof(path))
			return (-1);
	}

	slurm_spank_log("pyxis: saving container filesystem at %s", path);
	ret = enroot_exec_wait(context.job.uid, context.job.gid,
			       (char *const[]){ "enroot", "export", "-f", "-o", path, context.container.name, NULL });
	if (ret < 0) {
		enroot_print_last_log();
		return (-1);
	}

	return (0);
}

int pyxis_slurmstepd_exit(spank_t sp, int ac, char **av)
{
	int ret;
	int rv = 0;

	if (context.container.save_path != NULL) {
		slurm_spank_log("pyxis: saving container filesystem ...");

		ret = enroot_container_export();
		if (ret < 0) {
			slurm_error("pyxis: failed to save container filesystem");
			rv = -1;
		}
	}

	if (context.container.temporary) {
		slurm_spank_log("pyxis: removing container filesystem ...");

		ret = enroot_exec_wait(context.job.uid, context.job.gid,
				(char *const[]){ "enroot", "remove", "-f", context.container.name, NULL });
		if (ret < 0) {
			slurm_error("pyxis: failed to remove container filesystem");
			enroot_print_last_log();
			rv = -1;
		}
	}

	free(context.container.name);
	free(context.container.save_path);

	if (context.job.environ != NULL) {
		for (int i = 0; context.job.environ[i] != NULL; ++i)
			free(context.job.environ[i]);
		free(context.job.environ);
	}

	xclose(context.container.userns_fd);
	xclose(context.container.mntns_fd);
	xclose(context.container.cgroupns_fd);
	xclose(context.container.cwd_fd);
	xclose(context.log_fd);

	ret = shm_destroy(context.shm);
	if (ret < 0) {
		slurm_error("pyxis: couldn't destroy shared memory: %s", strerror(errno));
		rv = -1;
	}

	pyxis_args_free();

	memset(&context, 0, sizeof(context));

	return (rv);
}
