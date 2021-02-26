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
```

Notes:

- For an x86 guest, the irq parameter specifies an IO APIC interrupt line to be used as virtio backend-to-frontend notification.
- In the hypervisorless mode of operation, the transport is mmio.
- shmem-addr and shmem-size define a shared memory pool from which each virtio device receives a pre-shared memory region to use for virtqueues and data buffers


Currently the following virtio devices can operate in hypervisor-less mode: 9p, console, virtio network.
