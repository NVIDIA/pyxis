#include <stdlib.h>
#include <string.h>

#include "common.h"

char *get_line_from_file(FILE *fp)
{
	size_t len = 0;
	char * line = NULL;

	if (getline(&line, &len, fp) == -1) {
		free(line);
		return NULL;
	}

	len = strlen(line);
	if (len > 0 && line[len - 1] == '\n')
		line[len - 1] = '\0'; /* trim trailing newline */
	return line;
}

char *join_strings(char *const strings[], char *const sep)
{
	size_t strings_count, sep_len, result_len = 0;
	char *result;

	if (strings[0] == NULL)
		return NULL;

	for (strings_count = 0; strings[strings_count] != NULL; ++strings_count)
		result_len += strlen(strings[strings_count]);

	sep_len = strlen(sep);
	result_len += sep_len * (strings_count - 1) + 1;

	result = calloc(1, result_len);
	if (result == NULL)
		return NULL;

	for (int i = 0; i < strings_count; ++i) {
		if (i != 0)
			strncat(result, sep, sep_len);
		strcat(result, strings[i]);
	}
	return result;
}
