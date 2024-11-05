/*
 * Copyright (c) 2020-2022, NVIDIA CORPORATION. All rights reserved.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "args.h"
#include "common.h"

static struct plugin_args pyxis_args = {
	.image = NULL,
	.mounts = NULL,
	.mounts_len = 0,
	.workdir = NULL,
	.container_name = NULL,
	.container_name_flags = NULL,
	.container_save = NULL,
	.mount_home = -1,
	.remap_root = -1,
	.entrypoint = -1,
	.entrypoint_log = -1,
	.writable   = -1,
	.env_vars = NULL,
	.env_vars_len = 0,
};

static int spank_option_image(int val, const char *optarg, int remote);
static int spank_option_mount(int val, const char *optarg, int remote);
static int spank_option_workdir(int val, const char *optarg, int remote);
static int spank_option_container_name(int val, const char *optarg, int remote);
static int spank_option_container_save(int val, const char *optarg, int remote);
static int spank_option_container_mount_home(int val, const char *optarg, int remote);
static int spank_option_container_remap_root(int val, const char *optarg, int remote);
static int spank_option_container_entrypoint(int val, const char *optarg, int remote);
static int spank_option_container_entrypoint_log(int val, const char *optarg, int remote);
static int spank_option_container_writable(int val, const char *optarg, int remote);
static int spank_option_container_env(int val, const char *optarg, int remote);

struct spank_option spank_opts[] =
{
	{
		"container-image",
		"[USER@][REGISTRY#]IMAGE[:TAG]|PATH",
		"[pyxis] the image to use for the container filesystem. Can be either a docker image given as an enroot URI, "
			"or a path to a squashfs file on the remote host filesystem.",
		1, 0, spank_option_image
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
		"container-save",
		"PATH",
		"[pyxis] Save the container state to a squashfs file on the remote host filesystem.",
		1, 0, spank_option_container_save
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
	{
		"container-remap-root",
		NULL,
		"[pyxis] ask to be remapped to root inside the container. "
		"Does not grant elevated system permissions, despite appearances."
		,
		0, 1, spank_option_container_remap_root
	},
	{
		"no-container-remap-root",
		NULL,
		"[pyxis] do not remap to root inside the container"
		,
		0, 0, spank_option_container_remap_root
	},
	{
		"container-entrypoint",
		NULL,
		"[pyxis] execute the entrypoint from the container image"
		,
		0, 1, spank_option_container_entrypoint
	},
	{
		"no-container-entrypoint",
		NULL,
		"[pyxis] do not execute the entrypoint from the container image"
		,
		0, 0, spank_option_container_entrypoint
	},
	{
		"container-entrypoint-log",
		NULL,
		"[pyxis] print the output of the entrypoint script"
		,
		0, 1, spank_option_container_entrypoint_log
	},
	{
		"container-writable",
		NULL,
		"[pyxis] make the container filesystem writable"
		,
		0, 1, spank_option_container_writable
	},
	{
		"container-readonly",
		NULL,
		"[pyxis] make the container filesystem read-only"
		,
		0, 0, spank_option_container_writable
	},
	{
		"container-env",
		"NAME[,NAME...]",
		"[pyxis] names of environment variables to override with the host environment and set at the entrypoint. "
		"By default, all exported host environment variables are set in the container after the entrypoint is run, "
		"but their existing values in the image take precedence; "
		"the variables specified with this flag are preserved from the host and set before the entrypoint runs",
		1, 0, spank_option_container_env
	},
	SPANK_OPTIONS_TABLE_END
};

static int spank_option_image(int val, const char *optarg, int remote)
{
	if (optarg == NULL || *optarg == '\0') {
		slurm_error("pyxis: --container-image: argument required");
		return (-1);
	}

	/* Slurm can call us twice with the same value, check if it's a different value than before. */
	if (pyxis_args.image != NULL) {
		if (strcmp(pyxis_args.image, optarg) == 0)
			return (0);

		slurm_error("pyxis: --container-image specified multiple times");
		return (-1);
	}

	pyxis_args.image = strdup(optarg);
	return (0);
}

int add_mount(const char *source, const char *target, const char *flags)
{
	int ret;
	char *entry = NULL;
	const char *default_flags;
	int rv = -1;

	if (strspn(source, "./") > 0) {
		default_flags = "x-create=auto,rbind";
	} else {
		if (strcmp(source, "tmpfs") == 0) {
			default_flags = "x-create=dir";
		} else if (strcmp(source, "umount") == 0) {
			default_flags = "x-detach";
		} else {
			slurm_error("pyxis: mount source must be a relative path, an absolute path, \"tmpfs\" or \"umount\"");
			goto fail;
		}
	}

	if (strspn(target, "./") == 0) {
		slurm_error("pyxis: mount target must be a relative path or an absolute path");
		goto fail;
	}

	if (flags != NULL)
		ret = xasprintf(&entry, "%s %s %s,%s", source, target, default_flags, flags);
	else
		ret = xasprintf(&entry, "%s %s %s", source, target, default_flags);
	if (ret < 0) {
		slurm_error("pyxis: could not allocate memory");
		goto fail;
	}

	ret = array_add_unique(&pyxis_args.mounts, &pyxis_args.mounts_len, entry);
	if (ret < 0)
		goto fail;

	rv = 0;

fail:
	free(entry);

	return (rv);
}

void remove_all_mounts(void)
{
	array_free(&pyxis_args.mounts, &pyxis_args.mounts_len);
}

static int parse_mount_option(const char *option)
{
	int ret;
	char *option_dup = NULL;
	char *remainder, *src, *dst, *flags = NULL;
	int rv = -1;

	if (option == NULL)
		return (-1);
	option_dup = strdup(option);
	remainder = option_dup;

	src = strsep(&remainder, ":");
	if (src == NULL || *src == '\0') {
		slurm_error("pyxis: --container-mounts: invalid format: %s", option);
		goto fail;
	}
	dst = src;

	if (remainder == NULL)
		goto done;

	dst = strsep(&remainder, ":");
	if (dst == NULL || *dst == '\0') {
		slurm_error("pyxis: --container-mounts: invalid format: %s", option);
		goto fail;
	}

	if (remainder == NULL || remainder[0] == '\0')
		goto done;
	flags = remainder;
	/*
	 * enroot uses "," as the separator for mount flags, but we already use this character for
	 * separating mount entries, so we use "+" for mount flags and convert to "," here.
	 */
	for (int i = 0; flags[i]; ++i)
		if (flags[i] == '+')
			flags[i] = ',';

done:
	ret = add_mount(src, dst, flags);
	if (ret < 0) {
		slurm_error("pyxis: could not add mount entry: %s:%s", src, dst);
		goto fail;
	}

	rv = 0;

fail:
	free(option_dup);
	return (rv);
}

static int spank_option_mount(int val, const char *optarg, int remote)
{
	int ret;
	char *optarg_dup = NULL;
	char *args, *arg;
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
		ret = parse_mount_option(arg);
		if (ret < 0)
			goto fail;
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
	if (pyxis_args.workdir != NULL) {
		if (strcmp(pyxis_args.workdir, optarg) == 0)
			return (0);

		slurm_error("pyxis: --container-workdir specified multiple times");
		return (-1);
	}

	pyxis_args.workdir = strdup(optarg);
	return (0);
}

static int spank_option_container_name(int val, const char *optarg, int remote)
{
	char *optarg_dup = NULL;
	char *name, *flags;
	int rv = -1;

	if (optarg == NULL || *optarg == '\0') {
		slurm_error("pyxis: --container-name: argument required");
		return (-1);
	}

	/* Slurm can call us twice with the same value, check if it's a different value than before. */
	if (pyxis_args.container_name != NULL) {
		if (strcmp(pyxis_args.container_name, optarg) == 0)
			return (0);

		slurm_error("pyxis: --container-name specified multiple times");
		return (-1);
	}

	optarg_dup = strdup(optarg);

	flags = optarg_dup;
	name = strsep(&flags, ":");

	if (name == NULL || name[0] == '\0') {
		slurm_error("pyxis: --container-name: empty name");
		goto fail;
	}

	if (flags == NULL || flags[0] == '\0')
		flags = "auto";

	if (strcmp(flags, "auto") != 0 && strcmp(flags, "create") != 0 &&
	    strcmp(flags, "exec") != 0 && strcmp(flags, "no_exec") != 0) {
		slurm_error("pyxis: --container-name: flag must be \"auto\", \"create\", \"exec\" or \"no_exec\"");
		goto fail;
	}

	pyxis_args.container_name = strdup(name);
	pyxis_args.container_name_flags = strdup(flags);

	rv = 0;

fail:
	free(optarg_dup);

	return (rv);
}

static int spank_option_container_save(int val, const char *optarg, int remote)
{
	if (optarg == NULL || *optarg == '\0') {
		slurm_error("pyxis: --container-save: argument required");
		return (-1);
	}

	if (optarg[strlen(optarg) - 1] == '/') {
		slurm_error("pyxis: --container-save: target is a directory");
		return (-1);
	}

	/* Slurm can call us twice with the same value, check if it's a different value than before. */
	if (pyxis_args.container_save != NULL) {
		if (strcmp(pyxis_args.container_save, optarg) == 0)
			return (0);

		slurm_error("pyxis: --container-save specified multiple times");
		return (-1);
	}

	pyxis_args.container_save = strdup(optarg);
	return (0);
}

static int spank_option_container_mount_home(int val, const char *optarg, int remote)
{
	if (pyxis_args.mount_home != -1 && pyxis_args.mount_home != val) {
		slurm_error("pyxis: both --container-mount-home and --no-container-mount-home were specified");
		return (-1);
	}

	pyxis_args.mount_home = val;

	return (0);
}

static int spank_option_container_remap_root(int val, const char *optarg, int remote)
{
	if (pyxis_args.remap_root != -1 && pyxis_args.remap_root != val) {
		slurm_error("pyxis: both --container-remap-root and --no-container-remap-root were specified");
		return (-1);
	}

	pyxis_args.remap_root = val;

	return (0);
}

static int spank_option_container_entrypoint(int val, const char *optarg, int remote)
{
	if (pyxis_args.entrypoint != -1 && pyxis_args.entrypoint != val) {
		slurm_error("pyxis: both --container-entrypoint and --no-container-entrypoint were specified");
		return (-1);
	}

	pyxis_args.entrypoint = val;

	return (0);
}

static int spank_option_container_entrypoint_log(int val, const char *optarg, int remote)
{
	pyxis_args.entrypoint_log = val;

	return (0);
}

static int spank_option_container_writable(int val, const char *optarg, int remote)
{
	if (pyxis_args.writable != -1 && pyxis_args.writable != val) {
		slurm_error("pyxis: both --container-writable and --container-readonly were specified");
		return (-1);
	}

	pyxis_args.writable = val;

	return (0);
}

static int spank_option_container_env(int val, const char *optarg, int remote)
{
	int ret;
	char *optarg_dup = NULL;
	char *args, *arg;
	int rv = -1;

	if (optarg == NULL || *optarg == '\0') {
		slurm_error("pyxis: --container-env: argument required");
		goto fail;
	}

	optarg_dup = strdup(optarg);
	if (optarg_dup == NULL) {
		slurm_error("pyxis: could not allocate memory");
		goto fail;
	}

	args = optarg_dup;
	while ((arg = strsep(&args, ",")) != NULL) {
		if (*arg == '\0') {
			slurm_error("pyxis: --container-env: invalid format: %s", optarg);
			goto fail;
		}

		ret = array_add_unique(&pyxis_args.env_vars, &pyxis_args.env_vars_len, arg);
		if (ret < 0)
			goto fail;
	}

	rv = 0;

fail:
	free(optarg_dup);

	return (rv);
}

struct plugin_args *pyxis_args_register(spank_t sp)
{
	spank_err_t rc;

	for (int i = 0; spank_opts[i].name != NULL; ++i) {
		rc = spank_option_register(sp, &spank_opts[i]);
		if (rc != ESPANK_SUCCESS) {
			slurm_error("pyxis: couldn't register option %s: %s", spank_opts[i].name, spank_strerror(rc));
			return (NULL);
		}
	}

	return (&pyxis_args);
}

bool pyxis_args_enabled(void)
{
	if (pyxis_args.image == NULL && pyxis_args.container_name == NULL) {
		if (pyxis_args.mounts_len > 0)
			slurm_error("pyxis: ignoring --container-mounts because neither --container-image nor --container-name is set");
		if (pyxis_args.workdir != NULL)
			slurm_error("pyxis: ignoring --container-workdir because neither --container-image nor --container-name is set");
		if (pyxis_args.mount_home != -1)
			slurm_error("pyxis: ignoring --[no-]container-mount-home because neither --container-image nor --container-name is set");
		if (pyxis_args.remap_root != -1)
			slurm_error("pyxis: ignoring --[no-]container-remap-root because neither --container-image nor --container-name is set");
		if (pyxis_args.entrypoint != -1)
			slurm_error("pyxis: ignoring --[no-]container-entrypoint because neither --container-image nor --container-name is set");
		return (false);
	}

	return (true);
}

void pyxis_args_free(void)
{
	free(pyxis_args.image);
	remove_all_mounts();
	free(pyxis_args.workdir);
	free(pyxis_args.container_name);
	free(pyxis_args.container_name_flags);
	free(pyxis_args.container_save);
	array_free(&pyxis_args.env_vars, &pyxis_args.env_vars_len);
}

