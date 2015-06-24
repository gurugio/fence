
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

#include <sw_sync.h>
#include <sync.h>

MODULE_LICENSE("GPL");

struct and_sync_data
{
	struct sw_sync_timeline *timeline;
	int fence_fd;
	struct sync_fence *fence;
	struct sw_sync_create_fence_data data;
	struct sync_pt *pt;
};


void waiter_callback(struct sync_fence *fence,
		     struct sync_fence_waiter *waiter)
{
	printk("\n\n================== waiter callback ===============\n\n");

}


static int and_sync_open(struct inode *inode, struct file *file)
{
	char time_value[10];
	int i;
	struct and_sync_data *and_sync_data;
	struct sync_fence_waiter waiter;

	and_sync_data = kzalloc(sizeof(struct and_sync_data), GFP_KERNEL);

	and_sync_data->timeline = sw_sync_timeline_create("and_sync_timeline");
	if (and_sync_data->timeline == NULL) {
		printk(KERN_CRIT "fail to create timeline\n");
		return -ENOMEM;
	}

	and_sync_data->timeline->obj.ops->timeline_value_str(&and_sync_data->timeline->obj,
							     time_value, 10);
	printk(KERN_CRIT "start timeline:%s\n", time_value);
	printk("timeline:ref=%d\n", atomic_read(&and_sync_data->timeline->obj.kref.refcount));

	
	and_sync_data->pt = sw_sync_pt_create(and_sync_data->timeline, 5);
	if (and_sync_data->pt == NULL) {
		printk(KERN_CRIT "fail to create pt\n");
		return -ENOMEM;
	}
	and_sync_data->timeline->obj.ops->pt_value_str(and_sync_data->pt, time_value, 10);

	and_sync_data->fence = sync_fence_create("and_sync_fence", and_sync_data->pt);
	if (and_sync_data->fence == NULL) {
		sync_pt_free(and_sync_data->pt);
		return -ENOMEM;
	}
	printk(KERN_CRIT "fence:%p name=%s status=%d\n",
	       and_sync_data->fence, and_sync_data->fence->name,
	       atomic_read(&and_sync_data->fence->status));
	printk("timeline:ref=%d\n", atomic_read(&and_sync_data->timeline->obj.kref.refcount));
	printk("fence:ref=%d\n", atomic_read(&and_sync_data->fence->kref.refcount));

	for (i = 0; i < and_sync_data->fence->num_fences; ++i) {
		struct sync_pt *pt =
			container_of(and_sync_data->fence->cbs[i].sync_pt,
				     struct sync_pt, base);
		struct sync_timeline *parent = sync_pt_parent(pt);
		
		/* sync_print_pt(s, pt, true); */
		parent->ops->pt_value_str(and_sync_data->pt, time_value, 10);
		printk(KERN_CRIT "  pt:timeline-name=%s value=%s\n",
		       parent->name, time_value);
	}

	sync_fence_waiter_init(&waiter, waiter_callback);
	sync_fence_wait_async(and_sync_data->fence, &waiter);		       

	
	for (i = 0; i < 10; i++) {
		sw_sync_timeline_inc(and_sync_data->timeline, 1);

		and_sync_data->timeline->obj.ops->timeline_value_str(&and_sync_data->timeline->obj,
								     time_value, 10);
		printk("timeline:%s\n", time_value);
		ssleep(1);
	}

	printk("open finish\n");
	file->private_data = and_sync_data;
	return 0;
}

/* static void __exit and_sync_exit(void) */
static int and_sync_release(struct inode *inode, struct file *file)
{
	char time_value[10];
	struct and_sync_data *and_sync_data = file->private_data;
	int i;
	
	printk(KERN_CRIT "start exit\n");


	and_sync_data->timeline->obj.ops->timeline_value_str(&and_sync_data->timeline->obj,
							     time_value, 10);
	printk("finish timeline:%s\n", time_value);

	for (i = 0; i < and_sync_data->fence->num_fences; ++i) {
		struct sync_pt *pt =
			container_of(and_sync_data->fence->cbs[i].sync_pt,
				     struct sync_pt, base);
		struct sync_timeline *parent = sync_pt_parent(pt);
		
		/* sync_print_pt(s, pt, true); */
		parent->ops->pt_value_str(pt, time_value, 10);
		printk(KERN_CRIT "  pt[%p][%d]:timeline-name=%s value=%s\n",
		       pt, i, parent->name, time_value);
	}

	sync_timeline_destroy(&and_sync_data->timeline->obj);

	printk(KERN_CRIT "and_sync_exit\n");
	
        return 0;
}


static const struct file_operations and_sync_fops = {
	.owner = THIS_MODULE,
	.open = and_sync_open,
	.release = and_sync_release,
	/* .unlocked_ioctl = and_sync_ioctl, */
	/* .compat_ioctl = and_sync_ioctl, */
};

static struct miscdevice and_sync_dev = {
	.minor	= MISC_DYNAMIC_MINOR,
	.name	= "and_sync",
	.fops	= &and_sync_fops,
};

static int __init and_sync_init(void)
{
	printk(KERN_CRIT "\n\n================== and_sync init\n\n");
	return misc_register(&and_sync_dev);
}

static void __exit and_sync_exit(void)
{
	misc_deregister(&and_sync_dev);
}


module_init(and_sync_init);
module_exit(and_sync_exit);


