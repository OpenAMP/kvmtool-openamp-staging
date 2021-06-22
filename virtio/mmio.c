#include "kvm/devices.h"
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

static u32 virtio_mmio_io_space_blocks = KVM_VIRTIO_MMIO_AREA;

static u32 virtio_mmio_get_io_space_block(u32 size)
{
	u32 block = virtio_mmio_io_space_blocks;
	virtio_mmio_io_space_blocks += size;

	return block;
}

#ifdef RSLD
static u64 virtio_mmio_shm_space_blocks;
static u64 virtio_mmio_shm_dtb_offset = FDT_MAX_SIZE;
static u64 virtio_mmio_get_shm_space_block(struct kvm *kvm, u32 size)
{
    u64 block;

    if (kvm->cfg.hvl_shmem_phys_addr == 0)
        return 0;

    if (virtio_mmio_shm_space_blocks == 0) {
        virtio_mmio_shm_space_blocks = kvm->cfg.hvl_shmem_phys_addr + virtio_mmio_shm_dtb_offset;
    }
    block = virtio_mmio_shm_space_blocks;
	virtio_mmio_shm_space_blocks += size;

	return block;
}
#endif

static void virtio_mmio_ioevent_callback(struct kvm *kvm, void *param)
{
	struct virtio_mmio_ioevent_param *ioeventfd = param;
	struct virtio_mmio *vmmio = ioeventfd->vdev->virtio;

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
		.io_addr	= vmmio->addr + VIRTIO_MMIO_QUEUE_NOTIFY,
		.io_len		= sizeof(u32),
		.fn		= virtio_mmio_ioevent_callback,
		.fn_ptr		= &vmmio->ioeventfds[vq],
		.datamatch	= vq,
		.fn_kvm		= kvm,
		.fd		= eventfd(0, 0),
	};

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

int virtio_mmio_signal_vq(struct kvm *kvm, struct virtio_device *vdev, u32 vq)
{
	struct virtio_mmio *vmmio = vdev->virtio;

	vmmio->hdr.interrupt_state |= VIRTIO_MMIO_INT_VRING;
#ifdef RSLD
	if (kvm->cfg.rsld) {
		if (vmmio->static_hdr != NULL)
			vmmio->static_hdr->interrupt_state |= VIRTIO_MMIO_INT_VRING;
		kvm__irq_trigger(kvm, kvm->cfg.hvl_irq);
	} else {
#endif
	kvm__irq_trigger(vmmio->kvm, vmmio->irq);
#ifdef RSLD
	}
#endif

	return 0;
}

static void virtio_mmio_exit_vq(struct kvm *kvm, struct virtio_device *vdev,
				int vq)
{
	struct virtio_mmio *vmmio = vdev->virtio;

	ioeventfd__del_event(vmmio->addr + VIRTIO_MMIO_QUEUE_NOTIFY, vq);
	virtio_exit_vq(kvm, vdev, vmmio->dev, vq);
}

int virtio_mmio_signal_config(struct kvm *kvm, struct virtio_device *vdev)
{
	struct virtio_mmio *vmmio = vdev->virtio;

	vmmio->hdr.interrupt_state |= VIRTIO_MMIO_INT_CONFIG;
#ifdef RSLD
    vmmio->static_hdr->interrupt_state |= VIRTIO_MMIO_INT_CONFIG;
#endif
	kvm__irq_trigger(vmmio->kvm, vmmio->irq);

	return 0;
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

static void virtio_mmio_config_in(struct kvm_cpu *vcpu,
				  u64 addr, void *data, u32 len,
				  struct virtio_device *vdev)
{
	struct virtio_mmio *vmmio = vdev->virtio;
	struct virt_queue *vq;
	u32 val = 0;

	switch (addr) {
	case VIRTIO_MMIO_MAGIC_VALUE:
	case VIRTIO_MMIO_VERSION:
	case VIRTIO_MMIO_DEVICE_ID:
	case VIRTIO_MMIO_VENDOR_ID:
	case VIRTIO_MMIO_STATUS:
	case VIRTIO_MMIO_INTERRUPT_STATUS:
#ifdef RSLD
	case VIRTIO_MMIO_SHM_BASE_LOW:
	case VIRTIO_MMIO_SHM_BASE_HIGH:
	case VIRTIO_MMIO_SHM_LEN_LOW:
	case VIRTIO_MMIO_SHM_LEN_HIGH:
#endif
		ioport__write32(data, *(u32 *)(((void *)&vmmio->hdr) + addr));
		break;
	case VIRTIO_MMIO_HOST_FEATURES:
		if (vmmio->hdr.host_features_sel == 0)
			val = vdev->ops->get_host_features(vmmio->kvm,
							   vmmio->dev);
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

	switch (addr) {
	case VIRTIO_MMIO_HOST_FEATURES_SEL:
	case VIRTIO_MMIO_GUEST_FEATURES_SEL:
	case VIRTIO_MMIO_QUEUE_SEL:
		val = ioport__read32(data);
		*(u32 *)(((void *)&vmmio->hdr) + addr) = val;
		break;
	case VIRTIO_MMIO_STATUS:
		vmmio->hdr.status = ioport__read32(data);
		if (!vmmio->hdr.status) /* Sample endianness on reset */
			vdev->endian = kvm_cpu__get_endianness(vcpu);
		virtio_notify_status(kvm, vdev, vmmio->dev, vmmio->hdr.status);
		break;
	case VIRTIO_MMIO_GUEST_FEATURES:
		if (vmmio->hdr.guest_features_sel == 0) {
			val = ioport__read32(data);
			virtio_set_guest_features(vmmio->kvm, vdev,
						  vmmio->dev, val);
		}
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
		val = ioport__read32(data);
		vdev->ops->notify_vq(vmmio->kvm, vmmio->dev, val);
		break;
	case VIRTIO_MMIO_INTERRUPT_ACK:
		val = ioport__read32(data);
		vmmio->hdr.interrupt_state &= ~val;
		break;
	default:
		break;
	};
}

#ifdef RSLD
static void virtio_mmio_notification_out(struct kvm_cpu *vcpu,
				   u64 addr, void *data, u32 len,
				   struct virtio_device *vdev)
{
	struct virtio_mmio *vmmio = vdev->virtio;
	struct kvm *kvm = vmmio->kvm;
	u32 val = 0;
    static int qidx = 0;
    int i = 0;

    if (vmmio->hdr.status & VIRTIO_CONFIG_S_DRIVER_OK) {
        for (i = 0; i < vmmio->num_vqs; i++) {
            vdev->ops->notify_vq(vmmio->kvm, vmmio->dev, i);
        }
        return;
    }

    if (vmmio->static_hdr->guest_features != vmmio->hdr.guest_features) {
		virtio_set_guest_features(kvm, vdev, vmmio->dev, vmmio->static_hdr->guest_features);
        vmmio->hdr.guest_features = vmmio->static_hdr->guest_features;
    }

    if ((vmmio->hdr.status & VIRTIO_CONFIG_S_DRIVER) && (!(vmmio->hdr.status & VIRTIO_CONFIG_S_DRIVER_OK))) {
        if (vmmio->static_hdr->queue_sel != vmmio->hdr.queue_sel) {
			vmmio->hdr.queue_sel = vmmio->static_hdr->queue_sel;
        }
        if (vmmio->static_hdr->queue_pfn != vmmio->hdr.queue_pfn) {
            vmmio->hdr.queue_pfn = vmmio->static_hdr->queue_pfn;
    		val = vmmio->static_hdr->queue_pfn;
            qidx = vmmio->num_vqs;
    		if (val) {
    			virtio_mmio_init_ioeventfd(vmmio->kvm, vdev, qidx);
    			vdev->ops->init_vq(vmmio->kvm, vmmio->dev,
    					   vmmio->num_vqs,
    					   vmmio->static_hdr->guest_page_size,
    					   vmmio->static_hdr->queue_align,
    					   val);
                vmmio->num_vqs++;
            } else {
    			virtio_mmio_exit_vq(kvm, vdev, qidx);
    		}
        }
    }

    if (vmmio->static_hdr->status != vmmio->hdr.status) {
        vmmio->static_hdr->host_features = vdev->ops->get_host_features(vmmio->kvm, vmmio->dev);
        if (!vmmio->hdr.status)
			vdev->endian = kvm_cpu__get_endianness(vcpu);
        vmmio->hdr.status = vmmio->static_hdr->status;
        virtio_notify_status(kvm, vdev, vmmio->dev, vmmio->hdr.status);
    }
    if (vmmio->static_hdr->interrupt_state != vmmio->hdr.interrupt_state) {
        vmmio->hdr.interrupt_state &= ~vmmio->static_hdr->interrupt_state;
        vmmio->static_hdr->interrupt_state = vmmio->hdr.interrupt_state;
    }
    //TODO: check config changes @VIRTIO_MMIO_CONFIG
}
#endif

static void virtio_mmio_mmio_callback(struct kvm_cpu *vcpu,
				      u64 addr, u8 *data, u32 len,
				      u8 is_write, void *ptr)
{
	struct virtio_device *vdev = ptr;
	struct virtio_mmio *vmmio = vdev->virtio;
	u32 offset = addr - vmmio->addr;

#ifdef RSLD
    if ((offset == 0x1F0) && is_write) {
        virtio_mmio_notification_out(vcpu, offset, data, len, ptr);
        return;
    }
#endif

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
#ifdef RSLD
    if (vmmio->static_hdr != NULL) {
        addr = (((u64)vmmio->static_hdr->shm_base_high) << 32) | vmmio->static_hdr->shm_base_low;
    }
#endif

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
	int r;
#ifdef RSLD
    u64 vmmio_shm_addr = 0;
    u64 vmmio_shm_phys_addr = 0;
    u64 vmmio_shm_size = 0x400000; //4MB
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
	};

	vmmio->dev_hdr = (struct device_header) {
		.bus_type	= DEVICE_BUS_MMIO,
		.data		= generate_virtio_mmio_fdt_node,
	};

	vmmio->irq = irq__alloc_line();

	r = device__register(&vmmio->dev_hdr);
	if (r < 0) {
		kvm__deregister_mmio(kvm, vmmio->addr);
		return r;
	}

#ifdef RSLD
    if (vdev->ops->get_mem_size)
        vmmio_shm_size = vdev->ops->get_mem_size(vmmio->kvm, vmmio->dev);

    vmmio_shm_phys_addr = virtio_mmio_get_shm_space_block(kvm, vmmio_shm_size);

    if (vmmio_shm_phys_addr != 0) {
        vmmio_shm_addr = (u64)kvm->shmem_start + (vmmio_shm_phys_addr - kvm->cfg.hvl_shmem_phys_addr);
        vmmio->hdr.shm_len_low = virtio_host_to_guest_u32(vdev, (u32)vmmio_shm_size);
        vmmio->hdr.shm_len_high = virtio_host_to_guest_u32(vdev, vmmio_shm_size >> 32);
        vmmio->hdr.shm_base_low = virtio_host_to_guest_u32(vdev, (u32)vmmio_shm_phys_addr);
        vmmio->hdr.shm_base_high = virtio_host_to_guest_u32(vdev, vmmio_shm_phys_addr >> 32);

        memcpy((void *)vmmio_shm_addr, &vmmio->hdr, sizeof(struct virtio_mmio_hdr));
        vmmio->static_hdr = (struct virtio_mmio_hdr *)vmmio_shm_addr;
        vmmio->static_hdr->guest_page_size = 0x1000;
        vmmio->static_hdr->queue_align = 0x1000;
        vmmio->static_hdr->host_features = vdev->ops->get_host_features(vmmio->kvm, vmmio->dev);
        vmmio->static_hdr->queue_num_max = vdev->ops->get_size_vq(vmmio->kvm, vmmio->dev, 0);
        vmmio->hdr.guest_page_size = 0x1000;
        vmmio->hdr.host_features = vmmio->static_hdr->host_features;
        vmmio->hdr.queue_num_max = vmmio->static_hdr->queue_num_max;
        vmmio->hdr.queue_align = vmmio->static_hdr->queue_align;
        vmmio->static_hdr->queue_sel = ~vmmio->hdr.queue_sel;

        if (vdev->ops->get_config_size) {
            int config_size = vdev->ops->get_config_size(vmmio->kvm, vmmio->dev);
            u8 *devcfg  = (u8 *)(vmmio_shm_addr + VIRTIO_MMIO_CONFIG);
            int i;
            for (i = 0; i < config_size; i++) {
                devcfg[i] = vdev->ops->get_config(vmmio->kvm,
                                    vmmio->dev)[i];
            }
        }
    }
#endif
	/*
	 * Instantiate guest virtio-mmio devices using kernel command line
	 * (or module) parameter, e.g
	 *
	 * virtio_mmio.devices=0x200@0xd2000000:5,0x200@0xd2000200:6
	 */
	pr_debug("virtio-mmio.devices=0x%x@0x%x:%d", VIRTIO_MMIO_IO_SIZE,
		 vmmio->addr, vmmio->irq);

	return 0;
}

int virtio_mmio_reset(struct kvm *kvm, struct virtio_device *vdev)
{
	int vq;
	struct virtio_mmio *vmmio = vdev->virtio;

	for (vq = 0; vq < vdev->ops->get_vq_count(kvm, vmmio->dev); vq++)
		virtio_mmio_exit_vq(kvm, vdev, vq);

	return 0;
}

int virtio_mmio_exit(struct kvm *kvm, struct virtio_device *vdev)
{
	struct virtio_mmio *vmmio = vdev->virtio;

	virtio_mmio_reset(kvm, vdev);
	kvm__deregister_mmio(kvm, vmmio->addr);

	return 0;
}

#ifdef RSLD
static void dump_fdt(const char *dtb_file, void *fdt)
{
	int count, fd;

	fd = open(dtb_file, O_CREAT | O_TRUNC | O_RDWR, 0666);
	if (fd < 0)
		die("Failed to write dtb to %s", dtb_file);

	count = write(fd, fdt, FDT_MAX_SIZE);
	if (count < 0)
		die_perror("Failed to dump dtb");

	pr_debug("Wrote %d bytes to dtb %s", count, dtb_file);
	close(fd);
}

static void generate_irq_prop_stub(void *fdt, u8 irq, enum irq_type irqt)
{
	return;
}

static int setup_virtio_mmio_fdt(struct kvm *kvm)
{
	struct device_header *dev_hdr;
	u8 staging_fdt[FDT_MAX_SIZE];
	u8 *pdest;
	void *fdt = staging_fdt;
	void (*generate_mmio_fdt_nodes)(void *, struct device_header *,
					void (*)(void *, u8, enum irq_type));

	/* Create new tree without a reserve map */
	_FDT(fdt_create(fdt, FDT_MAX_SIZE));
	_FDT(fdt_finish_reservemap(fdt));

	/* Header */
	_FDT(fdt_begin_node(fdt, ""));
	_FDT(fdt_property_cell(fdt, "#address-cells", 0x2));
	_FDT(fdt_property_cell(fdt, "#size-cells", 0x2));

	/* Virtio MMIO devices */
	dev_hdr = device__first_dev(DEVICE_BUS_MMIO);
	while (dev_hdr) {
		generate_mmio_fdt_nodes = dev_hdr->data;
		generate_mmio_fdt_nodes(fdt, dev_hdr, generate_irq_prop_stub);
		dev_hdr = device__next_dev(dev_hdr);
	}

	/* Finalise. */
	_FDT(fdt_end_node(fdt));
	_FDT(fdt_finish(fdt));
	if (kvm->shmem_start != 0) {
		pdest = kvm->shmem_start;
		_FDT(fdt_open_into(fdt, pdest, FDT_MAX_SIZE));
		_FDT(fdt_pack(pdest));

		if (kvm->cfg.arch.dump_dtb_filename)
			dump_fdt(kvm->cfg.arch.dump_dtb_filename, pdest);
	}
	return 0;
}
late_init(setup_virtio_mmio_fdt);
#endif
