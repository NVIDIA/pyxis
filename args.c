/*
 * Copyright (c) 2020, NVIDIA CORPORATION. All rights reserved.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "args.h"

static struct plugin_args pyxis_args = {
	.image = NULL,
	.mounts = NULL,
	.mounts_len = 0,
	.workdir = NULL,
	.container_name = NULL,
	.container_save = NULL,
	.mount_home = -1,
	.remap_root = -1,
};

static int spank_option_image(int val, const char *optarg, int remote);
static int spank_option_mount(int val, const char *optarg, int remote);
static int spank_option_workdir(int val, const char *optarg, int remote);
static int spank_option_container_name(int val, const char *optarg, int remote);
static int spank_option_container_save(int val, const char *optarg, int remote);
static int spank_option_container_mount_home(int val, const char *optarg, int remote);
static int spank_option_container_remap_root(int val, const char *optarg, int remote);

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
#if PYXIS_REMAP_ROOT_DEFAULT == 1
		" (default)"
#endif
		,
		0, 1, spank_option_container_remap_root
	},
	{
		"no-container-remap-root",
		NULL,
		"[pyxis] do not remap to root inside the container"
#if PYXIS_REMAP_ROOT_DEFAULT == 0
		" (default)"
#endif
		,
		0, 0, spank_option_container_remap_root
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

static int add_mount_entry(const char *entry)
{
	char *entry_dup = NULL;
	char **p = NULL;
	int rv = -1;

	for (size_t i = 0; i < pyxis_args.mounts_len; ++i) {
		/* This mount entry already exists, skip it. */
		if (strcmp(pyxis_args.mounts[i], entry) == 0) {
			slurm_info("pyxis: skipping duplicate mount entry: %s", entry);
			return (0);
		}
	}

	entry_dup = strdup(entry);
	if (entry_dup == NULL)
		goto fail;

	p = realloc(pyxis_args.mounts, sizeof(*pyxis_args.mounts) * (pyxis_args.mounts_len + 1));
	if (p == NULL)
		goto fail;
	pyxis_args.mounts = p;
	p = NULL;

	pyxis_args.mounts[pyxis_args.mounts_len] = entry_dup;
	entry_dup = NULL;
	pyxis_args.mounts_len += 1;

	rv = 0;

fail:
	free(entry_dup);
	free(p);
	return (rv);
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

	pyxis_args.container_name = strdup(optarg);
	return (0);
}

static int spank_option_container_save(int val, const char *optarg, int remote)
{
	if (optarg == NULL || *optarg == '\0') {
		slurm_error("pyxis: --container-save: argument required");
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
		return (false);
	}

	return (true);
}

void pyxis_args_free(void)
{
	free(pyxis_args.image);
	for (int i = 0; i < pyxis_args.mounts_len; ++i)
		free(pyxis_args.mounts[i]);
	free(pyxis_args.mounts);
	free(pyxis_args.workdir);
	free(pyxis_args.container_name);
	free(pyxis_args.container_save);
}

