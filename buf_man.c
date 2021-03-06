
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
#include <linux/seq_file.h>
#include <linux/debugfs.h>

#include "webos_fence_uapi.h"

MODULE_LICENSE("GPL");

static struct webos_fence *webos_create_fence(unsigned int context, unsigned int seq);
static const struct file_operations webos_fence_usync_fops;
static const struct file_operations webos_fence_usync_fops;

#if defined(BUF_MAN_DEBUG)
# define PRINT_DEBUG(fmt, args...) printk("WEBOS-FENCE: " fmt, ##args)
#else
# define PRINT_DEBUG(fmt, args...)
#endif


#define WF_MERGED 0x1
#define WF_PARENT 0x2

struct webos_fence
{
	struct fence base;
	spinlock_t lock;
	int flags;
	struct rcu_head rcu;
	struct list_head merged;

	/* for debugfs */
	char name[64];
	struct list_head webos_fence_list;

	/* for process */
	int fd;
	struct file *file;
};

static atomic_t webos_fence_index = ATOMIC_INIT(0);
LIST_HEAD(webos_fence_list_head);
DEFINE_SPINLOCK(webos_fence_list_lock);
struct dentry *webos_fence_debugfs;

static const char *webos_fence_flags_str(int flags)
{
	switch (flags) {
	case WF_PARENT:
		return "PARENT";
	case WF_MERGED:
		return "MERGED";
	case 0:
		return "NORMAL";
	}
	return "FLAG-ERROR";
}

static void webos_fence_print_fence(struct seq_file *s, struct webos_fence *wf)
{
	struct webos_fence *merge;
	seq_printf(s, "[%p] name=%s flag=%s fd=%d\n",
		   wf,
		   wf->name,
		   webos_fence_flags_str(wf->flags),
		   wf->fd);

	if (wf->flags == WF_PARENT) {
		WARN_ON(list_empty(&wf->merged));
		list_for_each_entry(merge, &wf->merged, merged) {
			WARN_ON(merge->flags != WF_MERGED);
			seq_printf(s, "    child-[%p] name=%s flag=%s fd=%d\n",
				   merge,
				   merge->name,
				   webos_fence_flags_str(merge->flags),
				   merge->fd);
		}
	}
}

static int webos_fence_debugfs_show(struct seq_file *s, void *unused)
{
	unsigned long flags;
	struct webos_fence *wf;

	spin_lock_irqsave(&webos_fence_list_lock, flags);
	seq_puts(s, "NAME=comm:pid:context:reqno\n");
	list_for_each_entry(wf, &webos_fence_list_head, webos_fence_list) {
		webos_fence_print_fence(s, wf);
	}
	spin_unlock_irqrestore(&webos_fence_list_lock, flags);

	return 0;
}

static int webos_fence_debugfs_open(struct inode *inode, struct file *file)
{
	return single_open(file, webos_fence_debugfs_show, inode->i_private);
}

static const struct file_operations webos_fence_debugfs_fops = {
	.open           = webos_fence_debugfs_open,
	.read           = seq_read,
	.llseek         = seq_lseek,
	.release        = single_release,
};

void webos_fence_ready(struct webos_fence *wf)
{
	/*
	 * fence_enable_sw_signaling cannot enable signal
	 * if SIGNALED_BIT is already set.
	 */
	if (wf->flags == WF_PARENT) {
		struct webos_fence *merge;

		list_for_each_entry(merge, &wf->merged, merged) {
			WARN_ON(merge->flags != WF_MERGED);
			clear_bit(FENCE_FLAG_SIGNALED_BIT, &merge->base.flags);
		}
	} else if (wf->flags == 0) {
		clear_bit(FENCE_FLAG_SIGNALED_BIT, &wf->base.flags);
	} else {
		__WARN();
	}
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

int webos_fence_signal(struct webos_fence *wf)
{
	int ret = 0;

	if (wf->flags == WF_PARENT) {
		/* signal parent fence */
		struct webos_fence *merge;

		/* wait for signals of all merged fences */
		list_for_each_entry(merge, &wf->merged, merged) {
			WARN_ON(merge->flags != WF_MERGED);
			PRINT_DEBUG("signal-child: %p\n", merge);
			ret = fence_signal(&merge->base);
			if (ret < 0)
				PRINT_DEBUG("ERROR: fail to signal fence:%p\n", merge);
		}
	} else if (wf->flags == 0) {
		/* signal normal or merged fence */
		ret = fence_signal(&wf->base);
	} else {
		/* merged fence cannot be visible to user */
		__WARN();
	}
	return ret;
}
EXPORT_SYMBOL(webos_fence_signal);

int webos_fence_wait(struct webos_fence *wf, int timeout_sec)
{
	long timeout;
	int ret = 0;

	if (wf->flags == WF_PARENT) {
		struct webos_fence *merge, *tmp;

		/* wait for signals of all merged fences */
		list_for_each_entry_safe(merge, tmp, &wf->merged, merged) {
			WARN_ON(merge->flags != WF_MERGED);

			PRINT_DEBUG("wait-child: %p\n", merge);
			timeout = fence_wait_timeout(&merge->base,
						     true, timeout_sec * HZ);
			PRINT_DEBUG("wait-ret=%ld\n", timeout);
			if (timeout <= 0) {
				ret = -ETIME;
				break;
			}
		}
	} else if (wf->flags == 0) {
		timeout = fence_wait_timeout(&wf->base, true, timeout_sec * HZ);
		if (timeout <= 0)
			ret = -ETIME;
	} else {
		/* merged fence cannot be visible to user */
		__WARN();
	}
	return ret;
}
EXPORT_SYMBOL(webos_fence_wait);

/* This is called when user close the fd of fence.
 * It means the user will not use the fence anymore,
 * so the fence is released by fence_put().
 * If the fence ref-count is zero, it is freed by "struct fence_ops->release"
 */
static int webos_fence_usync_release(struct inode *inode, struct file *file)
{
	struct webos_fence *wf = file->private_data;
	struct webos_fence *merge, *tmp;

	if (wf->flags == WF_MERGED)
		PRINT_DEBUG("%s: merged-fence: fd=%d reqno=%d\n",
			    __FUNCTION__, wf->fd, wf->base.seqno);
	else if (wf->flags == WF_PARENT)
		PRINT_DEBUG("%s: parent-fence: fd=%d reqno=%d\n",
			    __FUNCTION__, wf->fd, wf->base.seqno);
	else if (wf->flags == 0)
		PRINT_DEBUG("%s: normal-fence: fd=%d reqno=%d\n",
			    __FUNCTION__, wf->fd, wf->base.seqno);
	else
		PRINT_DEBUG("%s: FLAG-ERROR: fd=%d reqno=%d\n",
			    __FUNCTION__, wf->fd, wf->base.seqno);


	/* file is closed */
	wf->fd = -1;
	wf->file = NULL;

	if (wf->flags == WF_PARENT) {
		list_for_each_entry_safe(merge, tmp, &wf->merged, merged) {
			WARN_ON(merge->flags != WF_MERGED);
			list_del(&merge->merged);
			fence_put(&merge->base);
		}
		fence_put(&wf->base);
	} else if (wf->flags == WF_MERGED) {
		/* merged fence is freed when the parent fence is freed */
		PRINT_DEBUG("not freed: prev=%p next=%p\n", wf->merged.prev, wf->merged.next);
	} else if (wf->flags == 0) {
		fence_put(&wf->base);
	} else {
		__WARN();
	}

	return 0;
}

struct webos_fence *webos_fence_fdget(int fd)
{
	struct file *file = fget(fd);

	if (file == NULL)
		return NULL;

	if (file->f_op != &webos_fence_usync_fops)
		goto err;

	return file->private_data;

err:
	fput(file);
	return NULL;
}

static long webos_fence_merge(struct webos_fence *wf, struct webos_fence_merge_info *info)
{
	struct webos_fence *new_wf;
	struct webos_fence *merge_wf;
	int fd;

	/* create new fence */
	new_wf = webos_create_fence(wf->base.context,
				    atomic_inc_return(&webos_fence_index));
	new_wf->file = anon_inode_getfile("webos_sync_fence",
				      &webos_fence_usync_fops,
				      new_wf,
				      0);
	if (IS_ERR(new_wf->file)) {
		PRINT_DEBUG("fail to create fence-fd\n");
		return -EFAULT;
	}

	fd = get_unused_fd_flags(O_CLOEXEC);
	fd_install(fd, new_wf->file);
	new_wf->fd = fd;
	new_wf->flags |= WF_PARENT;

	PRINT_DEBUG("create parent-fence:fence=%p fd=%d\n", new_wf, new_wf->fd);

	info->fence = new_wf->fd;
	info->seqno = new_wf->base.seqno;

	if (wf->flags == WF_PARENT) {
		/* children are moved into new-parent
		 * and parent becomes normal fence
		 */
		wf->flags = 0;
		list_splice(&wf->merged, &new_wf->merged);
		{
			struct webos_fence *merge;

			/* wait for signals of all merged fences */
			list_for_each_entry(merge, &new_wf->merged, merged) {
				PRINT_DEBUG("new-wf-child: %p\n", merge);
			}
		}
	} else if (wf->flags == 0x0) {
		wf->flags = WF_MERGED;
		list_add_tail(&wf->merged, &new_wf->merged);
		PRINT_DEBUG("merge1:%p\n", wf);
	} else {
		/* merged fence cannot be visible to user */
		__WARN();
	}


	merge_wf = webos_fence_fdget(info->fd2);
	if (merge_wf->flags == WF_PARENT) {
		merge_wf->flags = WF_MERGED;
		list_splice(merge_wf->merged.next, &new_wf->merged);
		{
			struct webos_fence *merge;

			/* wait for signals of all merged fences */
			list_for_each_entry(merge, &new_wf->merged, merged) {
				PRINT_DEBUG("move-child: %p\n", merge);
			}
		}
	} else if (merge_wf->flags == 0x0) {
		merge_wf->flags = WF_MERGED;
		list_add_tail(&merge_wf->merged, &new_wf->merged);
		PRINT_DEBUG("merge2:%p\n", merge_wf);
	} else {
		__WARN();
	}
	/*
	 * webos_fence_fdget gets file of merge_wf,
	 * so it needs to put.
	 */
	fput(merge_wf->file);

	return 0;
}

/* ioctl of fence-fd */
static long webos_fence_usync_ioctl(struct file *file, unsigned int cmd,
			     unsigned long arg)
{
	struct webos_fence *wf = file->private_data;
	struct webos_fence_wait_info wait_info;
	struct webos_fence_merge_info merge_info;
	long ret = 0;

	switch (cmd) {
	case WEBOS_FENCE_IOC_WAIT:
		if (copy_from_user(&wait_info,
				   (void __user *)arg,
				   sizeof(wait_info)))
			wait_info.timeout = 10; /* default */

		ret = webos_fence_wait(wf, wait_info.timeout);
		break;
	case WEBOS_FENCE_IOC_READY:
		webos_fence_ready(wf);
		break;
	case WEBOS_FENCE_IOC_SIGNAL:
		ret = webos_fence_signal(wf);
		break;
	case WEBOS_FENCE_IOC_MERGE:
		if (copy_from_user(&merge_info,
				   (void __user *)arg,
				   sizeof(merge_info))) {
			ret = -EFAULT;
			break;
		}
		webos_fence_merge(wf, &merge_info);
		if (copy_to_user((void __user *)arg,
				 &merge_info,
				 sizeof(merge_info))) {
			ret = -EFAULT;
			break;
		}
		break;
	default:
		return -EINVAL;
	}

	return ret;
}

/* operations of fence-fd */
static const struct file_operations webos_fence_usync_fops = {
	.release = webos_fence_usync_release,
	.unlocked_ioctl = webos_fence_usync_ioctl,
	.compat_ioctl = webos_fence_usync_ioctl,
};

static void webos_fence_release(struct fence *fence)
{
	unsigned long flags;
	struct webos_fence *wf = container_of(fence,
					      struct webos_fence, base);

	PRINT_DEBUG("%s: release fence=%p seqno=%d\n", __FUNCTION__, fence, fence->seqno);
	spin_lock_irqsave(&webos_fence_list_lock, flags);
	list_del(&wf->webos_fence_list);
	spin_unlock_irqrestore(&webos_fence_list_lock, flags);

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

struct webos_fence *webos_create_fence(unsigned int context, unsigned int seq)
{
	struct webos_fence *wf;
	unsigned long flags;

	wf = kmalloc(sizeof(struct webos_fence), GFP_KERNEL);
	if (!wf)
		return NULL;

	wf->flags = 0;
	spin_lock_init(&wf->lock);
	wf->fd = -1;
	wf->file = NULL;
	INIT_LIST_HEAD(&wf->webos_fence_list);
	INIT_LIST_HEAD(&wf->merged);

	fence_init(&wf->base,
		   &webos_fence_ops,
		   &wf->lock,
		   /* context: id of something that fence is included */
		   context,
		   /* seq: id of fence */
		   seq);
	fence_enable_sw_signaling(&wf->base);

	snprintf(wf->name, 64, "%s:%d:%d:%d",
		 current->comm, (int)(current->pid), context, seq);

	spin_lock_irqsave(&webos_fence_list_lock, flags);
	list_add_tail(&wf->webos_fence_list, &webos_fence_list_head);
	spin_unlock_irqrestore(&webos_fence_list_lock, flags);

	return wf;
}
EXPORT_SYMBOL(webos_create_fence);

/* ioctl of /dev/buf_man */
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
		wf->file = anon_inode_getfile("webos_sync_fence",
					      &webos_fence_usync_fops,
					      wf,
					      0);
		if (IS_ERR(wf->file)) {
			PRINT_DEBUG("fail to create fence-fd\n");
			return -EFAULT;
		}

		fd = get_unused_fd_flags(O_CLOEXEC);
		fd_install(fd, wf->file);
		wf->fd = fd;

		PRINT_DEBUG("create user-fence:fence=%p fd=%d\n", wf, wf->fd);
		
		info.fd = wf->fd;
		info.seqno = wf->base.seqno;
		if (copy_to_user((void __user *)arg, &info, sizeof(info))) {
			put_unused_fd(wf->fd);
			fput(wf->file);
			webos_fence_release(&wf->base);
			ret = -EFAULT;
		}
		break;
	default:
		return -ENOTTY;
	}

	return ret;
}

static int buf_man_open(struct inode *inode, struct file *file)
{
	file->private_data = NULL;
	PRINT_DEBUG(KERN_CRIT "buf_man open\n");
	return 0;
}


static int buf_man_release(struct inode *inode, struct file *file)
{
	PRINT_DEBUG(KERN_CRIT "buf_man release\n");
	return 0;
}

/* operations of /dev/buf_man */
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
	PRINT_DEBUG("webos_buf_man start\n");
	webos_fence_debugfs = debugfs_create_file("webos_fence", S_IRUGO,
						  NULL, NULL,
						  &webos_fence_debugfs_fops);
	return misc_register(&buf_man_dev);
}

static void __exit buf_man_exit(void)
{
	struct webos_fence *wf, *tmp;

	WARN_ON(!list_empty(&webos_fence_list_head));
	list_for_each_entry_safe(wf, tmp,
				 &webos_fence_list_head, webos_fence_list) {
		fence_put(&wf->base);
	}

	misc_deregister(&buf_man_dev);
	debugfs_remove(webos_fence_debugfs);
	PRINT_DEBUG("webos_buf_man end\n");
}

module_init(buf_man_init);
module_exit(buf_man_exit);


