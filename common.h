#ifndef COMMON_H_
#define COMMON_H_

#include <unistd.h>

#define PYXIS_RUNTIME_PATH "/run/pyxis"
#define PYXIS_USER_RUNTIME_PATH "/run/pyxis/%d"

#define ARRAY_SIZE(x) (sizeof(x) / sizeof(*x))

static inline void xclose(int fd)
{
	if (fd < 0)
		return;
	close(fd);
}

#endif /* COMMON_H_ */
