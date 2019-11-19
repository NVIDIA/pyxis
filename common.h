#ifndef COMMON_H_
#define COMMON_H_

#include <unistd.h>
#include <stdio.h>
#include <sys/syscall.h>

#define PYXIS_RUNTIME_PATH "/run/pyxis"
#define PYXIS_USER_RUNTIME_PATH "/run/pyxis/%d"

#define ARRAY_SIZE(x) (sizeof(x) / sizeof(*x))

static inline void xclose(int fd)
{
	if (fd < 0)
		return;
	close(fd);
}

#if !defined(MFD_CLOEXEC)
# define MFD_CLOEXEC 0x0001U
#endif /* !defined(MFD_CLOEXEC) */

#if defined(__x86_64__)
# if !defined(__NR_memfd_create)
#  define __NR_memfd_create 319
# endif /* !defined(__NR_memfd_create) */
#endif /* defined(__x86_64__) */

static inline int pyxis_memfd_create(const char *name, unsigned int flags)
{
	return syscall(__NR_memfd_create, name, flags);
}

static char *get_line_from_file(FILE *fp)
{
	ssize_t read;
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

static char *join_strings(char *const strings[], char *const sep)
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

#endif /* COMMON_H_ */
