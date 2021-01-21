#ifndef KVM__KVM_CONFIG_ARCH_H
#define KVM__KVM_CONFIG_ARCH_H

#include "kvm/parse-options.h"

struct kvm_config_arch {
	int vidmode;
	const char	*dump_dtb_filename;
};

#define OPT_ARCH_RUN(pfx, cfg)						\
	pfx,								\
	OPT_STRING('\0', "dump-dtb", &(cfg)->dump_dtb_filename,			\
		   ".dtb file", "Dump generated .dtb to specified file"),	\
	OPT_GROUP("BIOS options:"),					\
	OPT_INTEGER('\0', "vidmode", &(cfg)->vidmode, "Video mode"),

#endif /* KVM__KVM_CONFIG_ARCH_H */
