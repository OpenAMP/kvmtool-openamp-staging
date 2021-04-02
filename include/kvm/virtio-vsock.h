#ifndef KVM__VIRTIO_VSOCK_H
#define KVM__VIRTIO_VSOCK_H

struct kvm;

int virtio_vsock_init(struct kvm *kvm);
int virtio_vsock_exit(struct kvm *kvm);

#endif  // KVM__VIRTIO_VSOCK_H
