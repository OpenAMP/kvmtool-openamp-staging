#ifndef KVM__VIRTIO_MMIO_H
#define KVM__VIRTIO_MMIO_H

#include <linux/types.h>
#include <linux/virtio_mmio.h>

#define VIRTIO_MMIO_MAX_VQ	 	32
#define VIRTIO_MMIO_MAX_CONFIG	 	1
#define VIRTIO_MMIO_IO_SIZE	 	0x200
#define VIRTIO_MMIO_MAX_FEATURE_SEL	2

#define VIRTIO_MMIO_STAT_QUEUE_NOTIFY	0
#define VIRTIO_MMIO_STAT_TRAP_IN	1
#define VIRTIO_MMIO_STAT_TRAP_OUT	2
#define VIRTIO_MMIO_STAT_ACK_IRQ	3
#define VIRTIO_MMIO_STAT_CHECK_IRQ	4
#define VIRTIO_MMIO_STAT_QUEUE_SEL	5
#define VIRTIO_MMIO_STAT_MSI_CMD	6
#define VIRTIO_MMIO_STAT_MSI_MASK	7
#define VIRTIO_MMIO_STAT_SIG_MSI	8
#define VIRTIO_MMIO_STAT_SIG_IRQ	9
#define VIRTIO_MMIO_STAT_MAX		10

#define VIRTIO_MMIO_F_SIGNAL_MSI 	(1 << 0)

struct kvm;

struct virtio_mmio_ioevent_param {
	struct virtio_device	*vdev;
	u32			vq;
};

union virtio_mmio_notify {
	struct {
	u32			notify_base:16;
	u32			notify_multiplier:16;
	} fields;
	u32 word;
};

struct virtio_mmio_hdr {
	char	magic[4];
	u32	version;
	u32	device_id;
	u32	vendor_id;
	u32	host_features;
	u32	host_features_sel;
	u32	reserved_1[2];
	u32	guest_features;
	u32	guest_features_sel;
	u32	guest_page_size;
	u32	reserved_2;
	u32	queue_sel;
	u32	queue_num_max;
	u32	queue_num;
	u32	queue_align;
	u32	queue_pfn;
	u32	reserved_3[3];
	u32	queue_notify;
	u32	reserved_4[3];
	u32	interrupt_state;
	u32	interrupt_ack;
	u32	reserved_5[2];
	u32	status;
        u32     reserved_6[19];
        u32     msi_vec_num;
        u32     msi_state;
        u32     msi_cmd;
        u32     reserved_7;
        u32     msi_vec_sel;
        u32     msi_addr_lo;
        u32     msi_addr_hi;
        u32     msi_data;
} __attribute__((packed));

struct virtio_mmio {
	struct virtio_mmio_hdr	hdr;
	struct device_header	dev_hdr;
	struct list_head        list;
	void			*dev;
	struct kvm		*kvm;

	u32			addr;
	u8			irq;
        u32                     features;
	u32                     host_features[VIRTIO_MMIO_MAX_FEATURE_SEL];
	u32			guest_features[VIRTIO_MMIO_MAX_FEATURE_SEL];
	u32			notify_offset;
	
	/* Stats */

	u32			stats[VIRTIO_MMIO_STAT_MAX];
	u32			ioevents[VIRTIO_MMIO_MAX_VQ];

        /* MSI */

        u32                     config_vector;
	u32                     config_gsi;
        u32                     vq_vector[VIRTIO_MMIO_MAX_VQ];
	u32                     gsis[VIRTIO_MMIO_MAX_VQ];
	u32                     msi_mba;
	u32                     msi_pba;
        struct msi_msg          msi_msg[VIRTIO_MMIO_MAX_VQ + VIRTIO_MMIO_MAX_CONFIG];

	struct virtio_mmio_ioevent_param ioeventfds[VIRTIO_MMIO_MAX_VQ];
};

int virtio_mmio_signal_vq(struct kvm *kvm, struct virtio_device *vdev, u32 vq);
int virtio_mmio_signal_config(struct kvm *kvm, struct virtio_device *vdev);
int virtio_mmio_exit(struct kvm *kvm, struct virtio_device *vdev);
int virtio_mmio_reset(struct kvm *kvm, struct virtio_device *vdev);
int virtio_mmio_init(struct kvm *kvm, void *dev, struct virtio_device *vdev,
		      int device_id, int subsys_id, int class);
#endif
