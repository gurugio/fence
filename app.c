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

void webos_buf_merge_test(int buf_man_fd)
{
	int i;
	int ret;
	struct webos_fence_fd_info info;
	int parent_fence, parent_seqno;
	int child_fence[5];
	int child_seqno[5];
	struct webos_fence_merge_info merge_info;
	pthread_t thr;
	struct webos_fence_wait_info wait_info;

	printf("====================== start merge test ===================\n");

	for (i = 0; i < 2; i++) {
		info.context = 1235+i;
		ret = ioctl(buf_man_fd, BUF_MAN_IOC_CREATE_FENCE, &info);
		if (ret < 0) {
			perror("[APP]fail to create user-fence\n");
			break;
		}
		printf("created-fence:seq=%d fd=%d\n", info.seqno, info.fd);

		child_fence[i] = info.fd;
		child_seqno[i] = info.seqno;
	}

	/* merge normal fences */
	merge_info.fd2 = child_fence[1];
	ret = ioctl(child_fence[0], WEBOS_FENCE_IOC_MERGE, &merge_info);
	if (ret < 0) {
		perror("[APP] fail to merge");
		return;
	}
	parent_fence = merge_info.fence;
	parent_seqno = merge_info.seqno;
	printf("parent fd=%d seqno=%d\n", merge_info.fence, merge_info.seqno);
	sleep(1);

	info.fd = merge_info.fence;
	info.seqno = merge_info.seqno;
	/* main waits for fence and thread signals fence */
	pthread_create(&thr, NULL, thr_signal_fence, &info);

	wait_info.timeout = 20;
	ret = ioctl(info.fd, WEBOS_FENCE_IOC_WAIT, &wait_info);
	if (ret < 0) {
		perror("[APP] fail to wait fence\n");
	}
	printf("meet-fence:seq=%d fd=%d\n", info.seqno, info.fd);
	pthread_join(thr, NULL);


	/* TODO: test to merge merged-fence and normal fence */
	info.context = 1333;
	ret = ioctl(buf_man_fd, BUF_MAN_IOC_CREATE_FENCE, &info);
	if (ret < 0) {
		perror("[APP]fail to create user-fence\n");
	}
	printf("created-fence:seq=%d fd=%d\n", info.seqno, info.fd);

	child_fence[2] = info.fd;
	child_seqno[2] = info.seqno;

	merge_info.fd2 = child_fence[2];
	ret = ioctl(parent_fence, WEBOS_FENCE_IOC_MERGE, &merge_info);
	if (ret < 0) {
		perror("[APP] fail to merge");
	}

	printf("parent fd=%d seqno=%d\n", merge_info.fence, merge_info.seqno);

	sleep(1);
	close(child_fence[2]);
	close(child_fence[0]);
	close(child_fence[1]);

	/* printf("close child-fences\n"); */

	close(merge_info.fence);
	close(parent_fence);
	printf("close parent-fence\n");

	sleep(2);

	printf("======================= end merge test =====================\n");
	return;
}

int main(void)
{
	int buf_man_fd;
	struct webos_fence_fd_info info;
	struct webos_fence_wait_info wait_info;
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

	webos_buf_merge_test(buf_man_fd);
	goto MAIN_END;

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

		wait_info.timeout = 5;
		ret = ioctl(info.fd, WEBOS_FENCE_IOC_WAIT, &wait_info);
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
MAIN_END:
	/* BUGBUG: it should close every-fd of every-fence. */
	close(buf_man_fd);
	return 0;
}
