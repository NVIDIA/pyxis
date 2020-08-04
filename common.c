/*
 * Copyright (c) 2019-2020, NVIDIA CORPORATION. All rights reserved.
 */

#include <stdlib.h>
#include <string.h>

#include "common.h"

char *get_line_from_file(FILE *fp)
{
	size_t len = 0;
	char *line = NULL;

	if (getline(&line, &len, fp) == -1) {
		free(line);
		return NULL;
	}

	len = strlen(line);
	if (len > 0 && line[len - 1] == '\n')
		line[len - 1] = '\0'; /* trim trailing newline */
	return line;
}

char *join_strings(char *const strings[], const char *sep)
{
	size_t strings_count, result_len = 0;
	char *result;

	if (strings[0] == NULL)
		return NULL;

	for (strings_count = 0; strings[strings_count] != NULL; ++strings_count)
		result_len += strlen(strings[strings_count]);

	result_len += strlen(sep) * (strings_count - 1) + 1;

	result = malloc(result_len);
	if (result == NULL)
		return NULL;
	result[0] = '\0';

	for (size_t i = 0; i < strings_count; ++i) {
		if (i != 0)
			strcat(result, sep);
		strcat(result, strings[i]);
	}
	return result;
}
