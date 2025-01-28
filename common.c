/*
 * Copyright (c) 2019-2020, NVIDIA CORPORATION. All rights reserved.
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common.h"

int xasprintf(char **strp, const char *fmt, ...)
{
	int ret;
	va_list ap;

	if (strp == NULL)
		return (-1);

	va_start(ap, fmt);
	ret = vasprintf(strp, fmt, ap);
	va_end(ap);

	/*
	 * man 3 asprintf:
	 * If memory allocation wasn't possible, or some other error occurs, these
	 * functions will return -1, and the contents of strp are undefined.
	 */
	if (ret < 0)
		*strp = NULL;

	return (ret);
}

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

bool array_contains(char **array, size_t len, const char *entry)
{
	for (size_t i = 0; i < len; ++i) {
		if (strcmp(array[i], entry) == 0) {
			return (true);
		}
	}

	return (false);

}

int array_add_unique(char ***array, size_t *len, const char *entry)
{
	char *entry_dup = NULL;
	char **p = NULL;
	int rv = -1;

	if (array_contains(*array, *len, entry)) {
		/* This entry already exists, skip it. */
		return (0);
	}

	entry_dup = strdup(entry);
	if (entry_dup == NULL)
		goto fail;

	p = realloc(*array, sizeof(char *) * (*len + 1));
	if (p == NULL)
		goto fail;
	*array = p;
	p = NULL;

	(*array)[*len] = entry_dup;
	entry_dup = NULL;
	*len += 1;

	rv = 0;

fail:
	free(entry_dup);
	free(p);
	return (rv);
}

void array_free(char ***array, size_t *len)
{
	for (size_t i = 0; i < *len; ++i)
		free((*array)[i]);
	free(*array);

	*array = NULL;
	*len = 0;
}
