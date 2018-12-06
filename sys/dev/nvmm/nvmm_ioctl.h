/*	$NetBSD: nvmm_ioctl.h,v 1.1 2018/11/07 07:43:08 maxv Exp $	*/

/*
 * Copyright (c) 2018 The NetBSD Foundation, Inc.
 * All rights reserved.
 *
 * This code is derived from software contributed to The NetBSD Foundation
 * by Maxime Villard.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE NETBSD FOUNDATION, INC. AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE FOUNDATION OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef _NVMM_IOCTL_H_
#define _NVMM_IOCTL_H_

#include <dev/nvmm/nvmm.h>

struct nvmm_ioc_capability {
	struct nvmm_capability cap;
};

struct nvmm_ioc_machine_create {
	nvmm_machid_t machid;
};

struct nvmm_ioc_machine_destroy {
	nvmm_machid_t machid;
};

struct nvmm_ioc_machine_configure {
	nvmm_machid_t machid;
	uint64_t op;
	void *conf;
};

struct nvmm_ioc_vcpu_create {
	nvmm_machid_t machid;
	nvmm_cpuid_t cpuid;
};

struct nvmm_ioc_vcpu_destroy {
	nvmm_machid_t machid;
	nvmm_cpuid_t cpuid;
};

struct nvmm_ioc_vcpu_setstate {
	nvmm_machid_t machid;
	nvmm_cpuid_t cpuid;
	uint64_t flags;
	void *state;
};

struct nvmm_ioc_vcpu_getstate {
	nvmm_machid_t machid;
	nvmm_cpuid_t cpuid;
	uint64_t flags;
	void *state;
};

struct nvmm_ioc_vcpu_inject {
	nvmm_machid_t machid;
	nvmm_cpuid_t cpuid;
	struct nvmm_event event;
};

struct nvmm_ioc_vcpu_run {
	/* input */
	nvmm_machid_t machid;
	nvmm_cpuid_t cpuid;
	/* output */
	struct nvmm_exit exit;
};

struct nvmm_ioc_gpa_map {
	nvmm_machid_t machid;
	uintptr_t hva;
	gpaddr_t gpa;
	size_t size;
	int flags;
};

struct nvmm_ioc_gpa_unmap {
	nvmm_machid_t machid;
	gpaddr_t gpa;
	size_t size;
};

#define NVMM_IOC_CAPABILITY		_IOR ('N',  0, struct nvmm_ioc_capability)
#define NVMM_IOC_MACHINE_CREATE		_IOWR('N',  1, struct nvmm_ioc_machine_create)
#define NVMM_IOC_MACHINE_DESTROY	_IOW ('N',  2, struct nvmm_ioc_machine_destroy)
#define NVMM_IOC_MACHINE_CONFIGURE	_IOW ('N',  3, struct nvmm_ioc_machine_configure)
#define NVMM_IOC_VCPU_CREATE		_IOW ('N',  4, struct nvmm_ioc_vcpu_create)
#define NVMM_IOC_VCPU_DESTROY		_IOW ('N',  5, struct nvmm_ioc_vcpu_destroy)
#define NVMM_IOC_VCPU_SETSTATE		_IOW ('N',  6, struct nvmm_ioc_vcpu_setstate)
#define NVMM_IOC_VCPU_GETSTATE		_IOW ('N',  7, struct nvmm_ioc_vcpu_getstate)
#define NVMM_IOC_VCPU_INJECT		_IOWR('N',  8, struct nvmm_ioc_vcpu_inject)
#define NVMM_IOC_VCPU_RUN		_IOWR('N',  9, struct nvmm_ioc_vcpu_run)
#define NVMM_IOC_GPA_MAP		_IOW ('N', 10, struct nvmm_ioc_gpa_map)
#define NVMM_IOC_GPA_UNMAP		_IOW ('N', 11, struct nvmm_ioc_gpa_unmap)

#endif /* _NVMM_IOCTL_H_ */