#include "kvm/devices.h"
#include "kvm/msi.h"
#include "kvm/virtio-mmio.h"
#include "kvm/ioeventfd.h"
#include "kvm/ioport.h"
#include "kvm/virtio.h"
#include "kvm/kvm.h"
#include "kvm/kvm-cpu.h"
#include "kvm/irq.h"
#include "kvm/fdt.h"

#include <linux/virtio_mmio.h>
#include <string.h>

#define INCLUDE_MMIO_MSI
#define INCLUDE_MMIO_MSI_SHARING
#define INCLUDE_MMIO_NOTIFICATION
#undef  INCLUDE_DUMP_STATS

#define FEATURE_BIT_SET(index, feature)	\
	(1U << (feature - (index * 32)))

static u32 virtio_mmio_io_space_blocks = KVM_VIRTIO_MMIO_AREA;

#ifdef INCLUDE_DUMP_STATS

static LIST_HEAD(ndevs);

#define DEFINE_VIRTIO_MMIO_STAT(stat) [stat] = #stat

static const char *virtio_mmio_stat[] = {
	DEFINE_VIRTIO_MMIO_STAT(VIRTIO_MMIO_STAT_QUEUE_NOTIFY),
	DEFINE_VIRTIO_MMIO_STAT(VIRTIO_MMIO_STAT_TRAP_IN),
	DEFINE_VIRTIO_MMIO_STAT(VIRTIO_MMIO_STAT_TRAP_OUT),
	DEFINE_VIRTIO_MMIO_STAT(VIRTIO_MMIO_STAT_ACK_IRQ),
	DEFINE_VIRTIO_MMIO_STAT(VIRTIO_MMIO_STAT_CHECK_IRQ),
	DEFINE_VIRTIO_MMIO_STAT(VIRTIO_MMIO_STAT_QUEUE_SEL),
	DEFINE_VIRTIO_MMIO_STAT(VIRTIO_MMIO_STAT_MSI_CMD),
	DEFINE_VIRTIO_MMIO_STAT(VIRTIO_MMIO_STAT_MSI_MASK),
	DEFINE_VIRTIO_MMIO_STAT(VIRTIO_MMIO_STAT_SIG_MSI),
	DEFINE_VIRTIO_MMIO_STAT(VIRTIO_MMIO_STAT_SIG_IRQ),
};

static int virtio_mmio__exit(struct kvm *kvm);
#endif

static inline bool virtio_mmio_has_feature(struct virtio_mmio *vmmio, u32 feature)
{
	u32 i = feature / 32;

	return vmmio->guest_features[i] & FEATURE_BIT_SET(i, feature);
}

static inline bool virtio_mmio_msi_enabled(struct virtio_mmio *vmmio)
{
	return vmmio->hdr.msi_state & VIRTIO_MMIO_MSI_ENABLED;
}

static inline bool virtio_mmio_msi_sharing(struct virtio_mmio *vmmio)
{
	return vmmio->hdr.msi_state & VIRTIO_MMIO_MSI_SHARING;
}

static void virtio_mmio_set_guest_features(struct kvm *kvm,
					   struct virtio_device *vdev,
					   void *dev, u32 features)
{
	struct virtio_mmio *vmmio = vdev->virtio;

	switch (vmmio->hdr.guest_features_sel) {
	case 0:
		/* device features */

		virtio_set_guest_features(kvm, vdev, dev, features);

		/* TBD: fail if features & ~host_features != 0 */

		vmmio->guest_features[0] = features;
		break;

	case 1:
		/* bus features */

		if ((features & FEATURE_BIT_SET(1, VIRTIO_F_MMIO_NOTIFICATION)) &&
			(vmmio->host_features[1] &
				FEATURE_BIT_SET(1, VIRTIO_F_MMIO_NOTIFICATION)))
			vmmio->guest_features[1] |=
				FEATURE_BIT_SET(1, VIRTIO_F_MMIO_NOTIFICATION);

		if ((features & FEATURE_BIT_SET(1, VIRTIO_F_MMIO_MSI)) && 
			(vmmio->host_features[1] &
				FEATURE_BIT_SET(1, VIRTIO_F_MMIO_MSI)))
			vmmio->guest_features[1] |=
				FEATURE_BIT_SET(1, VIRTIO_F_MMIO_MSI);
		break;

	default:
		break;
	}
}

static u32 virtio_mmio_get_io_space_block(u32 size)
{
	u32 block = virtio_mmio_io_space_blocks;
	virtio_mmio_io_space_blocks += size;

	return block;
}

static void virtio_mmio_ioevent_callback(struct kvm *kvm, void *param)
{
	struct virtio_mmio_ioevent_param *ioeventfd = param;
	struct virtio_mmio *vmmio = ioeventfd->vdev->virtio;

	vmmio->ioevents[ioeventfd->vq]++;

	ioeventfd->vdev->ops->notify_vq(kvm, vmmio->dev, ioeventfd->vq);
}

static int virtio_mmio_init_ioeventfd(struct kvm *kvm,
				      struct virtio_device *vdev, u32 vq)
{
	struct virtio_mmio *vmmio = vdev->virtio;
	struct ioevent ioevent;
	int err;

	vmmio->ioeventfds[vq] = (struct virtio_mmio_ioevent_param) {
		.vdev		= vdev,
		.vq		= vq,
	};

	ioevent = (struct ioevent) {
		.io_addr        = vmmio->addr,
		.fn		= virtio_mmio_ioevent_callback,
		.fn_ptr		= &vmmio->ioeventfds[vq],
		.datamatch	= vq,
		.fn_kvm		= kvm,
		.fd		= eventfd(0, 0),
	};

	if (virtio_mmio_has_feature(vmmio, VIRTIO_F_MMIO_NOTIFICATION)) {
		ioevent.io_addr += vmmio->notify_offset + (sizeof(u32) * vq);
		ioevent.io_len   = sizeof(u32);
	} else {
		ioevent.io_addr += VIRTIO_MMIO_QUEUE_NOTIFY;
		ioevent.io_len   = sizeof(u32);
	}

	if (vdev->use_vhost)
		/*
		 * Vhost will poll the eventfd in host kernel side,
		 * no need to poll in userspace.
		 */
		err = ioeventfd__add_event(&ioevent, 0);
	else
		/* Need to poll in userspace. */
		err = ioeventfd__add_event(&ioevent, IOEVENTFD_FLAG_USER_POLL);
	if (err)
		return err;

	if (vdev->ops->notify_vq_eventfd)
		vdev->ops->notify_vq_eventfd(kvm, vmmio->dev, vq, ioevent.fd);

	return 0;
}

static void virtio_mmio_signal_msi(struct kvm *kvm, struct virtio_mmio *vmmio,
				   int vec)
{
	struct kvm_msi msi = {
		.address_lo = vmmio->msi_msg[vec].address_lo,
		.address_hi = vmmio->msi_msg[vec].address_hi,
		.data = vmmio->msi_msg[vec].data,
	};

	if (kvm->msix_needs_devid) {
		msi.flags = KVM_MSI_VALID_DEVID;
		msi.devid = vmmio->dev_hdr.dev_num << 3;
	}

	irq__signal_msi(kvm, &msi);
}

int virtio_mmio_signal_vq(struct kvm *kvm, struct virtio_device *vdev, u32 vq)
{
	struct virtio_mmio *vmmio = vdev->virtio;
	u32 tbl = vmmio->vq_vector[vq];

	if (virtio_mmio_msi_enabled(vmmio) && (tbl != VIRTIO_MMIO_MSI_NO_VECTOR)) {

		/* check if the vector is masked */

		if (vmmio->msi_mba & (1 << tbl)) {
			vmmio->msi_pba |= 1 << tbl;
			return 0;
		}

		vmmio->stats[VIRTIO_MMIO_STAT_SIG_MSI]++;

		if (vmmio->features & VIRTIO_MMIO_F_SIGNAL_MSI)
			virtio_mmio_signal_msi(kvm, vmmio, tbl);
		else
			kvm__irq_trigger(kvm, vmmio->gsis[vq]);
	} else {
		vmmio->stats[VIRTIO_MMIO_STAT_SIG_IRQ]++;

		vmmio->hdr.interrupt_state |= VIRTIO_MMIO_INT_VRING;
		kvm__irq_trigger(vmmio->kvm, vmmio->irq);
	}

	return 0;
}

int virtio_mmio_signal_config(struct kvm *kvm, struct virtio_device *vdev)
{
	struct virtio_mmio *vmmio = vdev->virtio;
	u32 tbl = vmmio->config_vector;

	if (virtio_mmio_msi_enabled(vmmio) && (tbl != VIRTIO_MMIO_MSI_NO_VECTOR)) {
		if (vmmio->msi_mba & (1 << tbl)) {
			vmmio->msi_pba |= 1 << tbl;
			return 0;
		}

		vmmio->stats[VIRTIO_MMIO_STAT_SIG_MSI]++;

		if (vmmio->features & VIRTIO_MMIO_F_SIGNAL_MSI)
			virtio_mmio_signal_msi(kvm, vmmio, tbl);
		else
			kvm__irq_trigger(kvm, vmmio->config_gsi);
	} else {
		vmmio->stats[VIRTIO_MMIO_STAT_SIG_IRQ]++;

		vmmio->hdr.interrupt_state |= VIRTIO_MMIO_INT_CONFIG;
		kvm__irq_trigger(vmmio->kvm, vmmio->irq);
	}

	return 0;
}

static void virtio_mmio_signal_vector(struct kvm *kvm, 
				      struct virtio_device *vdev, u32 vecnum)
{
	struct virtio_mmio *vmmio = vdev->virtio;
	u32 i;

	if (vecnum == vmmio->config_vector) {
		virtio_mmio_signal_config(kvm, vdev);
	} else {
		if (!virtio_mmio_msi_sharing(vmmio)) {
			virtio_mmio_signal_vq(kvm,
					      vdev,
					      vecnum - 1);
		} else {
			for (i = 0; i < vmmio->hdr.msi_vec_num; i++) {
				if (vmmio->vq_vector[i] == vecnum) {
					virtio_mmio_signal_vq(kvm,
							      vdev,
							      i);
				}
			}
		}
	}
}

static void virtio_mmio_exit_vq(struct kvm *kvm, struct virtio_device *vdev,
				int vq)
{
	struct virtio_mmio *vmmio = vdev->virtio;

	ioeventfd__del_event(vmmio->addr + VIRTIO_MMIO_QUEUE_NOTIFY, vq);
	virtio_exit_vq(kvm, vdev, vmmio->dev, vq);
}

static void virtio_mmio_device_specific(struct kvm_cpu *vcpu,
					u64 addr, u8 *data, u32 len,
					u8 is_write, struct virtio_device *vdev)
{
	struct virtio_mmio *vmmio = vdev->virtio;
	u32 i;

	for (i = 0; i < len; i++) {
		if (is_write)
			vdev->ops->get_config(vmmio->kvm, vmmio->dev)[addr + i] =
					      *(u8 *)data + i;
		else
			data[i] = vdev->ops->get_config(vmmio->kvm,
							vmmio->dev)[addr + i];
	}
}

static void virtio_mmio_update_msix_route(struct virtio_mmio *vmmio,
					  struct virtio_device *vdev, 
					  u32 vecnum)
{
	int gsi;
	u32 i;

	/* Find the GSI number used for that vector */

	if (vecnum == vmmio->config_vector) {
		gsi = vmmio->config_gsi;
	} else {
		for (i = 0; i < vmmio->hdr.msi_vec_num; i++)
			if (vmmio->vq_vector[i] == vecnum)
				break;
		if (i >= vmmio->hdr.msi_vec_num)
			return;
		gsi = vmmio->gsis[i];
	}

	if (gsi != 0) {

		/* GSI already exists, update route */

		irq__update_msix_route(vmmio->kvm, gsi, &vmmio->msi_msg[vecnum]);
		return;
	}

	/* create and assign a GSI number for that vector */

	gsi = irq__add_msix_route(vmmio->kvm,
				  &vmmio->msi_msg[vecnum],
				  vmmio->dev_hdr.dev_num << 3);

	/*
	 * We don't need IRQ routing if we can use
	 * MSI injection via the KVM_SIGNAL_MSI ioctl.
	 */

	if ((gsi == -ENXIO) &&
	    (vmmio->features & VIRTIO_MMIO_F_SIGNAL_MSI))
		return;

	if (gsi < 0) {
		die("failed to configure MSIs");
		return;
	}

	if (vecnum == vmmio->config_vector) {
		vmmio->config_gsi = gsi;
	} else {
		for (i = 0; i < vmmio->hdr.msi_vec_num; i++) {
			if (vmmio->vq_vector[i] == vecnum) {
				vmmio->gsis[i] = gsi;
				if (vdev->ops->notify_vq_gsi)
					vdev->ops->notify_vq_gsi(vmmio->kvm,
								 vmmio->dev,
								 i,
								 gsi);
			}
		}
	}
}

static void virtio_mmio_msi_cmd(struct kvm_cpu *vcpu,
				u64 addr, void *data, u32 len,
				struct virtio_device *vdev)
{
	struct virtio_mmio *vmmio = vdev->virtio;
	u32 vecnum = vmmio->hdr.msi_vec_sel;
        u32 queue_sel;
	u32 val;

	val = ioport__read32(data);
	switch (val) {
	case VIRTIO_MMIO_MSI_CMD_ENABLE:
		vmmio->hdr.msi_state |= VIRTIO_MMIO_MSI_ENABLED;
		break;
	case VIRTIO_MMIO_MSI_CMD_DISABLE:
		vmmio->hdr.msi_state &= ~VIRTIO_MMIO_MSI_ENABLED;
		break;
	case VIRTIO_MMIO_MSI_CMD_CONFIGURE:
		if (vecnum == VIRTIO_MMIO_MSI_NO_VECTOR)
			break;

		vmmio->msi_msg[vecnum].address_lo = vmmio->hdr.msi_addr_lo;
		vmmio->msi_msg[vecnum].address_hi = vmmio->hdr.msi_addr_hi;
		vmmio->msi_msg[vecnum].data       = vmmio->hdr.msi_data;

		if (!virtio_mmio_msi_sharing(vmmio)) {
			if (vecnum == 0) {
				if (vmmio->config_vector == 
					VIRTIO_MMIO_MSI_NO_VECTOR) {
					vmmio->config_vector = vecnum;
				}
			} else {
				queue_sel = vecnum - 1;
				if (vmmio->vq_vector[queue_sel] == 
					VIRTIO_MMIO_MSI_NO_VECTOR) {
					vmmio->vq_vector[queue_sel] = vecnum;
				}
			}
		}

		virtio_mmio_update_msix_route(vmmio, vdev, vecnum);
		break;
	case VIRTIO_MMIO_MSI_CMD_MASK:
		vmmio->stats[VIRTIO_MMIO_STAT_MSI_MASK]++;

		vmmio->msi_mba |= (1UL << vecnum);
		break;
	case VIRTIO_MMIO_MSI_CMD_UNMASK:
		vmmio->msi_mba &= ~(1UL << vecnum);

		if (vmmio->msi_pba & (1UL << vecnum)) {
			virtio_mmio_signal_vector(vmmio->kvm, vdev, vecnum);
			vmmio->msi_pba &= ~(1UL << vecnum);
		}

		break;
	case VIRTIO_MMIO_MSI_CMD_MAP_CONFIG:
		if (virtio_mmio_msi_sharing(vmmio))
			vmmio->config_vector = vecnum;
		break;
	case VIRTIO_MMIO_MSI_CMD_MAP_QUEUE:
		if (virtio_mmio_msi_sharing(vmmio))
			vmmio->vq_vector[vmmio->hdr.queue_sel] = vecnum;
		break;
	default:
		break;
	};	
}

static void virtio_mmio_config_in(struct kvm_cpu *vcpu,
				  u64 addr, void *data, u32 len,
				  struct virtio_device *vdev)
{
	struct virtio_mmio *vmmio = vdev->virtio;
	struct virt_queue *vq;
	u32 val = 0;

	vmmio->stats[VIRTIO_MMIO_STAT_TRAP_IN]++;

	switch (addr) {
	case VIRTIO_MMIO_MAGIC_VALUE:
	case VIRTIO_MMIO_VERSION:
	case VIRTIO_MMIO_DEVICE_ID:
	case VIRTIO_MMIO_VENDOR_ID:
	case VIRTIO_MMIO_STATUS:
		ioport__write32(data, *(u32 *)(((void *)&vmmio->hdr) + addr));
		break;
	case VIRTIO_MMIO_QUEUE_NOTIFY:
		if (virtio_mmio_has_feature(vmmio, VIRTIO_F_MMIO_NOTIFICATION)) {
			union virtio_mmio_notify mmio_notify;

			mmio_notify.fields.notify_base       = vmmio->notify_offset;
			mmio_notify.fields.notify_multiplier = sizeof(u32);

			ioport__write32(data, cpu_to_le32(mmio_notify.word));
		}
		break;
	case VIRTIO_MMIO_INTERRUPT_STATUS:
		vmmio->stats[VIRTIO_MMIO_STAT_CHECK_IRQ]++;

		if (!virtio_mmio_msi_enabled(vmmio)) {
			ioport__write32(data, *(u32 *)(((void *)&vmmio->hdr) + addr));
		}
		break;
        case VIRTIO_MMIO_MSI_VEC_NUM:
		if (virtio_mmio_has_feature(vmmio, VIRTIO_F_MMIO_MSI)) {
			ioport__write32(data, *(u32 *)(((void *)&vmmio->hdr) + addr));
		}
		break;
	case VIRTIO_MMIO_MSI_STATE:
		if (virtio_mmio_has_feature(vmmio, VIRTIO_F_MMIO_MSI)) {
			val = cpu_to_le32(*(u32 *)(((void *)&vmmio->hdr) + addr));
			ioport__write32(data, val);
		}
		break;
	case VIRTIO_MMIO_HOST_FEATURES:
		if (vmmio->hdr.host_features_sel < VIRTIO_MMIO_MAX_FEATURE_SEL)
			val = vmmio->host_features[vmmio->hdr.host_features_sel];
		ioport__write32(data, val);
		break;
	case VIRTIO_MMIO_QUEUE_PFN:
		vq = vdev->ops->get_vq(vmmio->kvm, vmmio->dev,
				       vmmio->hdr.queue_sel);
		ioport__write32(data, vq->pfn);
		break;
	case VIRTIO_MMIO_QUEUE_NUM_MAX:
		val = vdev->ops->get_size_vq(vmmio->kvm, vmmio->dev,
					     vmmio->hdr.queue_sel);
		ioport__write32(data, val);
		break;
	default:
		break;
	}
}

static void virtio_mmio_config_out(struct kvm_cpu *vcpu,
				   u64 addr, void *data, u32 len,
				   struct virtio_device *vdev)
{
	struct virtio_mmio *vmmio = vdev->virtio;
	struct kvm *kvm = vmmio->kvm;
	u32 val = 0;

	vmmio->stats[VIRTIO_MMIO_STAT_TRAP_OUT]++;

	switch (addr) {
	case VIRTIO_MMIO_HOST_FEATURES_SEL:
	case VIRTIO_MMIO_GUEST_FEATURES_SEL:
		val = ioport__read32(data);
		*(u32 *)(((void *)&vmmio->hdr) + addr) = val;
		break;
	case VIRTIO_MMIO_QUEUE_SEL:
		vmmio->stats[VIRTIO_MMIO_STAT_QUEUE_SEL]++;

		val = ioport__read32(data);
		*(u32 *)(((void *)&vmmio->hdr) + addr) = val;
		break;
	case VIRTIO_MMIO_MSI_VEC_SEL:
		if (virtio_mmio_has_feature(vmmio, VIRTIO_F_MMIO_MSI)) {
			val = ioport__read32(data);
			if ((val < vmmio->hdr.msi_vec_num) ||
				(val == VIRTIO_MMIO_MSI_NO_VECTOR))
				*(u32 *)(((void *)&vmmio->hdr) + addr) = val;
		}
		break;
	case VIRTIO_MMIO_STATUS:
		vmmio->hdr.status = ioport__read32(data);
		if (!vmmio->hdr.status) /* Sample endianness on reset */
			vdev->endian = kvm_cpu__get_endianness(vcpu);
		virtio_notify_status(kvm, vdev, vmmio->dev, vmmio->hdr.status);
		break;
	case VIRTIO_MMIO_GUEST_FEATURES:
		val = ioport__read32(data);
		virtio_mmio_set_guest_features(vmmio->kvm, vdev, vmmio->dev, val);
		break;
	case VIRTIO_MMIO_GUEST_PAGE_SIZE:
		val = ioport__read32(data);
		vmmio->hdr.guest_page_size = val;
		break;
	case VIRTIO_MMIO_QUEUE_NUM:
		val = ioport__read32(data);
		vmmio->hdr.queue_num = val;
		vdev->ops->set_size_vq(vmmio->kvm, vmmio->dev,
				       vmmio->hdr.queue_sel, val);
		break;
	case VIRTIO_MMIO_QUEUE_ALIGN:
		val = ioport__read32(data);
		vmmio->hdr.queue_align = val;
		break;
	case VIRTIO_MMIO_QUEUE_PFN:
		val = ioport__read32(data);
		if (val) {
			virtio_mmio_init_ioeventfd(vmmio->kvm, vdev,
						   vmmio->hdr.queue_sel);
			vdev->ops->init_vq(vmmio->kvm, vmmio->dev,
					   vmmio->hdr.queue_sel,
					   vmmio->hdr.guest_page_size,
					   vmmio->hdr.queue_align,
					   val);
		} else {
			virtio_mmio_exit_vq(kvm, vdev, vmmio->hdr.queue_sel);
		}
		break;
	case VIRTIO_MMIO_QUEUE_NOTIFY:
		vmmio->stats[VIRTIO_MMIO_STAT_QUEUE_NOTIFY]++;

		val = ioport__read32(data);
		vdev->ops->notify_vq(vmmio->kvm, vmmio->dev, val);
		break;
	case VIRTIO_MMIO_INTERRUPT_ACK:
		vmmio->stats[VIRTIO_MMIO_STAT_ACK_IRQ]++;

		if (!virtio_mmio_msi_enabled(vmmio)) {
			val = ioport__read32(data);
			vmmio->hdr.interrupt_state &= ~val;
		}
		break;
	case VIRTIO_MMIO_MSI_CMD:
		vmmio->stats[VIRTIO_MMIO_STAT_MSI_CMD]++;

		if (virtio_mmio_has_feature(vmmio, VIRTIO_F_MMIO_MSI)) {
			virtio_mmio_msi_cmd(vcpu, addr, data, len, vdev);
		}
		break;
	case VIRTIO_MMIO_MSI_ADDR_LO:
		if (virtio_mmio_has_feature(vmmio, VIRTIO_F_MMIO_MSI)) {
			vmmio->hdr.msi_addr_lo = ioport__read32(data);
		}
		break;
	case VIRTIO_MMIO_MSI_ADDR_HI:
		if (virtio_mmio_has_feature(vmmio, VIRTIO_F_MMIO_MSI)) {
			vmmio->hdr.msi_addr_hi = ioport__read32(data);
		}
		break;
	case VIRTIO_MMIO_MSI_DATA:
		if (virtio_mmio_has_feature(vmmio, VIRTIO_F_MMIO_MSI)) {
			vmmio->hdr.msi_data = ioport__read32(data);
		}
		break;
	default:
		break;
	};
}

static void virtio_mmio_mmio_callback(struct kvm_cpu *vcpu,
				      u64 addr, u8 *data, u32 len,
				      u8 is_write, void *ptr)
{
	struct virtio_device *vdev = ptr;
	struct virtio_mmio *vmmio = vdev->virtio;
	u32 offset = addr - vmmio->addr;

	if (offset >= VIRTIO_MMIO_CONFIG) {
		offset -= VIRTIO_MMIO_CONFIG;
		virtio_mmio_device_specific(vcpu, offset, data, len, is_write, ptr);
		return;
	}

	if (is_write)
		virtio_mmio_config_out(vcpu, offset, data, len, ptr);
	else
		virtio_mmio_config_in(vcpu, offset, data, len, ptr);
}

#ifdef CONFIG_HAS_LIBFDT
#define DEVICE_NAME_MAX_LEN 32
static
void generate_virtio_mmio_fdt_node(void *fdt,
				   struct device_header *dev_hdr,
				   void (*generate_irq_prop)(void *fdt,
							     u8 irq,
							     enum irq_type))
{
	char dev_name[DEVICE_NAME_MAX_LEN];
	struct virtio_mmio *vmmio = container_of(dev_hdr,
						 struct virtio_mmio,
						 dev_hdr);
	u64 addr = vmmio->addr;
	u64 reg_prop[] = {
		cpu_to_fdt64(addr),
		cpu_to_fdt64(VIRTIO_MMIO_IO_SIZE),
	};

	snprintf(dev_name, DEVICE_NAME_MAX_LEN, "virtio@%llx", addr);

	_FDT(fdt_begin_node(fdt, dev_name));
	_FDT(fdt_property_string(fdt, "compatible", "virtio,mmio"));
	_FDT(fdt_property(fdt, "reg", reg_prop, sizeof(reg_prop)));
	_FDT(fdt_property(fdt, "dma-coherent", NULL, 0));
	generate_irq_prop(fdt, vmmio->irq, IRQ_TYPE_EDGE_RISING);
	_FDT(fdt_end_node(fdt));
}
#else
static void generate_virtio_mmio_fdt_node(void *fdt,
					  struct device_header *dev_hdr,
					  void (*generate_irq_prop)(void *fdt,
								    u8 irq))
{
	die("Unable to generate device tree nodes without libfdt\n");
}
#endif

int virtio_mmio_init(struct kvm *kvm, void *dev, struct virtio_device *vdev,
		     int device_id, int subsys_id, int class)
{
	struct virtio_mmio *vmmio = vdev->virtio;
	u32 nvqs = vdev->ops->get_vq_count(kvm, dev);
	int r;
#ifdef INCLUDE_MMIO_MSI
	u32 i;
#endif

	vmmio->addr	= virtio_mmio_get_io_space_block(VIRTIO_MMIO_IO_SIZE);
	vmmio->kvm	= kvm;
	vmmio->dev	= dev;

	r = kvm__register_mmio(kvm, vmmio->addr, VIRTIO_MMIO_IO_SIZE,
			       false, virtio_mmio_mmio_callback, vdev);
	if (r < 0)
		return r;

	vmmio->hdr = (struct virtio_mmio_hdr) {
		.magic		= {'v', 'i', 'r', 't'},
		.version	= 1,
		.device_id	= subsys_id,
		.vendor_id	= 0x4d564b4c , /* 'LKVM' */
		.queue_num_max	= 256,
                .msi_vec_num    = nvqs + VIRTIO_MMIO_MAX_CONFIG,
#if defined(INCLUDE_MMIO_MSI) && defined(INCLUDE_MMIO_MSI_SHARING)
		.msi_state      = VIRTIO_MMIO_MSI_SHARING,
#endif
	};

	/* device features */

	vmmio->host_features[0] = vdev->ops->get_host_features(kvm, dev);

	/* bus features */

#ifdef INCLUDE_MMIO_NOTIFICATION
	if ((nvqs * sizeof(u32)) <= 
		(VIRTIO_MMIO_CONFIG - VIRTIO_MMIO_MSI_DATA + 4)) {
        	vmmio->host_features[1] |= 
			FEATURE_BIT_SET(1, VIRTIO_F_MMIO_NOTIFICATION);

		vmmio->notify_offset = VIRTIO_MMIO_MSI_DATA + 4;
	}
#endif

#ifdef INCLUDE_MMIO_MSI
        vmmio->host_features[1] |= FEATURE_BIT_SET(1, VIRTIO_F_MMIO_MSI);
#endif

	vmmio->dev_hdr = (struct device_header) {
		.bus_type	= DEVICE_BUS_MMIO,
		.data		= generate_virtio_mmio_fdt_node,
	};

	vmmio->irq = irq__alloc_line();

        /* MSI */

#ifdef INCLUDE_MMIO_MSI
	if (irq__can_signal_msi(kvm))
		vmmio->features |= VIRTIO_MMIO_F_SIGNAL_MSI;
	for (i = 0; i < VIRTIO_MMIO_MAX_VQ; ++i) {
		vmmio->vq_vector[i] = VIRTIO_MMIO_MSI_NO_VECTOR;
	}
	vmmio->config_vector = VIRTIO_MMIO_MSI_NO_VECTOR;
#endif

	r = device__register(&vmmio->dev_hdr);
	if (r < 0) {
		kvm__deregister_mmio(kvm, vmmio->addr);
		return r;
	}

#ifdef INCLUDE_DUMP_STATS
	list_add_tail(&vmmio->list, &ndevs);
#endif

	/*
	 * Instantiate guest virtio-mmio devices using kernel command line
	 * (or module) parameter, e.g
	 *
	 * virtio_mmio.devices=0x200@0xd2000000:5,0x200@0xd2000200:6
	 */
	pr_debug("virtio-mmio.device=0x%x@0x%x:%d", VIRTIO_MMIO_IO_SIZE,
		 vmmio->addr, vmmio->irq);

	return 0;
}

int virtio_mmio_reset(struct kvm *kvm, struct virtio_device *vdev)
{
	int vq;
	struct virtio_mmio *vmmio = vdev->virtio;

	for (vq = 0; vq < vdev->ops->get_vq_count(kvm, vmmio->dev); vq++)
		virtio_mmio_exit_vq(kvm, vdev, vq);

	/* reset MSI */

	vmmio->hdr.msi_state &= ~VIRTIO_MMIO_MSI_ENABLED;

	return 0;
}

int virtio_mmio_exit(struct kvm *kvm, struct virtio_device *vdev)
{
	struct virtio_mmio *vmmio = vdev->virtio;

	virtio_mmio_reset(kvm, vdev);
	kvm__deregister_mmio(kvm, vmmio->addr);

	return 0;
}

#ifdef INCLUDE_DUMP_STATS
static int virtio_mmio__exit(struct kvm *kvm)
{
	struct virtio_mmio *vmmio;
	struct list_head *ptr, *n;
	u32 i;

	list_for_each_safe(ptr, n, &ndevs) {
		vmmio = list_entry(ptr, struct virtio_mmio, list);

		pr_info ("\n%s@%.8x:\n", "virtio", vmmio->addr);

		for (i = 0; i < VIRTIO_MMIO_STAT_MAX; ++i) {
			pr_info ("%32s %16u\n",
				 virtio_mmio_stat[i], vmmio->stats[i]);
		}

		pr_info ("IOEVENTS:\n");

		for (i = 0; i < VIRTIO_MMIO_MAX_VQ; ++i) {
			if (vmmio->ioevents[i] > 0)
				pr_info ("%32u %16u\n", i, vmmio->ioevents[i]);
		}

		list_del(&vmmio->list);
	}
	return 0;
}
virtio_dev_exit(virtio_mmio__exit);
#endif
