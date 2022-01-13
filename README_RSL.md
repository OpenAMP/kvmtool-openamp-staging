# Native Linux KVM tool

KVM tool is a lightweight tool for hosting KVM guests. As a pure virtualization tool it only supports guests using the same architecture, though it supports running 32-bit guests on those 64-bit architectures that allow this.

# Making virtio hypervisorless

At its core, virtio makes a set of assumptions which are typically fulfilled by a virtual machine monitor (VMM) which has complete access to the guest memory space. Removing the hypervisor from the virtio equation would imply the following: 

- Feature negotiation is replaced by predefined feature lists defined by the system architect. 
- Virtio can be used without virtualization hardware or virtualization enabled. 
- The term VMM is a misnomer when there is no virtualization so PMM (Physical Machine Monitor) is used instead. 
- The pre-shared memory region is defined / allocated by the PMM.  

What makes virtio hypervisor-less? 
- MMIO transport over shared memory  
- Unsupervised AMP support  
- Static configuration (features, queues)  
- Hardware notifications 
- A PMM instead of VMM

It is worth mentioning that performance is highly dependent on the type and number of hardware notifications available on the platform, since the hypervisor-based notification infrastructure will no longer be available. 

# Using kvmtool as a hypervisorless virtio test bed

KVM tool is a convenient starting point to build a PMM as it allows reuse of the virtio device support it already has. The information on the location and size of the shared memory allocated to each virtio device is published in the virtio device header as a shared memory region definition (found in virtio 1.2). 

Virtqueue configuration and supported feature bits are statically defined in the virtio device header which is populated in the shared memory region dedicated to a virtio device. 

On the guest side, the virtqueues are explicitly located in the shared memory region mapped by the guest. Data buffers are either allocated in the pre-shared memory region or are copied there by a transparent shim layer which implements a bounce-buffer mechanism. 

KVM tool can be used to bootstrap a KVM guest and then provide a hypervisor-less virtio backend for the guest to use.

A set of parameters is introduced for operating in hypervisor-less virtio mode.

Physical Machine Monitor options:

```
--irq <n>         Notification IRQ
--rsld            Run in PMM mode
--transport <transport>
                  virtio transport: mmio, pci
--shmem-addr <n>  Shared memory physical address
--shmem-size <n>  Shared memory size
--vproxy          vhost proxy mode
```

Notes:

- For an x86 guest, the irq parameter specifies an IO APIC interrupt line to be used as virtio backend-to-frontend notification.
- In the hypervisorless mode of operation, the transport is mmio.
- shmem-addr and shmem-size define a shared memory pool from which each virtio device receives a pre-shared memory region to use for virtqueues and data buffers
--vproxy enables notification indirection between the PMM and vhost_net and vhost_vsock to allow the PMM to make use of the host's AF_VSOCK support.

Currently the following virtio devices can operate in hypervisor-less mode: 9p, console, virtio network, virtio vsock.

This launch configuration enables virtio-net, vsock, 9p and virtio console for a guest running the VxWorks real-time operating system. In this configuration KVM is used to bootstrap the guest / virtio front-end and to support the notification infrastructure between the virtio back-end and front-end.

lkvm run --debug-nohostfs --cpus 1 --mem 2048 \
--console virtio \
--irq 12 --rsld --transport mmio \
--shmem-addr 0xd4000000 --shmem-size 0x1000000 \
--network mode=tap,tapif=tap0,trans=mmio,vhost=1 \
--vproxy --vsock 3 \
-- 9p /tmp,/tmp \
-p "fs(0,0):/rsl/hello h=192.168.200.254 e=192.168.200.2 u=ftp pw=ftp o=virtioNet;;hello r=;;hvl.irq=12;;hvl.shm_addr=0xd4000000 f=0x400 tn=hello" \
--kernel vxWorks_lpc

# Full hypervisorless-mode virtio mode

The  physical machine monitor virtio back-end has been validated in full hypervisorless mode on a Xilinx Zynq UltraScale+ MPSoC ZCU102 platform, with the RSL daemon running on PetaLinux on the main CPU cluster and with VxWorks running on one of the Cortex-R5 CPUs.

The shared memory information is included in the PetaLinux DTB and is made accessible to the RSL daemon (virtio back-end) via a userspace I/O device. The notification mechanism between the back-end and the front-end is based on the Xilinx IPI (Inter Processor Interrupt) mailbox controller.

## VMM to PMM transition

The KVM VCPU threads were replaced with a PMM thread which monitors a char device fd for mailbox notifications.

Several kvmtool initialization steps have been disabled and /dev/kvm not used at all.

The PMM pseudo-state machine implemented in virtio/mmio.c / virtio_mmio_notification_out handles configuration and virtqueue notifications and calls the appropriate virtio device back-end to handle requests.

A synchronization mechanism is put in place to handle queue PFN configuration which uses the same device register for all the virtqueues.

VM sockets (vsock) are enabled with support from the Linux vhost_vsock module. The PMM registers call and kick eventfds with the vhost subsystem and acts as notification proxy between vhost_vsock and the virtio vsock driver in the auxiliary runtime known as guest in hypervisor-based deployments.

A new command line parameter (--pmm) has been added to enable full hypervisor-less mode.

An updated launch configuration will look like:

lkvm run --debug \
--vxworks --rsld --pmm --debug-nohostfs --transport mmio \
--shmem-addr 0x77000000 --shmem-size 0x1000000 \
--cpus 1 --mem 128 \
-p "fs(0,0):/rsl/hello h=192.168.200.254 e=192.168.200.2 u=ftp pw=ftp o=virtioNet f=0x01 r=;;hvl.shm_addr=0x77000000" \
--vproxy \
--console virtio \
--network mode=tap,tapif=tap0,trans=mmio \
--vsock 3 \
--9p /tmp,/tmp

Zephyr can be used as a secondary runtime in a hypervisor-less virtio deployment. In this case the launch configuration would be similar to:

lkvm run --debug \
--vxworks --rsld --pmm --debug-nohostfs --transport mmio \
--shmem-addr 0x77000000 --shmem-size 0x1000000 \
--cpus 1 --mem 128 --no-dtb \
--vproxy \
--rng \
--network mode=tap,tapif=tap0,trans=mmio

