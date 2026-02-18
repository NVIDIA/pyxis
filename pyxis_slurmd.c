/*
 * Copyright (c) 2019-2026, NVIDIA CORPORATION. All rights reserved.
 */

#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <slurm/spank.h>

#include "pyxis_slurmd.h"
#include "args.h"
#include "common.h"
#include "config.h"
#include "enroot.h"

int pyxis_slurmd_init(spank_t sp, int ac, char **av)
{
	int ret;
	struct plugin_config config;
	mode_t mask;
	int rv = -1;

	slurm_info("pyxis: version v"PYXIS_VERSION);

	ret = pyxis_config_parse(&config, ac, av);
	if (ret < 0) {
		slurm_error("pyxis: failed to parse configuration");
		return (-1);
	}

	mask = umask(0);

	/* We only attempt to create the last component of the path. */
	ret = mkdir(config.runtime_path, 0755);
	if (ret < 0 && errno != EEXIST) {
		slurm_error("pyxis: slurmd: couldn't mkdir %s: %s", config.runtime_path, strerror(errno));
		goto fail;
	}

	rv = 0;

fail:
	(void)umask(mask);

	return (rv);
}

static int pyxis_container_remove(uid_t uid, gid_t gid, const char *name)
{
	int ret;
	int log_fd = -1;
	int rv = -1;

	slurm_verbose("pyxis: epilog: removing container %s", name);

	log_fd = pyxis_memfd_create("enroot-log", MFD_CLOEXEC);
	if (log_fd < 0) {
		slurm_error("pyxis: epilog: couldn't create in-memory log file: %s", strerror(errno));
		goto fail;
	}

	ret = enroot_exec_wait(uid, gid, log_fd, NULL,
			       (char *const[]){ "enroot", "remove", "-f", (char *)name, NULL });
	if (ret < 0) {
		slurm_error("pyxis: epilog: failed to remove container %s", name);
		memfd_print_log(&log_fd, true, "enroot");
		goto fail;
	}

	slurm_verbose("pyxis: epilog: removed container %s", name);

	rv = 0;

fail:
	xclose(log_fd);
	return (rv);
}

static bool pyxis_container_match_job(const char *name, uint32_t jobid)
{
	uint32_t id;
	int n = 0;

	if (sscanf(name, "pyxis_%u_%*s%n", &id, &n) != 1)
		return (false);

	return (strlen(name) == n && id == jobid);
}

static int pyxis_container_cleanup(uid_t uid, gid_t gid, uint32_t jobid)
{
	FILE *fp = NULL;
	char *name = NULL;
	int rv = 0;
	int leftover = 0;

	fp = enroot_exec_output(uid, gid, NULL,
				(char *const[]){ "enroot", "list", NULL });
	if (fp == NULL) {
		slurm_error("pyxis: epilog: couldn't get list of existing containers");
		return (-1);
	}

	while ((name = get_line_from_file(fp)) != NULL) {
		if (pyxis_container_match_job(name, jobid)) {
			if (pyxis_container_remove(uid, gid, name) < 0)
				rv = -1;
		}

		free(name);
		name = NULL;
	}

	fclose(fp);

	/*
	 * Some removals failed. Check if the containers were removed anyway.
	 */
	if (rv < 0) {
		slurm_verbose("pyxis: epilog: checking for leftover containers");

		fp = enroot_exec_output(uid, gid, NULL,
					(char *const[]){ "enroot", "list", NULL });
		if (fp == NULL) {
			slurm_error("pyxis: epilog: couldn't get list of existing containers");
			return (-1);
		}

		while ((name = get_line_from_file(fp)) != NULL) {
			if (pyxis_container_match_job(name, jobid)) {
				slurm_error("pyxis: epilog: container %s was not removed", name);
				leftover += 1;
			}

			free(name);
			name = NULL;
		}

		fclose(fp);

		if (leftover == 0) {
			slurm_verbose("pyxis: epilog: no leftover containers");
			rv = 0;
		}
	}

	return (rv);
}

/*
 * Fix the environment of the SPANK epilog process
 */
static int job_epilog_fixup(void)
{
	int ret;

	ret = setenv("PATH", "/usr/local/bin:/usr/bin:/bin", 0);
	if (ret < 0)
		return (-1);

	ret = unsetenv("HOME");
	if (ret < 0)
		return (-1);

	return (0);
}

int slurm_spank_job_epilog(spank_t sp, int ac, char **av)
{
	struct plugin_config config;
	spank_err_t rc;
	uid_t uid;
	gid_t gid;
	uint32_t jobid;
	int ret;

	ret = pyxis_config_parse(&config, ac, av);
	if (ret < 0) {
		slurm_error("pyxis: epilog: failed to parse configuration");
		return (-1);
	}

	if (config.container_scope != SCOPE_JOB)
		return (0);

	ret = job_epilog_fixup();
	if (ret < 0) {
		slurm_error("pyxis: epilog: couldn't prepare the job epilog process");
		return (-1);
	}

	rc = spank_get_item(sp, S_JOB_UID, &uid);
	if (rc != ESPANK_SUCCESS) {
		slurm_error("pyxis: epilog: couldn't get job uid: %s", spank_strerror(rc));
		return (-1);
	}

	rc = spank_get_item(sp, S_JOB_GID, &gid);
	if (rc != ESPANK_SUCCESS) {
		slurm_error("pyxis: epilog: couldn't get job gid: %s", spank_strerror(rc));
		return (-1);
	}

	rc = spank_get_item(sp, S_JOB_ID, &jobid);
	if (rc != ESPANK_SUCCESS) {
		slurm_error("pyxis: epilog: couldn't get job ID: %s", spank_strerror(rc));
		return (-1);
	}

	ret = pyxis_container_cleanup(uid, gid, jobid);
	if (ret < 0) {
		slurm_error("pyxis: epilog: couldn't cleanup pyxis containers for job %u", jobid);
		return (-1);
	}

	return (0);
}

int pyxis_slurmd_exit(spank_t sp, int ac, char **av)
{
	return (0);
}
