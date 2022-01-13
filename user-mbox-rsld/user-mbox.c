#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/mailbox_client.h>
#include <linux/sysfs.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/wait.h>
#include <linux/poll.h>

#define UMB_CDEV "umb"
#define UMB_CLASS "umb_class"

DECLARE_WAIT_QUEUE_HEAD(wait_queue_umb_cdev);

dev_t dev = 0;
static struct class *dev_class;
struct umb_dev {
    struct cdev cdev;
    void *priv;
};
static struct umb_dev umb_cdev;

static uint32_t umb_val;
static char umb_cdev_value[11];
static int          umb_cdev_open(struct inode *inode, struct file *file);
static int          umb_cdev_release(struct inode *inode, struct file *file);
static ssize_t      umb_cdev_read(struct file *filp, char __user *buf, size_t len, loff_t *off);
static ssize_t      umb_cdev_write(struct file *filp, const char *buf, size_t len, loff_t *off);
static unsigned int umb_cdev_poll(struct file *filp, struct poll_table_struct *wait);

static struct file_operations fops =
{
    .owner          = THIS_MODULE,
    .read           = umb_cdev_read,
    .write          = umb_cdev_write,
    .open           = umb_cdev_open,
    .release        = umb_cdev_release,
    .poll           = umb_cdev_poll
};

struct mbox_info {
    struct device *dev;
    struct mbox_client tx_mc;
    struct mbox_client rx_mc;
    struct mbox_chan *tx_chan;
    struct mbox_chan *rx_chan;
    struct work_struct workqueue;
    atomic_t remote_notify;
};

static int umb_cdev_uevent(struct device *dev, struct kobj_uevent_env *env)
{
	(void)dev;
    add_uevent_var(env, "DEVMODE=%#o", 0666);
    return 0;
}

static int umb_cdev_open(struct inode *inode, struct file *filp)
{
	struct umb_dev *ud;
    ud = container_of(inode->i_cdev, struct umb_dev, cdev);
    filp->private_data = ud;
    return 0;
}

static int umb_cdev_release(struct inode *inode, struct file *filp)
{
	(void)inode;
	(void)filp;
    return 0;
}

static ssize_t umb_cdev_read(struct file *filp, char __user *buf, size_t len, loff_t *off)
{
	(void)filp;
	(void)buf;
	(void)off;
    len = strlen(umb_cdev_value);

    if (copy_to_user(buf, umb_cdev_value, len) > 0) {
        return -EFAULT;
    }

    return len;
}

static ssize_t umb_cdev_write(struct file *filp, const char __user *buf, size_t len, loff_t *off)
{
	struct mbox_info *info;
	struct umb_dev *ud;
	
	(void)buf;
	(void)len;
	(void)off;
	ud = (struct umb_dev *)filp->private_data;
	info = (struct mbox_info *)ud->priv;
	(void)mbox_send_message(info->tx_chan, NULL); /* empty msg */

	return 0;
}

static unsigned int umb_cdev_poll(struct file *filp, struct poll_table_struct *wait)
{
    __poll_t mask = 0;

    poll_wait(filp, &wait_queue_umb_cdev, wait);

    if (umb_val) {
        umb_val--;
        sprintf(umb_cdev_value, "%d", umb_val);
        mask |= (POLLIN | POLLRDNORM);
    }

    return mask;
}

static int umb_cdev_init(void *priv)
{
    if ((alloc_chrdev_region(&dev, 0, 1, UMB_CDEV)) < 0) {
        pr_err("alloc_chrdev_region failed\n");
        return -1;
    }

    cdev_init(&umb_cdev.cdev, &fops);
    umb_cdev.cdev.owner = THIS_MODULE;
    umb_cdev.cdev.ops = &fops;
    umb_cdev.priv = priv;

    if ((cdev_add(&umb_cdev.cdev, dev, 1)) < 0) {
        pr_err("cdev_add failed\n");
        goto err_class;
    }

    if ((dev_class = class_create(THIS_MODULE, UMB_CLASS)) == NULL) {
        pr_err("class_create failed\n");
        goto err_class;
    }
	dev_class->dev_uevent = umb_cdev_uevent;

    if ((device_create(dev_class, NULL, dev, NULL, UMB_CDEV)) == NULL) {
        pr_err("device_create failed\n");
        goto err_device;
    }

    init_waitqueue_head(&wait_queue_umb_cdev);

    return 0;

err_device:
    class_destroy(dev_class);
err_class:
    unregister_chrdev_region(dev, 1);

    return -1;
}

static void umb_cdev_remove(void)
{
    device_destroy(dev_class, dev);
    class_destroy(dev_class);
    cdev_del(&umb_cdev.cdev);
    unregister_chrdev_region(dev, 1);
}

static void handle_event(struct work_struct *work)
{
    struct mbox_info  *info;

    info = container_of(work, struct mbox_info, workqueue);
    (void)mbox_send_message(info->rx_chan, NULL);

    umb_val++;
    sprintf(umb_cdev_value, "%u", umb_val);
    wake_up(&wait_queue_umb_cdev);
}


static void mbox_rx_cb(struct mbox_client *cl, void *msg)
{
    struct mbox_info *info;

    info  = container_of(cl, struct mbox_info, rx_mc);
    atomic_set(&info->remote_notify, 1);
    (void)msg;
    schedule_work(&info->workqueue);
}

static void mbox_tx_done(struct mbox_client *cl, void *msg, int r)
{
	(void)cl;
	(void)msg;
	(void)r;
}

static int mbox_probe(struct platform_device *pdev)
{
    struct device *dev = &pdev->dev;
    struct mbox_client *mclient;
    struct mbox_info *info;

    dev_err(dev, "%s()\n", __FUNCTION__);
    if (!of_get_property(dev->of_node, "mboxes", NULL))
        {
        dev_err(dev, "no mboxes\n");
        return -ENOSYS;
        }

    info = devm_kzalloc(dev, sizeof(*info), GFP_KERNEL);
    if (!info)
            return -ENOMEM;

    platform_set_drvdata(pdev, info);
    info->dev = dev;

    /* Setup TX mailbox channel client */
    mclient = &info->tx_mc;
    mclient->dev = dev;
    mclient->rx_callback = NULL;
    mclient->tx_block = false;
    mclient->knows_txdone = false;
    mclient->tx_done = mbox_tx_done;

    /* Setup RX mailbox channel client */
    mclient = &info->rx_mc;
    mclient->dev = dev;
    mclient->rx_callback = mbox_rx_cb;
    mclient->tx_block = false;
    mclient->knows_txdone = false;

    INIT_WORK(&info->workqueue, handle_event);

    info->rx_chan = mbox_request_channel_byname(mclient, "rx");
    if (IS_ERR(info->rx_chan)) {
        dev_err(dev, "Failed to allocate rx channel\n");
        info->rx_chan = NULL;
        return -EINVAL;
    }

    info->tx_chan = mbox_request_channel_byname(mclient, "tx");
    if (IS_ERR(info->tx_chan)) {
        dev_err(dev, "Failed to allocate tx channel\n");
        info->tx_chan = NULL;
        return -EINVAL;
    }

	umb_cdev_init(info);

    return 0;
}


static int mbox_remove(struct platform_device *pdev)
{
    struct device *dev = &pdev->dev;
    struct mbox_info *info = platform_get_drvdata(pdev);
    dev_err(dev, "%s()\n", __FUNCTION__);
    
    if (info->rx_chan != NULL) {
        mbox_free_channel(info->rx_chan);
        info->rx_chan = NULL;
        }
    if (info->tx_chan != NULL) {
        mbox_free_channel(info->tx_chan);
        info->tx_chan = NULL;
        }
   
    umb_cdev_remove();

    return 0;
}

static const struct of_device_id user_mbox_of_match [] = {
    { .compatible = "user-mailbox" },
    { /* End of list */ }
};

MODULE_DEVICE_TABLE(of, user_mbox_of_match);

static struct platform_driver user_mbox_driver = {
    .driver = {
        .name = "user-mailbox",
        .of_match_table = user_mbox_of_match,
    },
    .probe = mbox_probe,
    .remove = mbox_remove,
};

module_platform_driver(user_mbox_driver);

MODULE_DESCRIPTION("IPI test kernel module");
MODULE_AUTHOR("Vlad Lungu <vlad.lungu@windriver.com>");
MODULE_LICENSE("GPL v2");
