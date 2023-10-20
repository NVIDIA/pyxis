/*
 * Copyright (c) 2019-2023, NVIDIA CORPORATION. All rights reserved.
 */

#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
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

static int pyxis_container_cleanup(uid_t uid, gid_t gid, uint32_t jobid)
{
	int ret;
	FILE *fp = NULL;
	char *name = NULL;
	uint32_t id;
	int n;
	int rv = -1;

	fp = enroot_exec_output(uid, gid, NULL,
				(char *const[]){ "enroot", "list", NULL });
	if (fp == NULL) {
		slurm_error("pyxis: couldn't get list of existing containers");
		goto fail;
	}

	while ((name = get_line_from_file(fp)) != NULL) {
		/* Remove named and unnamed pyxis containers for this job */
		if (sscanf(name, "pyxis_%u_%*s%n", &id, &n) == 1) {
			if (strlen(name) != n || id != jobid)
				continue;

			slurm_verbose("pyxis: removing container %s", name);
			ret = enroot_exec_wait(uid, gid, -1, NULL,
					       (char *const[]){ "enroot", "remove", "-f", name, NULL });
			if (ret < 0) {
				slurm_error("pyxis: failed to remove container %s", name);
				goto fail;
			}
		}

		free(name);
		name = NULL;
	}

	rv = 0;

fail:
	free(name);
	if (fp) fclose(fp);
	return (rv);
}

/*
 * Fix a few quirks of the SPANK epilog process state:
 * - The environment is empty
 * - File descriptor 0 is not open
 */
static int job_epilog_fixup(void)
{
	int null_fd;
	int ret;

	null_fd = open("/dev/null", O_RDWR);
	if (null_fd < 0)
		return (-1);

	if (null_fd != STDIN_FILENO) {
		ret = dup2(null_fd, STDIN_FILENO);
		xclose(null_fd);
		if (ret < 0)
			return (-1);
	}

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
		slurm_error("pyxis: failed to parse configuration");
		return (-1);
	}

	if (config.container_scope != SCOPE_JOB)
		return (0);

	ret = job_epilog_fixup();
	if (ret < 0) {
		slurm_error("pyxis: couldn't prepare the job epilog process");
		return (-1);
	}

	rc = spank_get_item(sp, S_JOB_UID, &uid);
	if (rc != ESPANK_SUCCESS) {
		slurm_error("pyxis: couldn't get job uid: %s", spank_strerror(rc));
		return (-1);
	}

	rc = spank_get_item(sp, S_JOB_GID, &gid);
	if (rc != ESPANK_SUCCESS) {
		slurm_error("pyxis: couldn't get job gid: %s", spank_strerror(rc));
		return (-1);
	}

	rc = spank_get_item(sp, S_JOB_ID, &jobid);
	if (rc != ESPANK_SUCCESS) {
		slurm_error("pyxis: couldn't get job ID: %s", spank_strerror(rc));
		return (-1);
	}

	ret = pyxis_container_cleanup(uid, gid, jobid);
	if (ret < 0) {
		slurm_error("pyxis: couldn't cleanup pyxis containers for job %u", jobid);
		return (-1);
	}

	return (0);
}

int pyxis_slurmd_exit(spank_t sp, int ac, char **av)
{
	return (0);
}
