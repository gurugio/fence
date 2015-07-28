
typedef __signed__ int __s32;

#define WEBOS_FENCE_IOC_MAGIC		'<'
#define BUF_MAN_IOC_CREATE_FENCE	_IOW(WEBOS_FENCE_IOC_MAGIC, 1, __s32)
#define WEBOS_FENCE_IOC_WAIT 		_IOW(WEBOS_FENCE_IOC_MAGIC, 2, __s32)
#define WEBOS_FENCE_IOC_READY		_IOW(WEBOS_FENCE_IOC_MAGIC, 3, __s32)
#define WEBOS_FENCE_IOC_SIGNAL		_IOW(WEBOS_FENCE_IOC_MAGIC, 4, __s32)


struct webos_fence_fd_info
{
	/* input */
	unsigned int context;
	/* output */
	unsigned int seqno;
	int fd;
};

struct webos_fence_wait_info
{
	/* input */
	int timeout;
};

struct webos_fence_merge_info
{
	/* fence to be merged */
	int fd2;

	/* new fence */
	int fence;
	unsigned int seqno;
};
