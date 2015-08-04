#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/ioctl.h>

#include <stdio.h>
#include <stdint.h>
#include <pthread.h>
#include <assert.h>

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
	int grand_parent_fence, grand_parent_seqno;

	int child_fence[5];
	int child_seqno[5];
	struct webos_fence_merge_info merge_info;
	pthread_t thr;
	struct webos_fence_wait_info wait_info;

	/*
	 * TEST: merge leaf-fences
	 */
	for (i = 0; i < 2; i++) {
		info.context = 1235+i;
		ret = ioctl(buf_man_fd, BUF_MAN_IOC_CREATE_FENCE, &info);
		if (ret < 0) {
			perror("[APP]fail to create user-fence\n");
			break;
		}
		printf("create normal-fence:seq=%d fd=%d\n", info.seqno, info.fd);

		child_fence[i] = info.fd;
		child_seqno[i] = info.seqno;
	}

	/* grand_fence = child_fence[0] + child_fence[1] */
	merge_info.fd2 = child_fence[1];
	ret = ioctl(child_fence[0], WEBOS_FENCE_IOC_MERGE, &merge_info);
	if (ret < 0) {
		perror("[APP] fail to merge");
		assert(ret >= 0);
	}
	parent_fence = merge_info.fence;
	parent_seqno = merge_info.seqno;
	printf("create parent-fence seqno=%d fd=%d\n", parent_seqno, parent_fence);

	info.fd = parent_fence;
	info.seqno = parent_seqno;
	pthread_create(&thr, NULL, thr_signal_fence, &info);

	wait_info.timeout = 20;
	ret = ioctl(info.fd, WEBOS_FENCE_IOC_WAIT, &wait_info);
	assert(ret >= 0);
	printf("meet-fence:seq=%d fd=%d\n", info.seqno, info.fd);
	pthread_join(thr, NULL);

	system("cat /sys/kernel/debug/webos_fence");

	/*
	 * TEST: merge parent fence and normal fence
	 */
	info.context = 1333;
	ret = ioctl(buf_man_fd, BUF_MAN_IOC_CREATE_FENCE, &info);
	assert(ret >= 0);
	printf("create normal-fence:seq=%d fd=%d\n", info.seqno, info.fd);

	child_fence[2] = info.fd;
	child_seqno[2] = info.seqno;

	/* parent_fence is already signaled so need to be ready */
	ret = ioctl(parent_fence, WEBOS_FENCE_IOC_READY, NULL);
	assert(ret >= 0);

	/* grand_parend_fence = parent_fence + child_fence[2] */
	merge_info.fd2 = child_fence[2];
	ret = ioctl(parent_fence, WEBOS_FENCE_IOC_MERGE, &merge_info);
	assert(ret >= 0);

	grand_parent_fence = merge_info.fence;
	grand_parent_seqno = merge_info.seqno;
	printf("create grand-parent-fence fd=%d seqno=%d\n",
	       grand_parent_fence, grand_parent_seqno);

	info.fd = grand_parent_fence;
	info.seqno = grand_parent_seqno;
	pthread_create(&thr, NULL, thr_signal_fence, &info);

	wait_info.timeout = 20;
	ret = ioctl(info.fd, WEBOS_FENCE_IOC_WAIT, &wait_info);
	assert(ret >= 0);

	printf("meet-fence:seq=%d fd=%d\n", info.seqno, info.fd);
	pthread_join(thr, NULL);

	system("cat /sys/kernel/debug/webos_fence");

	/*
	 * TEST: recycle merged fence
	 */
	ret = ioctl(grand_parent_fence, WEBOS_FENCE_IOC_READY, NULL);
	assert(ret >= 0);

	info.fd = grand_parent_fence;
	info.seqno = grand_parent_seqno;
	pthread_create(&thr, NULL, thr_signal_fence, &info);

	wait_info.timeout = 20;
	ret = ioctl(info.fd, WEBOS_FENCE_IOC_WAIT, &wait_info);
	/* wait must success after ready */
	assert(ret >= 0);
	printf("meet-fence again:seq=%d fd=%d\n", info.seqno, info.fd);
	pthread_join(thr, NULL);

	system("cat /sys/kernel/debug/webos_fence");

	/*
	 * END of test
	 */
	close(child_fence[2]);
	close(child_fence[0]);
	close(child_fence[1]);
	close(grand_parent_fence);
	close(parent_fence);
	printf("Every fence must be freed\n");
	system("cat /sys/kernel/debug/webos_fence");

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

	/* fd must start with 4.
	 * If not, closing fence in previous test has failed.
	 */
	assert(fence_fd[0] == 4);
	assert(fence_fd[9] == 13);

	for (i = 0; i < 10; i++) {
		printf("close fence:seq=%d fd=%d\n", fence_seqno[i], fence_fd[i]);
		if (fence_fd[i] > 0) close(fence_fd[i]);
	}
MAIN_END:
	close(buf_man_fd);
	return 0;
}
