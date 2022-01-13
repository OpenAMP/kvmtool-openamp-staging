KMAKEFLAGS := CROSS_COMPILE=aarch64-xilinx-linux- ARCH=arm64 
KDIR := /home/dan/projects/cto/appstar/src/zynq_ipi/linux-xlnx

modules:
	$(KMAKEFLAGS) make -C $(KDIR) M=$(CURDIR) modules
clean:
	$(KMAKEFLAGS) make -C $(KDIR) M=$(CURDIR) clean
