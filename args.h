/*
 * Copyright (c) 2020, NVIDIA CORPORATION. All rights reserved.
 */

#ifndef ARGS_H_
#define ARGS_H_

#include <stdbool.h>
#include <stddef.h>

#include <slurm/spank.h>

struct plugin_args {
	char *image;
	char **mounts;
	size_t mounts_len;
	char *workdir;
	char *container_name;
	char *container_save;
	int mount_home;
	int remap_root;
	int entrypoint;
};

struct plugin_args *pyxis_args_register(spank_t sp);

bool pyxis_args_enabled(void);

void pyxis_args_free(void);

#endif /* ARGS_H_ */
