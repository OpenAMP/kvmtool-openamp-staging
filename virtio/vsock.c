#include "kvm/virtio-vsock.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/kvm.h>
#include <linux/vhost.h>
#include <linux/virtio_config.h>
#include <linux/list.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>

#include "kvm/guest_compat.h"
#include "kvm/kvm.h"
#include "kvm/virtio.h"
#include "kvm/virtio-pci-dev.h"
#include "kvm/util.h"

#define VIRTIO_VSOCK_QUEUE_SIZE 256
#define VIRTIO_VSOCK_NUM_QUEUES 3

static int compat_id = -1;
static struct vsock_dev *g_vdev = NULL;

struct virtio_vsock_config {
	u64 guest_cid;
};

struct vsock_dev {
	struct virt_queue vqs[VIRTIO_VSOCK_NUM_QUEUES];
	struct virtio_vsock_config config;

	u64 features;
	int vhost_fd;
	u8 status;

	struct virtio_device dev;
	struct kvm *kvm;
};

static u8 *get_config(struct kvm *kvm, void *dev) {
	struct vsock_dev *vdev = dev;

	return ((u8*)(&vdev->config));
}

static u32 get_host_features(struct kvm *kvm, void *dev) {
	struct vsock_dev *vdev = dev;
	return vdev->features;
}

static void set_guest_features(struct kvm *kvm, void *dev, u32 features) {
	struct vsock_dev *vdev = dev;

	u64 nfeatures = features;
	if (ioctl(vdev->vhost_fd, VHOST_SET_FEATURES, &nfeatures) != 0) {
		pr_err("Unable to set vhost features for virtio-vsock");
		return;
	}

	vdev->features = features;
}

static void notify_status(struct kvm *kvm, void *dev, u32 status) {
	struct vsock_dev *vdev = dev;
	if ((vdev->status & VIRTIO_CONFIG_S_DRIVER_OK) == 0 &&
	    (status & VIRTIO_CONFIG_S_DRIVER_OK) != 0) {
		// The driver was just enabled.
		int on = 1;
		if (ioctl(vdev->vhost_fd, VHOST_VSOCK_SET_RUNNING, &on) != 0)
			die_perror("VHOST_VSOCK_SET_RUNNING failed");
	}

	if ((vdev->status & VIRTIO_CONFIG_S_DRIVER_OK) != 0 &&
	    (status & VIRTIO_CONFIG_S_DRIVER_OK) == 0) {
		// The driver was just disabled.
		int off = 0;
		if (ioctl(vdev->vhost_fd, VHOST_VSOCK_SET_RUNNING, &off) != 0)
			die_perror("VHOST_VSOCK_SET_RUNNING failed");
	}

	vdev->status = status;
}

static int init_vq(struct kvm *kvm, void *dev, u32 vq, u32 page_size, u32 align,
		u32 pfn) {
	compat__remove_message(compat_id);

	int ret = 0;
	void *p = NULL;
	struct vsock_dev *vdev = dev;
	struct virt_queue *queue = &vdev->vqs[vq];

	queue->pfn = pfn;
	p = virtio_get_vq(kvm, queue->pfn, page_size);

	vring_init(&queue->vring, VIRTIO_VSOCK_QUEUE_SIZE, p, align);

	if (vq > 1) {
		// TODO(chirantan): Implement the event virtqueue
		return 0;
	}

	struct vhost_vring_state state = {
		.index = vq,
		.num = queue->vring.num,
	};
	if (ioctl(vdev->vhost_fd, VHOST_SET_VRING_NUM, &state) != 0) {
		ret = -errno;
		pr_err("VHOST_SET_VRING_NUM failed for vsock device: %d", ret);
		return ret;
	}

	state.num = 0;
	if (ioctl(vdev->vhost_fd, VHOST_SET_VRING_BASE, &state) != 0) {
		ret = -errno;
		pr_err("VHOST_SET_VRING_BASE failed for vsock device: %d", ret);
		return ret;
	}

	struct vhost_vring_addr addr = {
		.index = vq,
		.desc_user_addr = (u64)queue->vring.desc,
		.avail_user_addr = (u64)queue->vring.avail,
		.used_user_addr = (u64)queue->vring.used,
	};
	if (ioctl(vdev->vhost_fd, VHOST_SET_VRING_ADDR, &addr) != 0) {
		ret = -errno;
		pr_err("VHOST_SET_VRING_ADDR failed for vsock device: %d", ret);
		return ret;
	}

	return 0;
}

static void notify_vq_gsi(struct kvm *kvm, void *dev, u32 vq, u32 gsi) {
	if (vq > 1) {
		// TODO(chirantan): Implement the event virtqueue
		return;
	}
	int fd = eventfd(0, 0);
	if (fd < 0) {
		// No graceful way to exit here.
		die_perror("Unable to create eventfd");
	}

	struct kvm_irqfd irq = {
		.gsi = gsi,
		.fd = fd,
	};
	if (ioctl(kvm->vm_fd, KVM_IRQFD, &irq) != 0)
		die_perror("KVM_IRQFD failed for vsock device");

	struct vhost_vring_file file = {
		.index = vq,
		.fd = irq.fd,
	};
	struct vsock_dev *vdev = dev;
	if (ioctl(vdev->vhost_fd, VHOST_SET_VRING_CALL, &file) != 0)
		die_perror("VHOST_SET_VRING_CALL failed for vsock device");
}

static void notify_vq_eventfd(struct kvm *kvm, void *dev, u32 vq, u32 efd) {
	if (vq > 1) {
		// TODO(chirantan): Implement the event virtqueue
		return;
	}
	struct vsock_dev *vdev = dev;
	struct vhost_vring_file file = {
		.index = vq,
		.fd = efd,
	};

	if (ioctl(vdev->vhost_fd, VHOST_SET_VRING_KICK, &file) != 0)
		die_perror("VHOST_VRING_SET_KICK failed for vsock device");
}

static int notify_vq(struct kvm *kvm, void *dev, u32 vq)
{
	return 0;
}

static struct virt_queue *get_vq(struct kvm *kvm, void *dev, u32 vq)
{
	struct vsock_dev *vdev = dev;

	return &vdev->vqs[vq];
}

static int get_size_vq(struct kvm *kvm, void *dev, u32 vq) {
	return VIRTIO_VSOCK_QUEUE_SIZE;
}

static int set_size_vq(struct kvm *kvm, void *dev, u32 vq, int size)
{
	// Unsupported?
	return size;
}

static struct virtio_ops vsock_dev_virtio_ops = {
	.get_config = get_config,
	.get_host_features = get_host_features,
	.set_guest_features = set_guest_features,
	.init_vq = init_vq,
	.get_vq = get_vq,
	.get_size_vq = get_size_vq,
	.set_size_vq = set_size_vq,
	.notify_vq = notify_vq,
	.notify_vq_gsi = notify_vq_gsi,
	.notify_vq_eventfd = notify_vq_eventfd,
	.notify_status = notify_status,
};

int virtio_vsock_init(struct kvm *kvm) {
	int ret = 0, i;
	struct kvm_mem_bank *bank;
    struct vhost_memory *mem;

	if (kvm->cfg.guest_cid == 0)
		return 0;

	if (g_vdev != NULL) {
		pr_err("Already initialized virtio vsock once");
		return -EINVAL;
	}

	struct vsock_dev *vdev = malloc(sizeof(struct vsock_dev));
	if (vdev == NULL)
		return -ENOMEM;

	vdev->config = (struct virtio_vsock_config) {
		.guest_cid = kvm->cfg.guest_cid,
	};
	vdev->kvm = kvm;

	ret = virtio_init(kvm, vdev, &vdev->dev, &vsock_dev_virtio_ops,
			  VIRTIO_DEFAULT_TRANS(kvm), PCI_DEVICE_ID_VIRTIO_VSOCK,
			  VIRTIO_ID_VSOCK, PCI_CLASS_VSOCK);
	if (ret < 0)
		goto cleanup;

	vdev->vhost_fd = open("/dev/vhost-vsock", O_RDWR);
	if (vdev->vhost_fd < 0) {
		ret = -errno;
		pr_err("Unable to open vhost-vsock device: %d", ret);
		goto cleanup;
	}

	if (ioctl(vdev->vhost_fd, VHOST_SET_OWNER) != 0) {
		ret = -errno;
		pr_err("VHOST_SET_OWNER failed on vhost-vsock device: %d", ret);
		goto vhost_cleanup;
	}

	if (ioctl(vdev->vhost_fd, VHOST_GET_FEATURES, &vdev->features) != 0) {
		ret = -errno;
		pr_err("VHOST_GET_FEATURES failed on vhost-vsock device: %d", ret);
		goto vhost_cleanup;
	}

	mem = calloc(1, sizeof(*mem) + kvm->mem_slots * sizeof(struct vhost_memory_region));
	if (mem == NULL) {
		ret = -ENOMEM;
		goto vhost_cleanup;
	}

	i = 0;
	list_for_each_entry(bank, &kvm->mem_banks, list) {
		mem->regions[i] = (struct vhost_memory_region) {
			.guest_phys_addr = bank->guest_phys_addr,
			.memory_size	 = bank->size,
			.userspace_addr	 = (unsigned long)bank->host_addr,
		};
		i++;
	}
	mem->nregions = i;

	if (ioctl(vdev->vhost_fd, VHOST_SET_MEM_TABLE, mem) != 0) {
		ret = -errno;
		pr_err("VHOST_SET_MEM_TABLE on vhost-vsock device failed: %d", ret);
		goto vhost_mem_cleanup;
	}
	free(mem);  // sigh... manual memory management

	if (ioctl(vdev->vhost_fd, VHOST_VSOCK_SET_GUEST_CID,
		  &vdev->config.guest_cid) != 0) {
		ret = -errno;
		pr_err("VHOST_VSOCK_SET_GUEST_CID failed: %d", ret);
		goto vhost_cleanup;
	}

	vdev->dev.use_vhost = true;

	if (compat_id == -1) {
		compat_id = virtio_compat_add_message("virtio-vsock",
						      "CONFIG_VIRTIO_VSOCKETS");
	}

	g_vdev = vdev;
	return 0;

vhost_mem_cleanup:
	free(mem);
vhost_cleanup:
	close(vdev->vhost_fd);
cleanup:
	free(vdev);

	return ret;
}
virtio_dev_init(virtio_vsock_init);

int virtio_vsock_exit(struct kvm *kvm) {
	if (g_vdev == NULL)
		return 0;

	struct vsock_dev *vdev = g_vdev;
	g_vdev = NULL;

	close(vdev->vhost_fd);
	free(vdev);

	return 0;
}
virtio_dev_exit(virtio_vsock_exit);
