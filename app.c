#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/ioctl.h>

#include <stdio.h>
#include <stdint.h>
#include <pthread.h>

#include "webos_fence_uapi.h"


void *thr_signal_fence(void *fence_info)
{
	struct webos_fence_fd_info *info = fence_info;
	int ret;
	
	sleep(2);
	ret = ioctl(info->fd, WEBOS_FENCE_IOC_SIGNAL, NULL);
	if (ret < 0)
		printf("[APP] fail to signal fence: seq=%d fd=%d\n",
		       info->seqno, info->fd);
	return NULL;
}

int main(void)
{
	int buf_man_fd;
	struct webos_fence_fd_info info;
	int ret;
	int i;
	int fence_fd[10] = {0, };
	int fence_seqno[10];
	pthread_t thr;
	
	buf_man_fd = open("/dev/buf_man", O_RDONLY);
	if (buf_man_fd < 0) {
		perror("[APP]fail to open:/dev/buf_man\n");
		return 1;
	}

	for (i = 0; i < 10; i++) {
		info.context = 1234 + i;
		ret = ioctl(buf_man_fd, BUF_MAN_IOC_CREATE_FENCE, &info);
		if (ret < 0) {
			perror("[APP]fail to create user-fence\n");
			break;
		}
		printf("created-fence:seq=%d fd=%d\n", info.seqno, info.fd);

		fence_fd[i] = info.fd;
		fence_seqno[i] = info.seqno;


		/* main waits for fence and thread signals fence */
		pthread_create(&thr, NULL, thr_signal_fence, &info);
		ret = ioctl(info.fd, WEBOS_FENCE_IOC_WAIT, NULL);
		if (ret < 0) {
			perror("[APP] fail to wait fence\n");
			break;
		}
		printf("meet-fence:seq=%d fd=%d\n", info.seqno, info.fd);
		pthread_join(thr, NULL);
	}

	for (i = 0; i < 10; i++) {
		printf("close fence:seq=%d fd=%d\n", fence_seqno[i], fence_fd[i]);
		if (fence_fd[i] > 0) close(fence_fd[i]);
	}

	/* BUGBUG: it should close every-fd of every-fence. */
	close(buf_man_fd);
	return 0;
}
