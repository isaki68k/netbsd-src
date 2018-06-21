/*
 * userland emulation
 */

#pragma once

#include <string.h>
#include <errno.h>
#include <util.h>
// audioio.h 中の SLINEAR_NE とかを define させるため一時的に _KERNEL をつける
#define _KERNEL
#include "../../sys/audioio.h"
#undef _KERNEL
#include <sys/event.h>
#include <sys/fcntl.h>
#include <sys/filio.h>
#include <sys/mman.h>
#include <sys/param.h>
#include <sys/poll.h>
#include <sys/queue.h>
#include <sys/stat.h>
#include <sys/sysctl.h>

// アサートするとき定義
#define AUDIO_ASSERT

// ユーザランドはグローバル変数 audio_blk_ms を使う。
// デフォルト 40msec で -m オプションで変更可能。
#define AUDIO_BLK_MS audio_blk_ms

#ifdef AUDIO_ASSERT
#define KASSERT(expr)	do {\
	if (!(expr)) panic(#expr);\
} while (0)
#define KASSERTMSG(expr, fmt, ...)	do {\
	if (!(expr)) panic(fmt, ## __VA_ARGS__);\
} while (0)
#else
#define KASSERT(expr)	/**/
#define KASSERTMSG(expr, fmt, ...)	/**/
#endif

/*
 * sys/audioio.h にある他の定数は compat.h で定義する。
 * NetBSD 側プログラムは定数がシステムと一致してないといけないため。
 */
#define AUDIO_ENCODING_RAWBYTE		32767	/* これは AUDIO2 独自 */

/* サポートする最大のチャンネル数 */
#define AUDIO_MAX_CHANNELS	12

#ifndef AudioCvirtchan
#define AudioCvirtchan "vchan"
#endif

#define IO_NDELAY	0

#define MODULE(a, b, c)

typedef struct kcondvar kcondvar_t;
typedef struct kmutex kmutex_t;

struct kcondvar {
	volatile int v;
};

struct kmutex {
	volatile int v;
};

struct file {
	void *f_audioctx;
	int f_flag;
	kauth_cred_t f_cred;
};
struct lwp {
	int l_lid;
};
struct device {
};
typedef struct device *device_t;


struct audio_params {
	int sample_rate;
	int channels;
	int encoding;
	int validbits;
	int precision;
};
typedef struct audio_params audio_params_t;

// audio_params 定義後 audio_hw_if より前…
#include "aufilter.h"

/* <dev/audio_if.h> */
#define SOUND_DEVICE		0
#define AUDIO_DEVICE		0x80
#define AUDIOCTL_DEVICE		0xc0
#define MIXER_DEVICE		0x10
#define AUDIODEV(x)		(minor(x)&0xf0)
#define ISDEVSOUND(x)		(AUDIODEV((x)) == SOUND_DEVICE)
#define ISDEVAUDIO(x)		(AUDIODEV((x)) == AUDIO_DEVICE)
typedef struct { } stream_fetcher_t;
typedef struct { } stream_filter_list_t;
typedef struct { } stream_filter_factory_t;

#define	AUFMT_INVALIDATE(fmt)	(fmt)->mode |= 0x80000000
#define	AUFMT_VALIDATE(fmt)	(fmt)->mode &= 0x7fffffff
#define	AUFMT_IS_VALID(fmt)	(((fmt)->mode & 0x80000000) == 0)

struct audio_hw_if {
	int	(*open)(void *, int);	/* open hardware */
	void	(*close)(void *);	/* close hardware */
	int	(*drain)(void *);	/* Optional: drain buffers */

	/* Encoding. */
	/* XXX should we have separate in/out? */
	int	(*query_encoding)(void *, audio_encoding_t *);

	/* Set the audio encoding parameters (record and play).
	 * Return 0 on success, or an error code if the
	 * requested parameters are impossible.
	 * The values in the params struct may be changed (e.g. rounding
	 * to the nearest sample rate.)
	 */
	int	(*set_params)(void *, int, int, audio_params_t *,
		    audio_params_t *, stream_filter_list_t *,
		    stream_filter_list_t *);

	/* Hardware may have some say in the blocksize to choose */
	int	(*round_blocksize)(void *, int, int, const audio_params_t *);

	/*
	 * Changing settings may require taking device out of "data mode",
	 * which can be quite expensive.  Also, audiosetinfo() may
	 * change several settings in quick succession.  To avoid
	 * having to take the device in/out of "data mode", we provide
	 * this function which indicates completion of settings
	 * adjustment.
	 */
	int	(*commit_settings)(void *);

	/* Start input/output routines. These usually control DMA. */
	int	(*init_output)(void *, void *, int);
	int	(*init_input)(void *, void *, int);
	int	(*start_output)(void *, void *, int,
				    void (*)(void *), void *);
	int	(*start_input)(void *, void *, int,
				   void (*)(void *), void *);
	int	(*halt_output)(void *);
	int	(*halt_input)(void *);

	int	(*speaker_ctl)(void *, int);
#define SPKR_ON		1
#define SPKR_OFF	0

	int	(*getdev)(void *, struct audio_device *);
	int	(*setfd)(void *, int);

	/* Mixer (in/out ports) */
	int	(*set_port)(void *, mixer_ctrl_t *);
	int	(*get_port)(void *, mixer_ctrl_t *);

	int	(*query_devinfo)(void *, mixer_devinfo_t *);

	/* Allocate/free memory for the ring buffer. Usually malloc/free. */
	void	*(*allocm)(void *, int, size_t);
	void	(*freem)(void *, void *, size_t);
	size_t	(*round_buffersize)(void *, int, size_t);
	paddr_t	(*mappage)(void *, void *, off_t, int);

	int	(*get_props)(void *); /* device properties */

	int	(*trigger_output)(void *, void *, void *, int,
		    void (*)(void *), void *, const audio_params_t *);
	int	(*trigger_input)(void *, void *, void *, int,
		    void (*)(void *), void *, const audio_params_t *);
	int	(*dev_ioctl)(void *, u_long, void *, int, struct lwp *);
	void	(*get_locks)(void *, kmutex_t **, kmutex_t **);

	int (*get_format)(void *, audio_format_t *);
	int	(*init_format)(void *, int,
		    const audio_format_t *, const audio_format_t *,
		    audio_filter_reg_t *, audio_filter_reg_t *);
};
struct audio_attach_args {
	int type;
	const struct audio_hw_if *hwif;
	void *hdl;
};
#define	AUDIODEV_TYPE_AUDIO	0
#define	AUDIODEV_TYPE_MIDI	1
#define AUDIODEV_TYPE_OPL	2
#define AUDIODEV_TYPE_MPU	3
#define AUDIODEV_TYPE_AUX	4

struct audio_softc;

void audio_softc_init(const audio_format2_t *, const audio_format2_t *);

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

static inline void
cv_init(kcondvar_t *cv, const char *msg)
{
	cv->v = 0;
}

static inline void
cv_destroy(kcondvar_t *cv)
{
	// nop
}

static inline void
cv_broadcast(kcondvar_t *cv)
{
	cv->v = 1;
}

int cv_wait_sig(kcondvar_t *cv, kmutex_t *lock);

static inline int
mutex_owned(kmutex_t *mutex)
{
	return (mutex->v != 0);
}

static inline void
mutex_enter(kmutex_t *mutex)
{
	mutex->v = 1;
}

static inline void
mutex_exit(kmutex_t *mutex)
{
	mutex->v = 0;
}

static inline int
mutex_tryenter(kmutex_t *mutex)
{
	if (mutex_owned(mutex)) {
		return 0;
	} else {
		mutex_enter(mutex);
		return 1;
	}
}

#define M_NOWAIT	(0)
static inline void *
kern_malloc(size_t size, int flags)
{
	return malloc(size);
}

static inline void *
kern_realloc(void *ptr, size_t size, int flags)
{
	return realloc(ptr, size);
}

static inline void
kern_free(void *ptr)
{
	free(ptr);
}

#define KM_SLEEP	(0)

static inline void *
kmem_alloc(size_t size, int flags)
{
	return kern_malloc(size, 0);
}

static inline void *
kmem_zalloc(size_t size, int flags)
{
	void *p;
	p = kern_malloc(size, 0);
	if (p)
		memset(p, 0, size);
	return p;
}

static inline void
kmem_free(void *p, size_t size)
{
	kern_free(p);
}

void aprint_error_dev(device_t, const char *fmt, ...);
void aprint_normal_dev(device_t, const char *fmt, ...);

static inline uint32_t
atomic_cas_32(volatile uint32_t *ptr, uint32_t expected, uint32_t newvalue)
{
	uint32_t rv = *ptr;
	if (rv == expected) {
		*ptr = newvalue;
	}
	return rv;
}


/* <sys/intr.h> */
#define SOFTINT_SERIAL 3
#define SOFTINT_MPSAFE 0x100

struct softintr_XXX
{
	void (*func)(void *);
	void *arg;
};

static inline void *
softint_establish(int level, void(*fun)(void *), void *arg)
{
	struct softintr_XXX *rv = malloc(sizeof(struct softintr_XXX));
	rv->func = fun;
	rv->arg = arg;
	return rv;
}

static inline void
softint_disestablish(void *cookie)
{
	free(cookie);
}

static inline void
softint_schedule(void *cookie)
{
	struct softintr_XXX *intr = cookie;
	intr->func(intr->arg);
}

static inline void
kpreempt_enable(void)
{
}

static inline void
kpreempt_disable(void)
{
}

#define PAGE_SIZE	(4096)
#define UVM_MAPFLAG(a,b,c,d,e)	(0)
typedef off_t voff_t;
typedef unsigned int uvm_flag_t;
struct vm_map {
};
struct uvm_object {
};

extern struct vm_map *kernel_map;

static inline int
uvm_map(struct vm_map *map, vaddr_t *startp, vsize_t size,
	struct uvm_object *uobj, voff_t uoffset, vsize_t align, uvm_flag_t flags)
{
	void *p;
	p = malloc(size);
	if (p == NULL)
		return ENOMEM;
	*startp = (vaddr_t)p;
	return 0;
}

static inline void
uvm_unmap(struct vm_map *map, vaddr_t start, vaddr_t end)
{
	free((void *)start);
}

static inline int
uvm_map_pageable(struct vm_map *map, vaddr_t start, vaddr_t end,
	bool new_pageable, int lockflags)
{
	return 0;
}

static inline struct uvm_object *
uao_create(vsize_t size, int flags)
{
	return NULL;
}

static inline void
uao_detach(struct uvm_object *uobj)
{
}

static inline int
cpu_intr_p()
{
	return 0;
}

static inline uint32_t
atomic_swap_32(volatile uint32_t *var, uint32_t newval)
{
	uint32_t oldval;
	oldval = *var;
	*var = newval;
	return oldval;
}

extern struct lwp *curlwp;

typedef struct { } cfdata_t;
enum devact { DUMMY };
typedef struct { } modcmd_t;
struct tty { };
#define CFATTACH_DECL3_NEW(a, b, c,d,e,f,g,h,i)
#define aprint_error(fmt...)	printf(fmt)
#define aprint_normal(fmt...)	printf(fmt)
#define aprint_naive(fmt...)	printf(fmt)
#define vdevgone(a,b,c,d)

/* sys/conf.h */
struct knote;
struct cdevsw {
	int		(*d_open)(dev_t, int, int, struct lwp *);
	int		(*d_close)(dev_t, int, int, struct lwp *);
	int		(*d_read)(dev_t, struct uio *, int);
	int		(*d_write)(dev_t, struct uio *, int);
	int		(*d_ioctl)(dev_t, u_long, void *, int, struct lwp *);
	void		(*d_stop)(struct tty *, int);
	struct tty *	(*d_tty)(dev_t);
	int		(*d_poll)(dev_t, int, struct lwp *);
	paddr_t		(*d_mmap)(dev_t, off_t, int);
	int		(*d_kqfilter)(dev_t, struct knote *);
	int		(*d_discard)(dev_t, off_t, off_t);
	int		d_flag;
};
#define device_unit(dev)	0
#define device_xname(dev)	""
#define cdevsw_lookup_major(a)	0
#define	dev_type_open(n)	int n (dev_t, int, int, struct lwp *)
#define	noclose		NULL
#define	noread		NULL
#define	nowrite		NULL
#define	noioctl		NULL
#define	nostop		NULL
#define	notty		NULL
#define	nopoll		NULL
#define nommap		NULL
#define	nodump		NULL
#define	nosize		NULL
#define	nokqfilter	NULL
#define nodiscard	NULL

#define D_OTHER		0x0000
#define	D_MPSAFE	0x0100

/* <sys/device.h> */
#define UNCONF 1
#define device_is_active(a)	true

/* <sys/file.h> */
struct fileops {
	const char *fo_name;
	int	(*fo_read)	(struct file *, off_t *, struct uio *,
				    kauth_cred_t, int);
	int	(*fo_write)	(struct file *, off_t *, struct uio *,
				    kauth_cred_t, int);
	int	(*fo_ioctl)	(struct file *, u_long, void *);
	int	(*fo_fcntl)	(struct file *, u_int, void *);
	int	(*fo_poll)	(struct file *, int);
	int	(*fo_stat)	(struct file *, struct stat *);
	int	(*fo_close)	(struct file *);
	int	(*fo_kqfilter)	(struct file *, struct knote *);
	void	(*fo_restart)	(struct file *);
	int	(*fo_mmap)	(struct file *, off_t *, size_t, int, int *,
				 int *, struct uvm_object **, int *);
	void	(*fo_spare2)	(void);
};
int	fnullop_fcntl(struct file *, u_int, void *);
void	fnullop_restart(struct file *);

/* <sys/device.h> */
#define DVACT_DEACTIVATE	0
#define config_found(a,b,c)	(device_t)NULL
#define device_private(x) NULL
extern struct audio_softc local_sc;	/* audio_dev.c */
#define device_lookup_private(a,b) &local_sc
#define DVA_SYSTEM	0
#define device_active(a, b)	do { } while (0)

/* <sys/error.h> */
#define EMOVEFD	-6

/* <sys/filedesc.h> */
#define fd_allocfile(a,b)	0
#define fd_clone(a,b,c,d,e)	0

/* <sys/kauth.h> */
#define kauth_cred_get()	(kauth_cred_t)NULL
#define kauth_cred_geteuid(a)	0
#define kauth_cred_getegid(a)	0
#define kauth_cred_hold(a)
#define kauth_cred_free(a)

/* <sys/pmf.h> */
typedef struct { } pmf_qual_t;
#define pmf_device_register(a,b,c)	true
#define pmf_device_deregister(a)
#define pmf_event_register(a,b,c,d)	true
#define pmf_event_deregister(a,b,c,d)

/* <sys/proc.h> */
extern struct proc *curproc;
typedef struct proc {
	pid_t p_pid;
} proc_t;
extern kmutex_t *proc_lock;
#define proc_find(a)	NULL
#define psignal(a, b)	do { } while (0)

/* <sys/select.h> */
#define selinit(a)
#define seldestroy(a)
#define selrecord(a, b)
#define selnotify(a,b,c)

/* <sys/event.h> (!_KERNEL 部分) */
struct knote {
	SLIST_ENTRY(knote)	kn_selnext;	/* o: for struct selinfo */
	void *kn_hook;
	const struct filterops *kn_fop;
	int kn_filter;
	int kn_data;
};

struct filterops {
	int	f_isfd;			/* true if ident == filedescriptor */
	int	(*f_attach)	(struct knote *);
					/* called when knote is ADDed */
	void	(*f_detach)	(struct knote *);
					/* called when knote is DELETEd */
	int	(*f_event)	(struct knote *, long);
					/* called when event is triggered */
};

/* <sys/selinfo.h> */
struct selinfo {
	struct klist sel_klist;
};

/* <sys/sysctl.h> (!_KERNEL部分) */
#define sysctl_createv(a,b,c,d,e,f,g,h,i,j,k,l...)
#define	CTLTYPE_NODE	1	/* name is a node */
#define SYSCTLFN_PROTO const int *, u_int, void *, \
	size_t *, const void *, size_t, \
	const int *, struct lwp *, const struct sysctlnode *
#define SYSCTLFN_ARGS const int *name, u_int namelen, \
	void *oldp, size_t *oldlenp, \
	const void *newp, size_t newlen, \
	const int *oname, struct lwp *l, \
	const struct sysctlnode *rnode
#define SYSCTLFN_CALL(node) name, namelen, oldp, \
	oldlenp, newp, newlen, \
	oname, l, node
int	sysctl_lookup(SYSCTLFN_PROTO);


/* <uvm*h> */
#define UVM_ADV_RANDOM 0
#define uao_reference(a)

extern void audio_attach(struct audio_softc **softc, bool hw);
extern void audio_detach(struct audio_softc *sc);

extern void audio_softc_play_start(struct audio_softc *sc);
extern bool audio_softc_play_busy(struct audio_softc *sc);

extern void lock(struct audio_softc *sc);
extern void unlock(struct audio_softc *sc);

extern int audio_blk_ms;
