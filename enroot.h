/*
 * Copyright (c) 2020, NVIDIA CORPORATION. All rights reserved.
 */

#ifndef ENROOT_H_
#define ENROOT_H_

#include <sys/types.h>
#include <stdbool.h>
#include <stdio.h>

typedef int (*child_cb)(void);

pid_t enroot_exec(uid_t uid, gid_t gid, int log_fd,
		  child_cb callback, char *const argv[]);

int enroot_exec_wait(uid_t uid, gid_t gid, int log_fd,
		     child_cb callback, char *const argv[]);

FILE *enroot_exec_output(uid_t uid, gid_t gid,
			 child_cb callback, char *const argv[]);

void enroot_print_log(int log_fd, bool error);

#endif /* ENROOT_H_ */
