/*
 * Copyright (c) 2025, NVIDIA CORPORATION. All rights reserved.
 */

#ifndef IMPORTER_H_
#define IMPORTER_H_

#include <stdbool.h>

typedef int (*child_cb)(void);

int importer_exec_get(const char *importer_path, uid_t uid, gid_t gid,
		      child_cb callback, const char *image_uri, char **squashfs_path);

int importer_exec_release(const char *importer_path, uid_t uid, gid_t gid,
			  child_cb callback);

#endif /* IMPORTER_H_ */

