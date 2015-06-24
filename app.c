#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include <stdio.h>

int main(void)
{
	int fd;

	fd = open("/dev/webos_sync", O_RDONLY);
	if (fd < 0) {
		printf("fail to open\n");
		return 1;
	}

	close(fd);
	return 0;
}
