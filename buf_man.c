
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

#include "webos_fence_uapi.h"

MODULE_LICENSE("GPL");


struct webos_fence
{
	struct fence base;
	spinlock_t lock;
	struct rcu_head rcu;

	/* for process */
	int fd;
	struct file *file;
};

static atomic_t webos_fence_index = ATOMIC_INIT(0);

void webos_fence_ready(struct webos_fence *wf);

static int webos_fence_usync_release(struct inode *inode, struct file *file)
{
	struct webos_fence *wf = file->private_data;

	/* printk("webos_fence_usync_release:fd=%d reqno=%d\n", wf->fd, wf->base.seqno); */
	wf->fd = -1;
	wf->file = NULL;
	return 0;
}

static long webos_fence_usync_ioctl(struct file *file, unsigned int cmd,
			     unsigned long arg)
{
	struct webos_fence *wf = file->private_data;
	struct webos_fence_wait_info wait_info;
	long ret = 0;

	switch (cmd) {
	case WEBOS_FENCE_IOC_WAIT:
		if (copy_from_user(&wait_info,
				   (void __user *)arg,
				   sizeof(wait_info)))
			wait_info.timeout = 10; /* default */

		/* printk("webos_fence_usync_ioctl-wait:%d %p\n", wf->base.seqno, wf); */
		fence_wait_timeout(&wf->base, true, wait_info.timeout * HZ);
		/* printk("meet fence:%d %p\n", wf->base.seqno, wf); */
		break;
	case WEBOS_FENCE_IOC_READY:
		webos_fence_ready(wf);
		break;
	case WEBOS_FENCE_IOC_SIGNAL:
		fence_signal(&wf->base);
		break;
	default:
		return -EINVAL;
	}

	return ret;
}

static const struct file_operations webos_fence_usync_fops = {
	.release = webos_fence_usync_release,
	.unlocked_ioctl = webos_fence_usync_ioctl,
	.compat_ioctl = webos_fence_usync_ioctl,
};

static void webos_fence_release(struct fence *fence)
{
	struct webos_fence *wf = container_of(fence,
					      struct webos_fence, base);
	/* free webos_fence */
	printk("release fence=%p seq=%d\n", fence, fence->seqno);
	kfree_rcu(wf, rcu);
}

static bool webos_fence_enable_signaling(struct fence *fence)
{
	struct webos_fence *wf = container_of(fence,
					      struct webos_fence, base);
	webos_fence_ready(wf);
	return true;
}

static const char *webos_fence_get_driver_name(struct fence *fence)
{
	return "webos_fence_v0.1";
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
	.release = webos_fence_release,
};

void webos_fence_ready(struct webos_fence *wf)
{
	/*
	 * fence_enable_sw_signaling cannot enable signal
	 * if SIGNALED_BIT is already set.
	 */
	clear_bit(FENCE_FLAG_SIGNALED_BIT, &wf->base.flags);
}
EXPORT_SYMBOL(webos_fence_ready);

void webos_fence_get(struct webos_fence *wf)
{
	fence_get(&wf->base);
}
EXPORT_SYMBOL(webos_fence_get);

void webos_fence_put(struct webos_fence *wf)
{
	fence_put(&wf->base);
}
EXPORT_SYMBOL(webos_fence_put);

struct webos_fence *webos_create_fence(unsigned int context, unsigned int seq)
{
	struct webos_fence *wf;

	wf = kmalloc(sizeof(struct webos_fence), GFP_KERNEL);
	if (!wf)
		return NULL;

	spin_lock_init(&wf->lock);
	wf->fd = -1;
	wf->file = NULL;

	fence_init(&wf->base,
		   &webos_fence_ops,
		   &wf->lock,
		   /* context: id of something that fence is included */
		   context,
		   /* seq: id of fence */
		   seq);
	fence_enable_sw_signaling(&wf->base);
	/* printk("create-fence:%d %d\n", wf->base.context, wf->base.seqno); */
	return wf;
}
EXPORT_SYMBOL(webos_create_fence);

static long buf_man_ioctl(struct file *file, unsigned int cmd,
			      unsigned long arg)
{
	struct webos_fence_fd_info info;
	int fd;
	struct webos_fence *wf;
	long ret = 0;

	switch (cmd) {
	case BUF_MAN_IOC_CREATE_FENCE:
		if (copy_from_user(&info, (void __user *)arg, sizeof(info))) {
			ret = -EFAULT;
			break;
		}
		wf = webos_create_fence(info.context,
					atomic_inc_return(&webos_fence_index));
		wf->file = anon_inode_getfile("sync_fence",
					      &webos_fence_usync_fops,
					      wf,
					      0);
		if (IS_ERR(wf->file)) {
			printk("fail to create fence-fd\n");
			return -EFAULT;
		}

		fd = get_unused_fd_flags(O_CLOEXEC);
		fd_install(fd, wf->file);
		wf->fd = fd;

		printk("create user-fence:fence=%p fd=%d\n", wf, wf->fd);
		
		info.fd = wf->fd;
		info.seqno = wf->base.seqno;
		if (copy_to_user((void __user *)arg, &info, sizeof(info)))
			ret = -EFAULT;
		break;
	default:
		return -ENOTTY;
	}

	return ret;
}

static int buf_man_open(struct inode *inode, struct file *file)
{
	file->private_data = NULL;
	printk(KERN_CRIT "buf_man open\n");
	return 0;
}


static int buf_man_release(struct inode *inode, struct file *file)
{
	printk(KERN_CRIT "buf_man release\n");

	/* TODO: force to free every fence and resources */
	/* close means this process is not using fence anymore */
	/* For that, fence list is necessary. */
	return 0;
}

static const struct file_operations buf_man_fops = {
	.owner = THIS_MODULE,
	.open = buf_man_open,
	.release = buf_man_release,
	.unlocked_ioctl = buf_man_ioctl,
	.compat_ioctl = buf_man_ioctl,
};

static struct miscdevice buf_man_dev = {
	.minor	= MISC_DYNAMIC_MINOR,
	.name	= "webos_buf_man",
	.fops	= &buf_man_fops,
};

static int __init buf_man_init(void)
{
	printk("webos_buf_man start\n");
	return misc_register(&buf_man_dev);
}

static void __exit buf_man_exit(void)
{
	misc_deregister(&buf_man_dev);
	printk("webos_buf_man end\n");
}

module_init(buf_man_init);
module_exit(buf_man_exit);


