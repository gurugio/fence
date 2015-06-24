
#include <linux/device.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/cdev.h>

#include <linux/export.h>
#include <linux/vmalloc.h>
#include <linux/mm_types.h>
#include <linux/types.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/kallsyms.h>
#include <linux/miscdevice.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/rcupdate.h>

#include <linux/sizes.h>
#include <linux/fence.h>

MODULE_LICENSE("GPL");


struct webos_fence
{
	struct fence base;
	spinlock_t lock;

	/* TODO: process sharing */
	int fd;
};

struct webos_surface
{
	char name[20];
	char *buf;
	size_t buf_size;
};

extern struct webos_fence *webos_fence_get(void);
extern void webos_fence_put(struct webos_fence *);
extern struct webos_surface *webos_get_buf(int i);

static int __init webos_fence_init(void)
{
	int i;
	struct webos_fence *fence;
	struct webos_surface *sur;

	for (i = 0; i < 6; i++) {
		fence = webos_fence_get();
		printk("\t\t==== screen_man ====\n");
		printk("\t\tget fence=%p context=%d id=%d\n",
		       &fence->base, fence->base.context, fence->base.seqno);

		fence_wait_timeout(&fence->base, true, 10*HZ);
		printk("\t\tmeet fence[%d]:flag=%lx\n",
		       fence->base.seqno,
		       fence->base.flags);

		sur = webos_get_buf(fence->base.context);
		printk("\t\tget_buf=%p size=%x\n", sur->buf, sur->buf_size);
		memset(sur->buf, fence->base.context, sur->buf_size);

		webos_fence_put(fence);
	}

	printk(KERN_CRIT "\t\tscreen_man init\n\n");

	return 0;
}

static void __exit webos_fence_exit(void)
{
	printk(KERN_CRIT "\t\tscreen_man exit\n\n");
}


module_init(webos_fence_init);
module_exit(webos_fence_exit);


