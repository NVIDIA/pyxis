/*
 * Copyright (c) 2019-2025, NVIDIA CORPORATION. All rights reserved.
 */

#include <linux/limits.h>

#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <sys/wait.h>

#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <glob.h>
#include <inttypes.h>
#include <paths.h>
#include <pthread.h>
#include <sched.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <slurm/spank.h>

#include "pyxis_slurmstepd.h"
#include "common.h"
#include "config.h"
#include "args.h"
#include "seccomp_filter.h"
#include "enroot.h"
#include "importer.h"

static bool pyxis_debug = false;

#define PYXIS_DEBUG_LOG(fmt, ...) do { \
	if (pyxis_debug) \
		slurm_spank_log("pyxis: debug: " fmt, ##__VA_ARGS__); \
} while (0)

static bool env_bool(spank_t sp, const char *name, bool def)
{
	char buf[64];
	const char *val = NULL;

	if (sp != NULL && spank_context() == S_CTX_REMOTE) {
		spank_err_t rc = spank_getenv(sp, name, buf, sizeof(buf));
		if (rc == ESPANK_SUCCESS)
			val = buf;
	}
	if (val == NULL)
		val = getenv(name);
	if (val == NULL)
		return def;

	int ret = parse_bool(val);
	if (ret >= 0)
		return ret != 0;

	char *end = NULL;
	long n = strtol(val, &end, 10);
	if (end != NULL && end != val && *end == '\0')
		return n != 0;

	return def;
}

struct container {
	char *name;
	char *squashfs_path;
	char *save_path;
	bool reuse_rootfs;
	bool reuse_ns;
	bool temporary_rootfs;
	bool cache_mode;
	char *cache_data_path_root;
	char *cache_data_path;
	int cache_lock_fd;
	bool use_enroot_import;
	bool use_enroot_load;
	bool use_importer;
	int userns_fd;
	int mntns_fd;
	int cgroupns_fd;
	int cwd_fd;
};

struct job_info {
	uid_t uid;
	gid_t gid;
	bool privileged;
	uint32_t jobid;
	uint32_t stepid;
	uint32_t local_task_count;
	uint32_t total_task_count;
	char **environ;
	char cwd[PATH_MAX];
};

struct shared_memory {
	pthread_mutex_t mutex;
	atomic_uint init_tasks;
	atomic_uint started_tasks;
	atomic_uint completed_tasks;
	pid_t pid;
	pid_t ns_pid;
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

static double timespec_diff_ms(const struct timespec *start, const struct timespec *end)
{
	return (end->tv_sec - start->tv_sec) * 1000.0 + (end->tv_nsec - start->tv_nsec) / 1000000.0;
}

/* ---- container-cache helpers ---- */

#define PYXIS_CACHE_CONTAINER_BASENAME_PREFIX "cache_u"
#define PYXIS_CACHE_CONTAINER_PREFIX "pyxis_cache_"
#define PYXIS_CACHE_LOCKFILE ".pyxis_cache_lock"

static uint64_t fnv1a64_update(uint64_t h, const void *data, size_t len)
{
	const unsigned char *p = (const unsigned char *)data;
	for (size_t i = 0; i < len; ++i) {
		h ^= (uint64_t)p[i];
		h *= 1099511628211ULL;
	}
	return h;
}

static bool image_looks_like_path(const char *image)
{
	/* Matches the logic used later in user_init() when detecting squashfs paths. */
	return image != NULL && strspn(image, "./") > 0;
}

static int container_cache_build_basename(char **out, const char *image, uid_t uid)
{
	struct stat st;
	uint64_t h = 1469598103934665603ULL;
	char buf[128];
	int ret;

	if (out == NULL || image == NULL)
		return (-1);

	h = fnv1a64_update(h, image, strlen(image));

	if (image_looks_like_path(image) && stat(image, &st) == 0) {
		ret = snprintf(buf, sizeof(buf), "|%lld|%lld",
			       (long long)st.st_mtime, (long long)st.st_size);
		if (ret > 0 && (size_t)ret < sizeof(buf))
			h = fnv1a64_update(h, buf, (size_t)ret);
	}

	ret = xasprintf(out, "%s%u_%016" PRIx64, PYXIS_CACHE_CONTAINER_BASENAME_PREFIX, (unsigned)uid, h);
	if (ret < 0)
		return (-1);

	return (0);
}

static char *cache_data_path_for_uid(const char *root, uid_t uid)
{
	char *out = NULL;
	int ret;

	if (root == NULL)
		return NULL;

	ret = xasprintf(&out, "%s/%u", root, (unsigned)uid);
	if (ret < 0)
		return NULL;
	return out;
}

static int mkdir_p_owned(const char *path, uid_t uid, gid_t gid, mode_t mode)
{
	char tmp[PATH_MAX];
	size_t len;
	struct stat st;
	int ret;

	if (path == NULL)
		return (-1);

	len = strnlen(path, sizeof(tmp));
	if (len == 0 || len >= sizeof(tmp))
		return (-1);
	memcpy(tmp, path, len + 1);

	for (char *p = tmp + 1; *p; ++p) {
		if (*p != '/')
			continue;
		*p = '\0';
		(void)mkdir(tmp, 0755);
		*p = '/';
	}

	ret = mkdir(tmp, mode);
	if (ret < 0 && errno != EEXIST)
		return (-1);

	if (stat(tmp, &st) < 0)
		return (-1);
	if (!S_ISDIR(st.st_mode)) {
		errno = ENOTDIR;
		return (-1);
	}

	if (st.st_uid != uid || st.st_gid != gid) {
		if (chown(tmp, uid, gid) < 0) {
			slurm_error("pyxis: couldn't chown %s: %s", tmp, strerror(errno));
			return (-1);
		}
	}

	if ((st.st_mode & 07777) != mode) {
		if (chmod(tmp, mode) < 0) {
			slurm_error("pyxis: couldn't chmod %s: %s", tmp, strerror(errno));
			return (-1);
		}
	}
	return (0);
}

static int touch_path(const char *path)
{
	struct timespec ts[2];

	if (path == NULL)
		return (-1);

	clock_gettime(CLOCK_REALTIME, &ts[0]);
	ts[1] = ts[0];
	return utimensat(AT_FDCWD, path, ts, 0);
}

static int cache_lock_shared(const char *container_dir, int *out_fd)
{
	char lock_path[PATH_MAX];
	int fd;
	int ret;

	if (container_dir == NULL || out_fd == NULL)
		return (-1);

	ret = snprintf(lock_path, sizeof(lock_path), "%s/%s", container_dir, PYXIS_CACHE_LOCKFILE);
	if (ret < 0 || ret >= (int)sizeof(lock_path))
		return (-1);

	fd = open(lock_path, O_CREAT | O_RDWR | O_CLOEXEC, 0644);
	if (fd < 0)
		return (-1);

	if (flock(fd, LOCK_SH) < 0) {
		close(fd);
		return (-1);
	}

	*out_fd = fd;
	return (0);
}

static int cache_gc_lock_fd(const char *data_path_root)
{
	char path[PATH_MAX];
	int fd;
	int ret;

	if (data_path_root == NULL || data_path_root[0] == '\0')
		return (-1);

	ret = snprintf(path, sizeof(path), "%s/pyxis-container-cache-gc.lock", data_path_root);
	if (ret < 0 || ret >= (int)sizeof(path))
		return (-1);

	fd = open(path, O_CREAT | O_RDWR | O_CLOEXEC, 0644);
	if (fd < 0)
		return (-1);
	return fd;
}

static int rm_rf_cb(const char *fpath, const struct stat *sb, int typeflag, struct FTW *ftwbuf)
{
	(void)sb;
	(void)ftwbuf;
	if (typeflag == FTW_DP || typeflag == FTW_D)
		(void)rmdir(fpath);
	else
		(void)unlink(fpath);
	return 0;
}

static int rm_rf(const char *path)
{
	if (path == NULL)
		return (-1);
	return nftw(path, rm_rf_cb, 64, FTW_DEPTH | FTW_PHYS);
}

struct cache_candidate {
	char *path;
	time_t mtime;
};

static int cache_candidate_cmp(const void *a, const void *b)
{
	const struct cache_candidate *ca = (const struct cache_candidate *)a;
	const struct cache_candidate *cb = (const struct cache_candidate *)b;
	if (ca->mtime < cb->mtime)
		return -1;
	if (ca->mtime > cb->mtime)
		return 1;
	return 0;
}

static int cache_collect_candidates(const char *user_dirs_glob, struct cache_candidate **out, size_t *out_len)
{
	glob_t g_users = {0}, g_cache = {0};
	struct cache_candidate *list = NULL;
	size_t list_len = 0;
	int ret;

	if (out == NULL || out_len == NULL || user_dirs_glob == NULL)
		return (-1);

	ret = glob(user_dirs_glob, 0, NULL, &g_users);
	if (ret != 0) {
		globfree(&g_users);
		*out = NULL;
		*out_len = 0;
		return (0);
	}

	for (size_t i = 0; i < g_users.gl_pathc; ++i) {
		char pattern[PATH_MAX];
		int n = snprintf(pattern, sizeof(pattern), "%s/%s*", g_users.gl_pathv[i], PYXIS_CACHE_CONTAINER_PREFIX);
		if (n < 0 || n >= (int)sizeof(pattern))
			continue;

		memset(&g_cache, 0, sizeof(g_cache));
		if (glob(pattern, 0, NULL, &g_cache) != 0) {
			globfree(&g_cache);
			continue;
		}

		for (size_t j = 0; j < g_cache.gl_pathc; ++j) {
			struct stat st;
			if (lstat(g_cache.gl_pathv[j], &st) < 0)
				continue;
			if (!S_ISDIR(st.st_mode))
				continue;

			struct cache_candidate *tmp = realloc(list, (list_len + 1) * sizeof(*list));
			if (tmp == NULL)
				continue;
			list = tmp;
			list[list_len].path = strdup(g_cache.gl_pathv[j]);
			list[list_len].mtime = st.st_mtime;
			list_len++;
		}
		globfree(&g_cache);
	}

	globfree(&g_users);
	*out = list;
	*out_len = list_len;
	return (0);
}

static void cache_free_candidates(struct cache_candidate *list, size_t len)
{
	if (list == NULL)
		return;
	for (size_t i = 0; i < len; ++i)
		free(list[i].path);
	free(list);
}

static int cache_fs_used_percent(const char *path, int *out_used_percent)
{
	struct statvfs vfs;
	unsigned long long total, avail, used;
	int used_pct;

	if (path == NULL || out_used_percent == NULL)
		return (-1);

	if (statvfs(path, &vfs) < 0)
		return (-1);

	total = (unsigned long long)vfs.f_blocks * (unsigned long long)vfs.f_frsize;
	avail = (unsigned long long)vfs.f_bavail * (unsigned long long)vfs.f_frsize;
	used = (total > avail) ? (total - avail) : 0;

	used_pct = (total == 0) ? 0 : (int)((used * 100ULL) / total);
	*out_used_percent = used_pct;
	return (0);
}

static int cache_gc_if_needed(const char *data_path_root, int high_water, int low_water)
{
	int used_pct;
	int ret;
	int lock_fd = -1;
	struct cache_candidate *cands = NULL;
	size_t cands_len = 0;
	char *user_glob = NULL;

	if (data_path_root == NULL)
		return (0);

	ret = cache_fs_used_percent(data_path_root, &used_pct);
	if (ret < 0)
		return (0);

	if (used_pct < high_water)
		return (0);

	/* Serialize GC for the whole cache root. */
	lock_fd = cache_gc_lock_fd(data_path_root);
	if (lock_fd < 0)
		return (0);
	if (flock(lock_fd, LOCK_EX) < 0) {
		close(lock_fd);
		return (0);
	}

	ret = xasprintf(&user_glob, "%s/*", data_path_root);
	if (ret < 0 || user_glob == NULL)
		goto out;

	ret = cache_collect_candidates(user_glob, &cands, &cands_len);
	if (ret < 0)
		goto out;

	qsort(cands, cands_len, sizeof(*cands), cache_candidate_cmp);

	for (size_t i = 0; i < cands_len; ++i) {
		char lock_path[PATH_MAX];
		int fd;
		int n;

		ret = cache_fs_used_percent(data_path_root, &used_pct);
		if (ret == 0 && used_pct < low_water)
			break;

		n = snprintf(lock_path, sizeof(lock_path), "%s/%s", cands[i].path, PYXIS_CACHE_LOCKFILE);
		if (n < 0 || n >= (int)sizeof(lock_path))
			continue;

		fd = open(lock_path, O_CREAT | O_RDWR | O_CLOEXEC, 0644);
		if (fd < 0)
			continue;

		if (flock(fd, LOCK_EX | LOCK_NB) < 0) {
			close(fd);
			continue; /* in use */
		}

		slurm_info("pyxis: container-cache GC: evicting %s", cands[i].path);
		(void)rm_rf(cands[i].path);
		close(fd);
	}

out:
	cache_free_candidates(cands, cands_len);
	free(user_glob);
	flock(lock_fd, LOCK_UN);
	close(lock_fd);
	return (0);
}

/* ---- end container-cache helpers ---- */

static struct plugin_context context = {
	.enabled = false,
	.log_fd = -1,
	.config = { .runtime_path = { 0 } },
	.args = NULL,
	.job = {
		.uid = -1, .gid = -1, .privileged = false,
		.jobid = 0, .stepid = 0,
		.local_task_count = 0, .total_task_count = 0,
		.environ = NULL, .cwd = { 0 }
	},
	.container = {
		.name = NULL, .squashfs_path = NULL, .save_path = NULL,
		.reuse_rootfs = false, .reuse_ns = false, .temporary_rootfs = false,
		.cache_mode = false, .cache_data_path_root = NULL, .cache_data_path = NULL, .cache_lock_fd = -1,
		.use_enroot_import = false, .use_enroot_load = false,
		.use_importer = false,
		.userns_fd = -1, .mntns_fd = -1, .cgroupns_fd = -1, .cwd_fd = -1
	},
	.user_init_rv = 0,
};

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
	char allow_superuser[] = "false";
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

	rc = spank_get_item(sp, S_JOB_TOTAL_TASK_COUNT, &job->total_task_count);
	if (rc != ESPANK_SUCCESS) {
		slurm_error("pyxis: couldn't get job total task count: %s", spank_strerror(rc));
		goto fail;
	}

	/* This should probably be added to the API as a spank_item */
	rc = spank_getenv(sp, "PWD", job->cwd, sizeof(job->cwd));
	if (rc != ESPANK_SUCCESS)
		slurm_info("pyxis: couldn't get job cwd path: %s", spank_strerror(rc));

	rc = spank_getenv(sp, "ENROOT_ALLOW_SUPERUSER", allow_superuser, sizeof(allow_superuser));
	if (rc == ESPANK_SUCCESS) {
		if (job->uid == 0 && strcasecmp(allow_superuser, "no") != 0 && strcasecmp(allow_superuser, "false") != 0 &&
		    strcasecmp(allow_superuser, "n") != 0 && strcasecmp(allow_superuser, "f") != 0) {
			job->privileged = true;
		}
	}

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

	/* Check environment variables for default values after command-line processing */
	pyxis_args_check_environment_variables(sp);
	pyxis_debug = env_bool(sp, "PYXIS_DEBUG", false);

	if (!pyxis_args_enabled())
		return (0);

	context.enabled = true;

	ret = job_get_info(sp, &context.job);
	if (ret < 0)
		return (-1);

	ret = enroot_create_user_runtime_dir();
	if (ret < 0)
		return (-1);

	/* Pre-compute cache paths and run GC early (this hook runs with elevated privileges). */
	if (context.args->container_cache == 1) {
		context.container.cache_mode = true;

		if (context.config.container_cache_data_path[0] == '\0') {
			slurm_error("pyxis: error: --container-cache requires container_cache_data_path to be configured");
			return (-1);
		}

		if (context.container.cache_data_path_root == NULL) {
			char *root = NULL;

			root = strdup(context.config.container_cache_data_path);
			if (root != NULL) {
				context.container.cache_data_path_root = root;
				context.container.cache_data_path = cache_data_path_for_uid(root, context.job.uid);
				if (context.container.cache_data_path != NULL)
					(void)mkdir_p_owned(context.container.cache_data_path, context.job.uid, context.job.gid, 0700);
			} else
				return (-1);
		}

		/*
		 * Only run GC when we're about to create a new cached rootfs.
		 * (Avoid evicting a rootfs this job will just reuse.)
		 */
		if (context.args->image != NULL &&
		    context.container.cache_data_path_root != NULL &&
		    context.container.cache_data_path != NULL) {
			char *cache_basename = NULL;
			char *container_name = NULL;
			char dir_path[PATH_MAX];
			struct stat st;
			int n;

			ret = container_cache_build_basename(&cache_basename, context.args->image, context.job.uid);
			if (ret == 0) {
				ret = xasprintf(&container_name, "pyxis_%s", cache_basename);
			}
			if (ret >= 0 && container_name != NULL) {
				n = snprintf(dir_path, sizeof(dir_path), "%s/%s", context.container.cache_data_path, container_name);
				if (n > 0 && n < (int)sizeof(dir_path) &&
				    (stat(dir_path, &st) < 0 || !S_ISDIR(st.st_mode))) {
					(void)cache_gc_if_needed(context.container.cache_data_path_root,
								 context.config.container_cache_gc_high,
								 context.config.container_cache_gc_low);
				}
			}

			free(container_name);
			free(cache_basename);
		}
	}

	return (0);
}

static int enroot_new_log(void)
{
	xclose(context.log_fd);

	/* We can use CLOEXEC here since we dup2(2) this file descriptor when needed. */
	context.log_fd = pyxis_memfd_create("enroot-log", MFD_CLOEXEC);
	if (context.log_fd < 0)
		slurm_info("pyxis: couldn't create in-memory log file: %s", strerror(errno));

	return (context.log_fd);
}

/* We do not want to inherit any environment variable from slurmstepd, except PATH */
static int slurm_clear_env(void)
{
	int rv = -1;
	const char *p;
	char *saved_path = NULL;

	/* It's unclear if the pointer returned by getenv(3) will always persist after clearenv(3), so make a copy. */
	p = getenv("PATH");
	if (p != NULL) {
		saved_path = strdup(p);
		if (saved_path == NULL)
			goto fail;
	}

	if (clearenv() != 0)
		goto fail;

	if (saved_path != NULL) {
		if (setenv("PATH", saved_path, 1) < 0)
			goto fail;
	}

	rv = 0;

fail:
	free(saved_path);
	return (rv);
}

/*
 * List of environment variables that should not be passed from the Slurm job to enroot.
 */

#define PYXIS_ENV_ENTRY(s) { s, sizeof(s) - 1 }
static const struct {
	const char *name;
	size_t len;
} enroot_deny_env[] = {
	PYXIS_ENV_ENTRY("PATH="),
	PYXIS_ENV_ENTRY("LD_LIBRARY_PATH="),
	PYXIS_ENV_ENTRY("LD_PRELOAD="),
	PYXIS_ENV_ENTRY("SLURM_PROCID="),
	PYXIS_ENV_ENTRY("SLURM_LOCALID="),
	PYXIS_ENV_ENTRY("SLURM_TASK_PID="),
	PYXIS_ENV_ENTRY("PMIX_RANK="),
	PYXIS_ENV_ENTRY("PMI_FD="),
	PYXIS_ENV_ENTRY("ENROOT_LIBRARY_PATH="),
	PYXIS_ENV_ENTRY("ENROOT_SYSCONF_PATH="),
	PYXIS_ENV_ENTRY("ENROOT_RUNTIME_PATH="),
	PYXIS_ENV_ENTRY("ENROOT_CACHE_PATH="),
	PYXIS_ENV_ENTRY("ENROOT_DATA_PATH="),
	PYXIS_ENV_ENTRY("ENROOT_TEMP_PATH="),
	PYXIS_ENV_ENTRY("ENROOT_ZSTD_OPTIONS="),
	PYXIS_ENV_ENTRY("ENROOT_TRANSFER_RETRIES="),
	PYXIS_ENV_ENTRY("ENROOT_CONNECT_TIMEOUT="),
	PYXIS_ENV_ENTRY("ENROOT_MAX_CONNECTIONS="),
	PYXIS_ENV_ENTRY("ENROOT_ALLOW_HTTP="),
	{ NULL, 0 }
};
#undef PYXIS_ENV_ENTRY

static int try_import_env(char *string)
{
	for (int i = 0; enroot_deny_env[i].name != NULL; ++i) {
		if (strncmp(string, enroot_deny_env[i].name, enroot_deny_env[i].len) == 0)
			return (0);
	}

	if (putenv(string) < 0)
		return (-1);

	return (0);
}

static int enroot_import_job_env(char **env)
{
	if (env == NULL)
		return (-1);

	/* Import all allowed environment variables from the job */
	for (int i = 0; env[i]; ++i) {
		if (try_import_env(env[i]) < 0)
			return (-1);
	}

	return (0);
}

static int enroot_set_env(void)
{
	if (slurm_clear_env() < 0)
		return (-1);

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

	if (context.args->remap_root == 0) {
		if (setenv("ENROOT_REMAP_ROOT", "n", 1) < 0)
			return (-1);
	} else if (context.args->remap_root == 1) {
		if (setenv("ENROOT_REMAP_ROOT", "y", 1) < 0)
			return (-1);
	} else {
		/* If remap_root was not set by the user, we rely on the setting specified in the enroot config. */
	}

	if (context.args->writable == 0) {
		if (setenv("ENROOT_ROOTFS_WRITABLE", "n", 1) < 0)
			return (-1);
	} else if (context.args->writable == 1) {
		if (setenv("ENROOT_ROOTFS_WRITABLE", "y", 1) < 0)
			return (-1);
	} else {
		/* If writable/readonly was not set by the user, we rely on the setting specified in the enroot config. */
	}

	/* container-cache may override ENROOT_DATA_PATH to a persistent, node-local directory. */
	if (context.container.cache_mode && context.container.cache_data_path != NULL) {
		if (setenv("ENROOT_DATA_PATH", context.container.cache_data_path, 1) < 0)
			return (-1);
	}

	if (setenv("PYXIS_RUNTIME_PATH", context.config.runtime_path, 1) < 0)
		return (-1);

	if (setenv("PYXIS_VERSION", PYXIS_VERSION, 1) < 0)
		return (-1);

	return (0);
}

static pid_t enroot_exec_ctx(char *const argv[])
{
	return enroot_exec(context.job.uid, context.job.gid, enroot_new_log(), enroot_set_env, argv);
}

static int enroot_exec_wait_ctx(char *const argv[])
{
	return enroot_exec_wait(context.job.uid, context.job.gid, enroot_new_log(), enroot_set_env, argv);
}

static FILE *enroot_exec_output_ctx(char *const argv[])
{
	return enroot_exec_output(context.job.uid, context.job.gid, enroot_set_env, argv);
}

static void enroot_print_log_ctx(bool error)
{
	if (context.log_fd >= 0)
		memfd_print_log(&context.log_fd, error, "enroot");
}

/*
 * Returns -1 if an error occurs.
 * Returns 0 and set *pid==-1 if container doesn't exist.
 * Returns 0 and set *pid==0 if container exists but is not running.
 * Returns 0 and set *pid>0 (the container PID) if the container exists and is running.
 */
static int enroot_container_get(const char *name, pid_t *pid)
{
	FILE *fp;
	char *line;
	char *ctr_name, *ctr_pid, *saveptr;
	unsigned long n;
	int rv = -1;

	if (name == NULL || strlen(name) == 0 || pid == NULL)
		return (-1);

	*pid = -1;

	fp = enroot_exec_output_ctx((char *const[]){ "enroot", "list", "-f", NULL });
	if (fp == NULL) {
		slurm_error("pyxis: couldn't get list of existing container filesystems");
		return (-1);
	}

	/* Skip headers line */
	line = get_line_from_file(fp);
	if (line == NULL) {
		slurm_error("pyxis: \"enroot list -f\" did not produce any usable output");
		goto fail;
	}
	free(line);
	line = NULL;

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
	if (fp) fclose(fp);
	return (rv);
}

static int read_proc_environ(pid_t pid, char **result, size_t *size)
{
	int ret;
	char path[PATH_MAX];
	int fd = -1;
	char *buf = NULL;
	size_t len = 0, capacity = 1024;
	ssize_t n;
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

	while ((n = read(fd, buf + len, capacity - len)) > 0) {
		len += n;

		if (capacity - len == 0) {
			/* Grow buffer. */
			capacity *= 2;

			new_buf = realloc(buf, capacity + 1);
			if (new_buf == NULL)
				goto fail;

			buf = new_buf;
		}
	}

	if (n < 0)
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

static const char *container_deny_env[] = {
	"LANG",
	"LANGUAGE",
	"LC_ALL",
	NULL
};

static int spank_import_container_env(spank_t sp, pid_t pid)
{
	int ret;
	char *proc_environ = NULL;
	size_t size;
	spank_err_t rc;
	int overwrite;
	int rv = -1;

	/* First, remove unwanted locale environment variables from the job */
	for (int i = 0; container_deny_env[i] != NULL; ++i) {
		/* Check if the user explicitly requested this environment variable to be preserved */
		if (array_contains(context.args->env_vars, context.args->env_vars_len, container_deny_env[i]))
			continue;

		rc = spank_unsetenv(sp, container_deny_env[i]);
		if (rc != ESPANK_SUCCESS) {
			slurm_error("pyxis: failed to unset %s: %s", container_deny_env[i], spank_strerror(rc));
			goto fail;
		}
	}

	ret = read_proc_environ(pid, &proc_environ, &size);
	if (ret < 0) {
		slurm_error("pyxis: couldn't read /proc/%d/environ", pid);
		goto fail;
	}

	for (size_t i = 0; i < size;) {
		char *key, *value;

		value = proc_environ + i;
		key = strsep(&value, "=");

		overwrite = 1;
		if (array_contains(context.args->env_vars, context.args->env_vars_len, key))
			overwrite = 0;

		rc = spank_setenv(sp, key, value, overwrite);
		if (rc != ESPANK_SUCCESS && rc != ESPANK_ENV_EXISTS) {
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
	char *enroot_uri = NULL;
	struct timespec start_time, end_time;
	int rv = -1;

	if (context.container.use_enroot_import || context.container.use_enroot_load || context.container.use_importer) {
		if (strncmp("docker://", context.args->image, sizeof("docker://") - 1) == 0 ||
		    strncmp("dockerd://", context.args->image, sizeof("dockerd://") - 1) == 0) {
			enroot_uri = strdup(context.args->image);
			if (enroot_uri == NULL)
				goto fail;
		} else {
			/* Assume `image` is an enroot URI for a docker image. */
			ret = xasprintf(&enroot_uri, "docker://%s", context.args->image);
			if (ret < 0)
				goto fail;
		}

		/*
		 * Be more verbose if there is a single task in the job (it might be interactive),
		 * or if we are executing the batch step (S_JOB_TOTAL_TASK_COUNT=0)
		 */
		if (context.job.total_task_count == 0 || context.job.total_task_count == 1)
			slurm_spank_log("pyxis: importing docker image: %s", context.args->image);
	}

	clock_gettime(CLOCK_MONOTONIC, &start_time);

	if (context.container.use_enroot_load) {
		ret = enroot_exec_wait_ctx((char *const[]){ "enroot", "load", "--name", context.container.name, enroot_uri, NULL });
		if (ret < 0) {
			slurm_error("pyxis: failed to import docker image");
			enroot_print_log_ctx(true);
			goto fail;
		}
		slurm_spank_log("pyxis: imported docker image: %s", context.args->image);
	} else {
		if (context.container.use_enroot_import) {
			ret = enroot_exec_wait_ctx((char *const[]){ "enroot", "import", "--output", context.container.squashfs_path, enroot_uri, NULL });
			if (ret < 0) {
				slurm_error("pyxis: failed to import docker image");
				enroot_print_log_ctx(true);
				goto fail;
			}
			slurm_spank_log("pyxis: imported docker image: %s", context.args->image);
		} else if (context.container.use_importer) {
			/* Use external importer to get squashfs file */
			ret = importer_exec_get(context.config.importer_path, context.job.uid, context.job.gid,
						enroot_set_env, enroot_uri, &context.container.squashfs_path);
			if (ret < 0) {
				slurm_error("pyxis: failed to import docker image with importer: %s", context.config.importer_path);
				goto fail;
			}
			slurm_spank_log("pyxis: imported docker image: %s", context.args->image);
		}

		if (context.container.squashfs_path != NULL) {
			slurm_info("pyxis: creating container filesystem: %s", context.container.name);

			ret = enroot_exec_wait_ctx((char *const[]){ "enroot", "create", "--name", context.container.name, context.container.squashfs_path, NULL });
			if (ret < 0) {
				slurm_error("pyxis: failed to create container filesystem");
				enroot_print_log_ctx(true);
				goto fail;
			}
		}
	}

	clock_gettime(CLOCK_MONOTONIC, &end_time);
	slurm_info("pyxis: completed container setup in %.0f ms", timespec_diff_ms(&start_time, &end_time));

	/* Mark the cached rootfs as recently used and hold a shared lock for the job lifetime */
	if (context.container.cache_mode && context.container.cache_data_path != NULL && context.container.cache_lock_fd < 0) {
		char dir_path[PATH_MAX];
		ret = snprintf(dir_path, sizeof(dir_path), "%s/%s", context.container.cache_data_path, context.container.name);
		if (ret > 0 && ret < (int)sizeof(dir_path)) {
			if (touch_path(dir_path) < 0)
				PYXIS_DEBUG_LOG("container-cache: touch failed: %s", dir_path);
			if (cache_lock_shared(dir_path, &context.container.cache_lock_fd) < 0) {
				PYXIS_DEBUG_LOG("container-cache: lock failed: %s/%s", dir_path, PYXIS_CACHE_LOCKFILE);
			} else {
				PYXIS_DEBUG_LOG("container-cache: locked: %s/%s", dir_path, PYXIS_CACHE_LOCKFILE);
			}
		}
	}

	rv = 0;

fail:
	free(enroot_uri);
	if (context.container.use_enroot_import && context.container.squashfs_path != NULL) {
		ret = unlink(context.container.squashfs_path);
		if (ret < 0)
			slurm_info("pyxis: could not remove squashfs %s: %s", context.container.squashfs_path, strerror(errno));
		free(context.container.squashfs_path);
		context.container.squashfs_path = NULL;
	}

	if (context.container.use_importer) {
		ret = importer_exec_release(context.config.importer_path, context.job.uid, context.job.gid,
					    enroot_set_env);
		if (ret < 0)
			slurm_info("pyxis: could not call importer release");
		free(context.container.squashfs_path);
		context.container.squashfs_path = NULL;
	}

	return (rv);
}

static int container_get_namespaces(pid_t pid, struct container *container)
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

	rv = 0;

fail:
	return (rv);
}

static int container_get_cwd(pid_t pid, struct container *container)
{
	int ret;
	char path[PATH_MAX];

	ret = snprintf(path, sizeof(path), "/proc/%d/cwd", pid);
	if (ret < 0 || ret >= sizeof(path))
		return (-1);

	container->cwd_fd = open(path, O_RDONLY | O_CLOEXEC);
	if (container->cwd_fd < 0) {
		slurm_error("pyxis: couldn't open cwd fd: %s", strerror(errno));
		return (-1);
	}

	return (0);
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

		/* mount entries */
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

	if (context.args->env_vars_len > 0) {
		ret = fprintf(f, "environ() {\n");
		if (ret < 0)
			goto fail;

		for (int i = 0; i < context.args->env_vars_len; ++i) {
			ret = fprintf(f, "\t[ -n \"${%1$s-}\" ] && echo \"%1$s=${%1$s}\" || :\n",
				      context.args->env_vars[i]);
			if (ret < 0)
				goto fail;
		}

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

	ret = snprintf(*path, sizeof(*path), "%s", template);
	if (ret < 0 || ret >= sizeof(*path))
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

	slurm_info("pyxis: starting container: %s", context.container.name);

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
	pid = enroot_exec_ctx((char *const[]){ "enroot", "start", "--conf", conf_file, context.container.name, "sh", "-c",
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
		enroot_print_log_ctx(true);
	else if (pyxis_execute_entrypoint() && context.args->entrypoint_log == 1)
		enroot_print_log_ctx(false);
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

	ret = pthread_mutexattr_setrobust(&mutex_attr, PTHREAD_MUTEX_ROBUST);
	if (ret < 0)
		goto fail;

	ret = pthread_mutex_init(&shm->mutex, &mutex_attr);
	if (ret < 0)
		goto fail;

	shm->init_tasks = 0;
	shm->started_tasks = 0;
	shm->completed_tasks = 0;
	shm->pid = -1;
	shm->ns_pid = -1;

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

	if (pthread_mutex_lock(&shm->mutex) == EOWNERDEAD)
		pthread_mutex_consistent(&shm->mutex);
	pthread_mutex_unlock(&shm->mutex);

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
	spank_err_t rc;
	int spank_argc = 0;
	char **spank_argv = NULL;
	char *container_name = NULL;
	char *cache_basename = NULL;
	pid_t pid;
	int rv = -1;
	enum container_scope name_scope;
	const char *requested_name;
	const char *requested_flags;

	if (!context.enabled)
		return (0);

	context.shm = shm_init();
	if (context.shm == NULL)
		goto fail;

	ret = job_get_env(sp, &context.job);
	if (ret < 0)
		goto fail;

	if (context.job.stepid == SLURM_BATCH_SCRIPT) {
		rc = spank_get_item(sp, S_JOB_ARGV, &spank_argc, &spank_argv);
		if (rc != ESPANK_SUCCESS) {
			slurm_error("pyxis: couldn't get job argv: %s", spank_strerror(rc));
			goto fail;
		}

		if (spank_argc == 0) {
			slurm_error("pyxis: couldn't get sbatch script: argc == 0");
			goto fail;
		}

		/* Mount the sbatch script (from the Slurmd spool dir) inside the container */
		ret = add_mount(spank_argv[0], spank_argv[0],
				"x-create=file,bind,ro,nosuid,nodev,private");
		if (ret < 0) {
			slurm_error("pyxis: couldn't add bind mount for sbatch script");
			goto fail;
		}
	}

	name_scope = context.config.container_scope;
	requested_name = context.args->container_name;
	requested_flags = context.args->container_name_flags;

	if (context.args->container_cache == 1) {
		/* container-cache requires an image to derive a stable cache key */
		if (context.args->image == NULL) {
			slurm_error("pyxis: error: --container-cache requires --container-image");
			goto fail;
		}
		if (context.args->container_save != NULL) {
			slurm_error("pyxis: error: --container-cache is incompatible with --container-save");
			goto fail;
		}
		if (context.args->writable == 1) {
			slurm_error("pyxis: error: --container-cache is incompatible with --container-writable");
			goto fail;
		}
		/* Force read-only containers in cache mode (prevents cross-job contamination) */
		context.args->writable = 0;

		/* Disallow special container-name flags in cache mode */
		if (requested_flags != NULL && strcmp(requested_flags, "auto") != 0) {
			slurm_error("pyxis: error: --container-cache is incompatible with --container-name flags (use plain --container-name or omit it)");
			goto fail;
		}

		/* Compute a stable cache basename for this user+image */
		ret = container_cache_build_basename(&cache_basename, context.args->image, context.job.uid);
		if (ret < 0) {
			slurm_error("pyxis: error: --container-cache: couldn't derive stable name");
			goto fail;
		}
		PYXIS_DEBUG_LOG("container-cache: basename=%s", cache_basename);

		requested_name = cache_basename;
		requested_flags = "auto";
		/* Override job-scoped naming; cached containers must outlive a single job */
		name_scope = SCOPE_GLOBAL;
		context.container.cache_mode = true;
	}

	/* In cache mode, determine a persistent ENROOT_DATA_PATH root and derive a per-user directory. */
	if (context.container.cache_mode &&
	    (context.container.cache_data_path_root == NULL || context.container.cache_data_path == NULL)) {
		char *root = NULL;

		if (context.config.container_cache_data_path[0] == '\0') {
			slurm_error("pyxis: error: --container-cache requires container_cache_data_path to be configured");
			goto fail;
		}

		root = strdup(context.config.container_cache_data_path);

		if (root != NULL) {
			context.container.cache_data_path_root = root;
			context.container.cache_data_path = cache_data_path_for_uid(root, context.job.uid);
			PYXIS_DEBUG_LOG("container-cache: ENROOT_DATA_PATH root=%s", root);
			PYXIS_DEBUG_LOG("container-cache: ENROOT_DATA_PATH=%s",
					context.container.cache_data_path ? context.container.cache_data_path : "(null)");
			if (context.container.cache_data_path != NULL) {
				if (mkdir_p_owned(context.container.cache_data_path, context.job.uid, context.job.gid, 0700) < 0)
					PYXIS_DEBUG_LOG("container-cache: couldn't init ENROOT_DATA_PATH=%s",
							context.container.cache_data_path);
			}
		} else
			goto fail;
	}

	if (requested_name != NULL) {
		if (name_scope == SCOPE_JOB)
			ret = xasprintf(&container_name, "pyxis_%u_%s", context.job.jobid, requested_name);
		else
			ret = xasprintf(&container_name, "pyxis_%s", requested_name);
		if (ret < 0)
			goto fail;

		ret = enroot_container_get(container_name, &pid);
		if (ret < 0) {
			slurm_error("pyxis: couldn't get list of containers");
			goto fail;
		}

		if (requested_flags != NULL && strcmp(requested_flags, "create") == 0 && pid >= 0) {
			slurm_error("pyxis: error: \"create\" flag was passed to --container-name but the container already exists");
			goto fail;
		}
		if (requested_flags != NULL && strcmp(requested_flags, "exec") == 0 && pid <= 0) {
			slurm_error("pyxis: error: \"exec\" flag was passed to --container-name but the container is not running");
			goto fail;
		}
		if (requested_flags != NULL && strcmp(requested_flags, "no_exec") == 0 && pid > 0)
			pid = 0;

		if (pid > 0) {
			slurm_info("pyxis: reusing existing container namespaces");
			context.shm->ns_pid = pid;
			context.container.reuse_ns = true;
			context.container.reuse_rootfs = true;
		} else if (pid == 0) {
			slurm_info("pyxis: reusing existing container filesystem");
			context.container.reuse_rootfs = true;
		} else if (context.args->image == NULL) {
			slurm_error("pyxis: error: a container with name \"%s\" does not exist, and --container-image is not set",
				    container_name);
			goto fail;
		}
		context.container.name = container_name;
		container_name = NULL;
	} else {
		if (name_scope == SCOPE_JOB)
			ret = xasprintf(&context.container.name, "pyxis_%u_%u.%u", context.job.jobid, context.job.jobid, context.job.stepid);
		else
			ret = xasprintf(&context.container.name, "pyxis_%u.%u", context.job.jobid, context.job.stepid);
		if (ret < 0)
			goto fail;
		context.container.temporary_rootfs = true;
	}

	if (!context.container.reuse_rootfs) {
		if (strspn(context.args->image, "./") > 0) {
			/* Assume `image` is a path to a squashfs file. */
			if (strnlen(context.args->image, PATH_MAX) >= PATH_MAX)
				goto fail;

			context.container.squashfs_path = strdup(context.args->image);
			if (context.container.squashfs_path == NULL)
				goto fail;
		} else {
			/* Determine how the image is going to be loaded */
			if (context.config.importer_path[0] != '\0') {
				context.container.use_importer = true;
			} else {
				/* No importer configured, use the builtin enroot import/load path */
				context.container.use_enroot_load = context.config.use_enroot_load &&
					strncmp("dockerd://", context.args->image, sizeof("dockerd://") - 1) != 0;
				context.container.use_enroot_import = !context.container.use_enroot_load;

				if (context.container.use_enroot_import) {
					ret = xasprintf(&context.container.squashfs_path, "%s/%u/%u.%u.squashfs",
							context.config.runtime_path, context.job.uid, context.job.jobid, context.job.stepid);
					if (ret < 0 || ret >= PATH_MAX)
						goto fail;
				}
			}
		}
	}

	if (context.container.reuse_ns && context.args->mounts_len > 0) {
		slurm_spank_log("pyxis: ignoring --container-mounts when attaching to a running container");
		remove_all_mounts();
	}

	if (context.args->container_save != NULL) {
		context.container.save_path = strdup(context.args->container_save);
		if (context.container.save_path == NULL)
			goto fail;
	}

	/* If cache mode is enabled and the rootfs already exists, mark it as recently used and lock it */
	if (context.container.cache_mode && context.container.reuse_rootfs && context.container.cache_data_path != NULL) {
		char dir_path[PATH_MAX];
		ret = snprintf(dir_path, sizeof(dir_path), "%s/%s", context.container.cache_data_path, context.container.name);
		if (ret > 0 && ret < (int)sizeof(dir_path)) {
			if (touch_path(dir_path) < 0)
				PYXIS_DEBUG_LOG("container-cache: touch failed: %s", dir_path);
			if (cache_lock_shared(dir_path, &context.container.cache_lock_fd) < 0) {
				PYXIS_DEBUG_LOG("container-cache: lock failed: %s/%s", dir_path, PYXIS_CACHE_LOCKFILE);
			} else {
				PYXIS_DEBUG_LOG("container-cache: locked: %s/%s", dir_path, PYXIS_CACHE_LOCKFILE);
			}
		}
	}

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
	free(cache_basename);

	return (0);
}

static int spank_copy_env(spank_t sp, const char *from, const char *to, int overwrite)
{
	char buf[256];
	spank_err_t rc;

	rc = spank_getenv(sp, from, buf, sizeof(buf));
	if (rc != ESPANK_SUCCESS) {
		slurm_error("pyxis: failed to get %s: %s", from, spank_strerror(rc));
		return (-1);
	}

	rc = spank_setenv(sp, to, buf, overwrite);
	if (rc != ESPANK_SUCCESS) {
		slurm_error("pyxis: failed to set %s: %s", to, spank_strerror(rc));
		return (-1);
	}

	return (0);
}

static bool pytorch_setup_needed(spank_t sp)
{
	char buf[256];
	spank_err_t rc;

	rc = spank_getenv(sp, "PYTORCH_VERSION", buf, sizeof(buf));
	if (rc != ESPANK_SUCCESS)
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

	if (pthread_mutex_lock(&shm->mutex) == EOWNERDEAD) {
		pthread_mutex_consistent(&shm->mutex);
		shm->pid = -1;
		shm->ns_pid = -1;
		goto fail;
	}

	shm->init_tasks += 1;

	/* The first task will create and/or start the enroot container */
	if (shm->init_tasks == 1) {
		if (!container->reuse_rootfs) {
			ret = enroot_container_create();
			if (ret < 0)
				goto fail;
		}
		shm->pid = enroot_container_start();

		if (!container->reuse_ns)
			shm->ns_pid = shm->pid;
	}

	if (shm->pid < 0 || shm->ns_pid < 0)
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

	/* Last task to start can stop the container process. */
	if (atomic_fetch_add(&shm->started_tasks, 1) == context.job.local_task_count - 1) {
		ret = enroot_container_stop(shm->pid);
		if (ret < 0)
			goto fail;

		shm->pid = -1;
		shm->ns_pid = -1;
	}

	rv = 0;

fail:
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
		if (pyxis_execute_entrypoint())
			slurm_error("pyxis: if the image has an unusual entrypoint, try using --no-container-entrypoint");
		goto fail;
	}

	ret = container_get_namespaces(context.shm->ns_pid, &context.container);
	if (ret < 0) {
		slurm_error("pyxis: couldn't get container namespaces");
		goto fail;
	}

	ret = container_get_cwd(context.shm->pid, &context.container);
	if (ret < 0) {
		slurm_error("pyxis: couldn't get container directory");
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

	if (!context.job.privileged) {
		ret = setns(context.container.userns_fd, CLONE_NEWUSER);
		if (ret < 0) {
			slurm_error("pyxis: couldn't join user namespace: %s", strerror(errno));
			goto fail;
		}
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

	if (!context.job.privileged) {
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
		ret = snprintf(path, sizeof(path), "%s", context.container.save_path);
		if (ret < 0 || ret >= sizeof(path))
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

	ret = enroot_exec_wait_ctx((char *const[]){ "enroot", "export", "-f", "-o", path, context.container.name, NULL });
	if (ret < 0) {
		enroot_print_log_ctx(true);
		return (-1);
	}

	return (0);
}

static int enroot_export(void)
{
	int ret;

	if (context.container.save_path == NULL)
		return (0);

	/* Check if job was interrupted before it fully started. */
	if (context.shm->started_tasks != context.job.local_task_count)
		return (0);

	ret = enroot_container_export();
	if (ret < 0)
		return (-1);

	slurm_spank_log("pyxis: exported container %s to %s", context.container.name, context.container.save_path);

	return (0);
}

static int enroot_cleanup(void)
{
	int ret;
	int rv = 0;

	/* Need to remove the temporary squashfs if the task was interrupted before cleanup. */
	if (context.container.use_enroot_import && context.container.squashfs_path != NULL)
		unlink(context.container.squashfs_path);

	if (context.container.use_importer) {
		ret = importer_exec_release(context.config.importer_path, context.job.uid, context.job.gid,
					    enroot_set_env);
		if (ret < 0) {
			slurm_info("pyxis: failed to call importer release");
			rv = -1;
		}
	}

	if (context.container.temporary_rootfs) {
		slurm_info("pyxis: removing container filesystem: %s", context.container.name);

		ret = enroot_exec_wait_ctx((char *const[]){ "enroot", "remove", "-f", context.container.name, NULL });
		if (ret < 0) {
			slurm_info("pyxis: failed to remove container filesystem: %s", context.container.name);
			rv = -1;
		}
	}

	return (rv);
}

int slurm_spank_task_exit(spank_t sp, int ac, char **av)
{
	int ret;
	int rv = 0;

	if (!context.enabled)
		return (0);

	/* Last task to exit does the container export and/or container cleanup, if needed. */
	if (atomic_fetch_add(&context.shm->completed_tasks, 1) == context.job.local_task_count - 1) {
		ret = enroot_export();
		if (ret < 0) {
			slurm_error("pyxis: failed to export container %s to %s", context.container.name, context.container.save_path);
			rv = -1;
		}

		enroot_cleanup();
	}

	return (rv);
}

int pyxis_slurmstepd_exit(spank_t sp, int ac, char **av)
{
	int ret;
	int rv = 0;

	free(context.container.name);
	free(context.container.squashfs_path);
	free(context.container.save_path);
	free(context.container.cache_data_path_root);
	free(context.container.cache_data_path);

	if (context.job.environ != NULL) {
		for (int i = 0; context.job.environ[i] != NULL; ++i)
			free(context.job.environ[i]);
		free(context.job.environ);
	}

	xclose(context.container.userns_fd);
	xclose(context.container.mntns_fd);
	xclose(context.container.cgroupns_fd);
	xclose(context.container.cwd_fd);
	xclose(context.container.cache_lock_fd);
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
