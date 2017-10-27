#pragma once

#include <stdint.h>
#include <string.h>
#include "compat.h"

enum uio_rw
{
	UIO_READ,
	UIO_WRITE,
};

struct iovec {
	void *iov_base;
	size_t iov_len;
};

struct vmspace {
	void *dummy;
};

struct uio {
	struct iovec *uio_iov;
	int uio_iovcnt;
	off_t uio_offset;
	size_t uio_resid;
	enum uio_rw uio_rw;
	struct vmspace *uio_vmspace;

	// エミュレーション
	void *buf;
};

static inline struct uio
buf_to_uio(void *buf, size_t n, enum uio_rw rw)
{
	struct uio rv;
	memset(&rv, 0, sizeof(rv));

	rv.uio_offset = 0;
	rv.uio_resid = n;
	rv.uio_rw = rw;

	rv.buf = buf;
	return rv;
}

static inline int
uiomove(void *buf, size_t n, struct uio *uio)
{
	if (uio->uio_resid < n) {
		n = uio->uio_resid;
	}
	if (uio->uio_rw == UIO_READ) {
		memcpy(buf, (uint8_t*)uio->buf + uio->uio_offset, n);
	} else {
		memcpy((uint8_t*)uio->buf + uio->uio_offset, buf, n);
	}
	uio->uio_offset += n;
	uio->uio_resid -= n;
	return 0;
}

