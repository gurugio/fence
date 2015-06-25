#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/ioctl.h>

#include <stdio.h>
#include <stdint.h>

typedef __signed__ int __s32;

#define WEBOS_FENCE_IOC_MAGIC		'<'

#define BUF_MAN_IOC_GET_FENCE		_IOW(WEBOS_FENCE_IOC_MAGIC, 0, __s32)
#define WEBOS_FENCE_IOC_WAIT 		_IOW(WEBOS_FENCE_IOC_MAGIC, 1, __s32)


struct webos_fence_fd_info
{
	int reqno;
	int fd;
};

int main(void)
{
	int buf_man_fd;
	struct webos_fence_fd_info info;
	int ret;
	int i;

	buf_man_fd = open("/dev/buf_man", O_RDONLY);
	if (buf_man_fd < 0) {
		perror("[APP]fail to open:/dev/buf_man\n");
		return 1;
	}

	for (i = 0; i < 5; i++) {
		ret = ioctl(buf_man_fd, BUF_MAN_IOC_GET_FENCE, &info);
		if (ret < 0) {
			perror("[APP]fail to get fence_fd\n");
			return 1;
		}
		printf("[APP]get-fence:reqno=%d fd=%d\n", info.reqno, info.fd);

		ret = ioctl(info.fd, WEBOS_FENCE_IOC_WAIT, 0);
		if (ret < 0) {
			perror("fail to ioctl-fence\n");
			return 1;
		}
		printf("[APP] meet fence[%d]\n", info.reqno);

		close(info.fd);
	}
	

	close(buf_man_fd);
	return 0;
}
