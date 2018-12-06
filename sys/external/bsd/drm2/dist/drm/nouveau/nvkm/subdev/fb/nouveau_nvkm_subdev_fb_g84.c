/*	$NetBSD: nouveau_nvkm_subdev_fb_g84.c,v 1.2 2018/08/27 04:58:33 riastradh Exp $	*/

/*
 * Copyright 2012 Red Hat Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 *
 * Authors: Ben Skeggs
 */
#include <sys/cdefs.h>
__KERNEL_RCSID(0, "$NetBSD: nouveau_nvkm_subdev_fb_g84.c,v 1.2 2018/08/27 04:58:33 riastradh Exp $");

#include "nv50.h"
#include "ram.h"

static const struct nv50_fb_func
g84_fb = {
	.ram_new = nv50_ram_new,
	.trap = 0x001d07ff,
};

int
g84_fb_new(struct nvkm_device *device, int index, struct nvkm_fb **pfb)
{
	return nv50_fb_new_(&g84_fb, device, index, pfb);
}