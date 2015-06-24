
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
#include <linux/anon_inodes.h>
#include <linux/uaccess.h>

#include <linux/sizes.h>
#include <linux/fence.h>

MODULE_LICENSE("GPL");

struct webos_fence_fd_into
{
	int reqno;
	int fd;
};

struct webos_fence
{
	struct fence base;
	spinlock_t lock;

	/* for process */
	int fd;
};

struct webos_surface
{
	char name[20];
	char *buf;
	size_t buf_size;
};


struct webos_surface *surface[3];
struct webos_fence *wfence[3];

struct webos_fence *latest_fence;



struct task_struct *sync_thr;

static int sync_thr_func(void *arg)
{
	int old_fence_id = -1;
	/* int stop=0; */
	
	printk("\t\tstart sync_thr\n");

	/*
	 * Simulate buffer manipulation.
	 * At every 2-sec it sends signal that the buffer is ready.
	 */
	while (!kthread_should_stop()) {
		if (latest_fence && old_fence_id != latest_fence->base.seqno) {
			fence_signal(&latest_fence->base);
			old_fence_id = latest_fence->base.seqno;
			printk("signal-%d\n", old_fence_id);
		} else {
			printk("no signal\n");
		}

		ssleep(2);

		/* if (stop++ > 10) /\* end simulation *\/ */
		/* 	break; */
	}

	return 0;
}

static void webos_fence_release(struct fence *fence)
{
	/* fence_free(fence); /\* this free wfence *\/ */

	/* not free fence */
	printk("release fence=%p\n", fence);
	/* fence_free(fence); */
}

static bool webos_fence_enable_signaling(struct fence *fence)
{
	clear_bit(FENCE_FLAG_SIGNALED_BIT, &fence->flags);
	
	return true;
}

static const char *webos_fence_get_driver_name(struct fence *fence)
{
	return "buffer-manager";
}

static const char *webos_fence_get_timeline_name(struct fence *fence)
{
	return NULL;
}

static const struct fence_ops webos_fence_ops = {
	.get_driver_name = webos_fence_get_driver_name,
	.get_timeline_name = webos_fence_get_timeline_name,
	.enable_signaling = webos_fence_enable_signaling,
	.wait = fence_default_wait,
	//.signaled = webos_fence_signaled,
	.release = webos_fence_release,
	/* .fill_driver_data = webos_fence_fill_driver_data, */
};

struct webos_fence *webos_fence_get(void)
{
	static uint32_t fence_seq = 0;
	struct webos_fence *wf;

	wf = wfence[fence_seq % 3];
	fence_get(&wf->base);

	wf->base.seqno = fence_seq;
	fence_seq++;

	/* recycle fence */
	clear_bit(FENCE_FLAG_SIGNALED_BIT, &wf->base.flags);
	
	/* printk("get_fence[%p]: seq=%d ref=%d\n", */
	/*        wf, wf->base.seqno, atomic_read(&wf->base.refcount.refcount)); */

	latest_fence = wf;
	return wf;
}
EXPORT_SYMBOL(webos_fence_get);

void webos_fence_put(struct webos_fence *wf)
{
	fence_put(&wf->base);
}
EXPORT_SYMBOL(webos_fence_put);

struct webos_surface *webos_get_buf(int i)
{
	return surface[i % 3];
}
EXPORT_SYMBOL(webos_get_buf);

static long webos_fence_ioctl(struct file *file, unsigned int cmd,
			      unsigned long arg)
{
	struct webos_fence_fd_info *info;
	int fd;
	
	switch (cmd) {
	case WEBOS_FENCE_IOC_FD:
		fd = get_unused_fd_flags(O_CLOEXEC);
		
		fd_install(fd, fence->file);


		break;
	}

	/* return fd of webos_fence */
}

static int buf_man_open(struct inode *inode, struct file *file)
{
	file->private_data = NULL;
	return 0;
}


static int buf_man_release(struct inode *inode, struct file *file)
{
	printk(KERN_CRIT "buf_man release\n");
	return 0;
}

static const struct file_operations buf_man_fops = {
	.owner = THIS_MODULE,
	.open = buf_man_open,
	.release = buf_man_release,
	/* .unlocked_ioctl = buf_man_ioctl, */
	/* .compat_ioctl = buf_man_ioctl, */
};

static struct miscdevice buf_man_dev = {
	.minor	= MISC_DYNAMIC_MINOR,
	.name	= "buf_man",
	.fops	= &buf_man_fops,
};

static int __init buf_man_init(void)
{
	int i;

	for (i = 0; i < 3; i++) {
		surface[i] = kzalloc(sizeof(struct webos_surface), GFP_KERNEL);
		strlcpy(surface[i]->name, "surface", sizeof("surface"));
		surface[i]->buf_size = SZ_1M * (i + 1);
		surface[i]->buf = kzalloc(surface[i]->buf_size, GFP_KERNEL);
		printk("create surface[%d]:%p buf=%p\n",
		       i, surface[i], surface[i]->buf);

		wfence[i] = kzalloc(sizeof(struct webos_fence), GFP_KERNEL);
		spin_lock_init(&wfence[i]->lock);
	
		fence_init(&wfence[i]->base,
			   &webos_fence_ops,
			   &wfence[i]->lock,
			   i, /* context=surface id */
			   0); /* sequence number of fence */

		fence_enable_sw_signaling(&wfence[i]->base);
		printk("create fence:%p context=%d id=%d flag=%lx\n",
		       wfence[i], wfence[i]->base.context,
		       wfence[i]->base.seqno, wfence[i]->base.flags);
	}

	printk("buf_man start thread\n");
	sync_thr = (struct task_struct *)kthread_run(sync_thr_func,
						     NULL,
						     "thread_sync");

	return misc_register(&buf_man_dev);
}

static void __exit buf_man_exit(void)
{
	int i;
	kthread_stop(sync_thr);

	for (i = 0; i < 3; i++) {
		/* Calling fence_put to free fence occurs kernel panic of __call_rcu */
		kfree(wfence[i]);
		kfree(surface[i]->buf);
		kfree(surface[i]);
	}

	misc_deregister(&buf_man_dev);
}

module_init(buf_man_init);
module_exit(buf_man_exit);


