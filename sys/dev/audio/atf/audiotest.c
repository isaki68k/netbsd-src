// vi:set ts=8:
/*	$NetBSD$	*/

#include <sys/cdefs.h>
__RCSID("$NetBSD$");

#include <errno.h>
#include <fcntl.h>
#define __STDC_FORMAT_MACROS	/* for PRIx64 */
#include <inttypes.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/audioio.h>
#include <sys/event.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/poll.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#if !defined(NO_RUMP)
#include <rump/rump.h>
#include <rump/rump_syscalls.h>
#endif

#if !defined(AUDIO_ENCODING_SLINEAR_NE)
#if BYTE_ORDER == LITTLE_ENDIAN
#define AUDIO_ENCODING_SLINEAR_NE AUDIO_ENCODING_SLINEAR_LE
#define AUDIO_ENCODING_ULINEAR_NE AUDIO_ENCODING_ULINEAR_LE
#define AUDIO_ENCODING_SLINEAR_OE AUDIO_ENCODING_SLINEAR_BE
#define AUDIO_ENCODING_ULINEAR_OE AUDIO_ENCODING_ULINEAR_BE
#else
#define AUDIO_ENCODING_SLINEAR_NE AUDIO_ENCODING_SLINEAR_BE
#define AUDIO_ENCODING_ULINEAR_NE AUDIO_ENCODING_ULINEAR_BE
#define AUDIO_ENCODING_SLINEAR_OE AUDIO_ENCODING_SLINEAR_LE
#define AUDIO_ENCODING_ULINEAR_OE AUDIO_ENCODING_ULINEAR_LE
#endif
#endif

struct testentry {
	const char *name;
	void (*func)(void);
};

void usage(void) __dead;
void xp_err(int, int, const char *, ...) __printflike(3, 4) __dead;
void xp_errx(int, int, const char *, ...) __printflike(3, 4) __dead;
void do_test(int);
int rump_or_open(const char *, int);
int rump_or_write(int, const void *, size_t);
int rump_or_read(int, void *, size_t);
int rump_or_ioctl(int, u_long, void *);
int rump_or_close(int);
int rump_or_fcntl(int, int, ...);
int rump_or_poll(struct pollfd *, nfds_t, int);
int rump_or_kqueue(void);
int rump_or_kevent(int, const struct kevent *, size_t,
	struct kevent *, size_t, const struct timespec *);
int hw_canplay(void);
int hw_canrec(void);
int hw_bidir(void);
int hw_fulldup(void);
int netbsd;
void init(int);
void *consumer_thread(void *);
void TEST(const char *, ...) __printflike(1, 2);
bool xp_fail(int, const char *, ...) __printflike(2, 3);
void xp_skip(int, const char *, ...) __printflike(2, 3);
bool xp_eq(int, int, int, const char *);
bool xp_eq_str(int, const char *, const char *, const char *);
bool xp_ne(int, int, int, const char *);
bool xp_sys_eq(int, int, int, const char *);
bool xp_sys_ok(int, int, const char *);
bool xp_sys_ok_ptr(int, void *, const char *);
bool xp_sys_ng(int, int, int, const char *);
bool xp_sys_if(int, bool, const char *);
bool xp_buffsize(int, bool, int, const char *);
int debug_open(int, const char *, int);
int debug_write(int, int, const void *, size_t);
int debug_read(int, int, void *, size_t);
int debug_ioctl(int, int, u_long, const char *, void *, const char *, ...)
	__printflike(6, 7);
int debug_fcntl(int, int, int, const char *, ...) __printflike(4, 5);
int debug_close(int, int);
void *debug_mmap(int, void *, size_t, int, int, int, off_t);
int debug_munmap(int, void *, int);
int debug_poll(int, struct pollfd *, int, int);
int debug_kqueue(int);
int debug_kevent_set(int, int, const struct kevent *, size_t);
int debug_kevent_poll(int, int, struct kevent *, size_t,
	const struct timespec *);
void debug_kev(int, const char *, const struct kevent *);
uid_t debug_getuid(int);
int debug_seteuid(int, uid_t);
int debug_sysctlbyname(int, const char *, void *, size_t *, const void *,
	size_t);

int openable_mode(void);
int mode2aumode(int);
int mode2play(int);
int mode2rec(int);

/* from audio.c */
static const char *encoding_names[]
__unused = {
	"none",
	AudioEmulaw,
	AudioEalaw,
	"pcm16",
	"pcm8",
	AudioEadpcm,
	AudioEslinear_le,
	AudioEslinear_be,
	AudioEulinear_le,
	AudioEulinear_be,
	AudioEslinear,
	AudioEulinear,
	AudioEmpeg_l1_stream,
	AudioEmpeg_l1_packets,
	AudioEmpeg_l1_system,
	AudioEmpeg_l2_stream,
	AudioEmpeg_l2_packets,
	AudioEmpeg_l2_system,
	AudioEac3,
};

int debug;
int props;
int hwfull;
bool opt_atf;
char testname[64];
int testcount;
int failcount;
int expfcount;
int skipcount;
int unit;
bool use_rump;
bool use_pad;
int padfd;
pthread_t th;
char devicename[16];	/* "audioN" */
char devaudio[16];	/* "/dev/audioN" */
char devsound[16];	/* "/dev/soundN" */
char devaudioctl[16];	/* "/dev/audioctlN" */
char devmixer[16];	/* "/dev/mixerN" */
extern struct testentry testtable[];

void
usage(void)
{
	fprintf(stderr, "usage:\t%s [<options>] [<testname>...]\n",
	    getprogname());
	fprintf(stderr, "\t-A        : make output suitable for ATF\n");
	fprintf(stderr, "\t-a        : Test all\n");
	fprintf(stderr, "\t-d        : Increase debug level\n");
	fprintf(stderr, "\t-l        : List all tests\n");
	fprintf(stderr, "\t-p        : Open pad\n");
#if !defined(NO_RUMP)
	fprintf(stderr, "\t-R        : Use rump (implies -p)\n");
#endif
	fprintf(stderr, "\t-u <unit> : Use audio<unit> (default:0)\n");
	exit(1);
}

/* Customized err(3) */
void
xp_err(int code, int line, const char *fmt, ...)
{
	va_list ap;
	int backup_errno;

	backup_errno = errno;
	printf(" %s %d: ", (opt_atf ? "Line" : "ERROR:"), line);
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	printf(": %s\n", strerror(backup_errno));

	exit(code);
}

/* Customized errx(3) */
void
xp_errx(int code, int line, const char *fmt, ...)
{
	va_list ap;

	printf(" %s %d: ", (opt_atf ? "Line" : "ERROR:"), line);
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	printf("\n");

	exit(code);
}

int
main(int argc, char *argv[])
{
	int i;
	int j;
	int c;
	enum {
		CMD_TEST,
		CMD_ALL,
		CMD_LIST,
	} cmd;
	bool found;

	props = -1;
	hwfull = 0;
	unit = 0;
	cmd = CMD_TEST;
	use_pad = false;
	padfd = -1;

	while ((c = getopt(argc, argv, "AadlpRu:")) != -1) {
		switch (c) {
		case 'A':
			opt_atf = true;
			break;
		case 'a':
			cmd = CMD_ALL;
			break;
		case 'd':
			debug++;
			break;
		case 'l':
			cmd = CMD_LIST;
			break;
		case 'p':
			use_pad = true;
			break;
		case 'R':
#if !defined(NO_RUMP)
			use_rump = true;
			use_pad = true;
#else
			usage();
#endif
			break;
		case 'u':
			unit = atoi(optarg);
			break;
		default:
			usage();
		}
	}
	argc -= optind;
	argv += optind;

	if (cmd == CMD_LIST) {
		/* List all */
		for (i = 0; testtable[i].name != NULL; i++)
			printf("%s\n", testtable[i].name);
		return 0;
	}

	init(unit);

	if (cmd == CMD_ALL) {
		/* Test all */
		if (argc > 0)
			usage();
		for (i = 0; testtable[i].name != NULL; i++)
			do_test(i);
	} else {
		/* Test only matched */
		if (argc == 0)
			usage();

		found = false;
		for (j = 0; j < argc; j++) {
			for (i = 0; testtable[i].name != NULL; i++) {
				if (strncmp(argv[j], testtable[i].name,
				    strlen(argv[j])) == 0) {
					do_test(i);
					found = true;
				}
			}
		}
		if (!found) {
			printf("test not found\n");
			exit(1);
		}
	}

	if (opt_atf == false) {
		printf("Result: %d tests, %d success",
		    testcount,
		    testcount - failcount - expfcount - skipcount);
		if (failcount > 0)
			printf(", %d failed", failcount);
		if (expfcount > 0)
			printf(", %d expected failure", expfcount);
		if (skipcount > 0)
			printf(", %d skipped", skipcount);
		printf("\n");
	}

	if (skipcount > 0)
		return 2;
	if (failcount > 0)
		return 1;

	return 0;
}

void
do_test(int testnumber)
{
	/* Sentinel */
	strlcpy(testname, "<NoName>", sizeof(testname));
	/* Do test */
	testtable[testnumber].func();
}

/*
 * system call wrappers for rump.
 */

/* open(2) or rump_sys_open(3) */
int
rump_or_open(const char *filename, int flag)
{
	int r;

#if !defined(NO_RUMP)
	if (use_rump)
		r = rump_sys_open(filename, flag);
	else
#endif
		r = open(filename, flag);
	return r;
}

/* write(2) or rump_sys_write(3) */
int
rump_or_write(int fd, const void *buf, size_t len)
{
	int r;

#if !defined(NO_RUMP)
	if (use_rump)
		r = rump_sys_write(fd, buf, len);
	else
#endif
		r = write(fd, buf, len);
	return r;
}

/* read(2) or rump_sys_read(3) */
int
rump_or_read(int fd, void *buf, size_t len)
{
	int r;

#if !defined(NO_RUMP)
	if (use_rump)
		r = rump_sys_read(fd, buf, len);
	else
#endif
		r = read(fd, buf, len);
	return r;
}

/* ioctl(2) or rump_sys_ioctl(3) */
int
rump_or_ioctl(int fd, u_long cmd, void *arg)
{
	int r;

#if !defined(NO_RUMP)
	if (use_rump)
		r = rump_sys_ioctl(fd, cmd, arg);
	else
#endif
		r = ioctl(fd, cmd, arg);
	return r;
}

/* close(2) or rump_sys_close(3) */
int
rump_or_close(int fd)
{
	int r;

#if !defined(NO_RUMP)
	if (use_rump)
		r = rump_sys_close(fd);
	else
#endif
		r = close(fd);
	return r;
}

/* fcntl(2) or rump_sys_fcntl(3) */
/* XXX Supported only with no arguments for now */
int
rump_or_fcntl(int fd, int cmd, ...)
{
	int r;

#if !defined(NO_RUMP)
	if (use_rump)
		r = rump_sys_fcntl(fd, cmd);
	else
#endif
		r = fcntl(fd, cmd);
	return r;
}

/* poll(2) or rump_sys_poll(3) */
int
rump_or_poll(struct pollfd *fds, nfds_t nfds, int timeout)
{
	int r;

#if !defined(NO_RUMP)
	if (use_rump)
		r = rump_sys_poll(fds, nfds, timeout);
	else
#endif
		r = poll(fds, nfds, timeout);
	return r;
}

/* kqueue(2) or rump_sys_kqueue(3) */
int
rump_or_kqueue(void)
{
	int r;

#if !defined(NO_RUMP)
	if (use_rump)
		r = rump_sys_kqueue();
	else
#endif
		r = kqueue();
	return r;
}

/* kevent(2) or rump_sys_kevent(3) */
int
rump_or_kevent(int kq, const struct kevent *chlist, size_t nch,
	struct kevent *evlist, size_t nev,
	const struct timespec *timeout)
{
	int r;

#if !defined(NO_RUMP)
	if (use_rump)
		r = rump_sys_kevent(kq, chlist, nch, evlist, nev, timeout);
	else
#endif
		r = kevent(kq, chlist, nch, evlist, nev, timeout);
	return r;
}

int
hw_canplay(void)
{
	return (props & AUDIO_PROP_PLAYBACK) ? 1 : 0;
}

int
hw_canrec(void)
{
	return (props & AUDIO_PROP_CAPTURE) ? 1 : 0;
}

int
hw_bidir(void)
{
	return hw_canplay() & hw_canrec();
}

int
hw_fulldup(void)
{
	return (props & AUDIO_PROP_FULLDUPLEX) ? 1 : 0;
}

#define DPRINTF(fmt...) do {	\
	if (debug)		\
		printf(fmt);	\
} while (0)

#define DPRINTFF(line, fmt...) do {		\
	if (debug) {				\
		printf("  > %d: ", line);	\
		DPRINTF(fmt);			\
		fflush(stdout);			\
	}					\
} while (0)

#define DRESULT(r) do {				\
	int backup_errno = errno;		\
	if (r == -1) {				\
		DPRINTF(" = %d, err#%d %s\n",	\
		    r, backup_errno,		\
		    strerror(backup_errno));	\
	} else {				\
		DPRINTF(" = %d\n", r);		\
	}					\
	errno = backup_errno;			\
	return r;				\
} while (0)

/* pointer variants for mmap */
#define DRESULT_PTR(r) do {			\
	int backup_errno = errno;		\
	if (r == (void *)-1) {			\
		DPRINTF(" = -1, err#%d %s\n",	\
		    backup_errno,		\
		    strerror(backup_errno));	\
	} else {				\
		DPRINTF(" = %p\n", r);		\
	}					\
	errno = backup_errno;			\
	return r;				\
} while (0)


/*
 * requnit <  0: Use auto by pad (not implemented).
 * requnit >= 0: Use audio<requnit>.
 */
void
init(int requnit)
{
	struct audio_device devinfo;
	size_t len;
	int rel;
	int fd;
	int r;

	if (requnit < 0) {
		xp_errx(1, __LINE__, "requnit < 0 not implemented.");
	} else {
		unit = requnit;
	}

	/* Set device name */
	snprintf(devicename, sizeof(devicename), "audio%d", unit);
	snprintf(devaudio, sizeof(devaudio), "/dev/audio%d", unit);
	snprintf(devsound, sizeof(devsound), "/dev/sound%d", unit);
	snprintf(devaudioctl, sizeof(devaudioctl), "/dev/audioctl%d", unit);
	snprintf(devmixer, sizeof(devmixer), "/dev/mixer%d", unit);

	/*
	 * version
	 * audio2 is merged in 8.99.39.
	 */
	len = sizeof(rel);
	r = sysctlbyname("kern.osrevision", &rel, &len, NULL, 0);
	if (r == -1)
		xp_err(1, __LINE__, "sysctl kern.osrevision");
	netbsd = rel / 100000000;
	if (rel >=  899003900)
		netbsd = 9;

#if !defined(NO_RUMP)
	if (use_rump) {
		DPRINTF("  use rump\n");
		rump_init();
	}
#endif

	/*
	 * Open pad device before all accesses (including /dev/audioctl).
	 */
	if (use_pad) {

		padfd = rump_or_open("/dev/pad0", O_RDONLY);
		if (padfd == -1)
			xp_err(1, __LINE__, "rump_or_open");

		/* Create consumer thread */
		pthread_create(&th, NULL, consumer_thread, NULL);
		/* Set this thread's name */
		pthread_setname_np(pthread_self(), "main", NULL);
	}

	/*
	 * Get device properties, etc.
	 */
	fd = rump_or_open(devaudioctl, O_RDONLY);
	if (fd == -1)
		xp_err(1, __LINE__, "open %s", devaudioctl);
	r = rump_or_ioctl(fd, AUDIO_GETPROPS, &props);
	if (r == -1)
		xp_err(1, __LINE__, "AUDIO_GETPROPS");
	r = rump_or_ioctl(fd, AUDIO_GETDEV, &devinfo);
	if (r == -1)
		xp_err(1, __LINE__, "AUDIO_GETDEV");
	rump_or_close(fd);

	if (debug) {
		printf("  device = %s, %s, %s\n",
		    devinfo.name, devinfo.version, devinfo.config);
		printf("  hw props =");
		if (hw_canplay())
			printf(" playback");
		if (hw_canrec())
			printf(" capture");
		if (hw_fulldup())
			printf(" fullduplex");
		printf("\n");
	}

}

/* Consumer thread used by pad */
void *
consumer_thread(void *arg)
{
	char buf[1024];
	int r;

	pthread_setname_np(pthread_self(), "consumer", NULL);
	pthread_detach(pthread_self());

	for (;;) {
		r = read(padfd, buf, sizeof(buf));
		if (r == 0) {
			fprintf(stderr, "EOF on pad ?");
			exit(1);
		}
		if (r == -1) {
			fprintf(stderr, "consumer_thread read: %s\n",
			    strerror(errno));
			exit(1);
		}
	}

	return NULL;
}

/*
 * Support functions
 */

/* Set testname */
void
TEST(const char *name, ...)
{
	va_list ap;

	va_start(ap, name);
	vsnprintf(testname, sizeof(testname), name, ap);
	va_end(ap);
	if (opt_atf == false) {
		printf("%s\n", testname);
		fflush(stdout);
	}
}

/*
 * XP_FAIL() should be called when this test fails.
 * If caller already count up testcount, call xp_fail() instead.
 */
#define XP_FAIL(fmt...)	do {	\
	testcount++;	\
	xp_fail(__LINE__, fmt);	\
} while (0)
bool xp_fail(int line, const char *fmt, ...)
{
	va_list ap;

	if (opt_atf == false)
		printf(" FAIL ");
	printf("%d: ", line);
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	printf("\n");
	fflush(stdout);
	failcount++;

	return false;
}

/*
 * XP_SKIP() should be called when you want to skip this test.
 * If caller already count up testcount, call xp_skip() instead.
 */
#define XP_SKIP(fmt...)	do { \
	testcount++;	\
	xp_skip(__LINE__, fmt);	\
} while (0)
void xp_skip(int line, const char *fmt, ...)
{
	va_list ap;

	if (opt_atf == false)
		printf(" SKIP ");
	printf("%d: ", line);
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	printf("\n");
	fflush(stdout);
	skipcount++;
}

#define XP_EQ(exp, act)	xp_eq(__LINE__, exp, act, #act)
bool xp_eq(int line, int exp, int act, const char *varname)
{
	bool r = true;

	testcount++;
	if (exp != act) {
		r = xp_fail(line, "%s expects %d but %d", varname, exp, act);
	}
	return r;
}
#define XP_EQ_STR(exp, act) xp_eq_str(__LINE__, exp, act, #act)
bool xp_eq_str(int line, const char *exp, const char *act, const char *varname)
{
	bool r = true;

	testcount++;
	if (strcmp(exp, act) != 0) {
		r = xp_fail(line, "%s expects \"%s\" but \"%s\"",
		    varname, exp, act);
	}
	return r;
}

#define XP_NE(exp, act)	xp_ne(__LINE__, exp, act, #act)
bool xp_ne(int line, int exp, int act, const char *varname)
{
	bool r = true;

	testcount++;
	if (exp == act) {
		r = xp_fail(line, "%s expects != %d but %d", varname, exp, act);
	}
	return r;
}

/* This expects that the system call returns 'exp'. */
#define XP_SYS_EQ(exp, act)	xp_sys_eq(__LINE__, exp, act, #act)
bool xp_sys_eq(int line, int exp, int act, const char *varname)
{
	bool r = true;

	testcount++;
	if (act == -1) {
		r = xp_fail(line, "%s expects %d but -1,err#%d(%s)",
		    varname, exp, errno, strerror(errno));
	} else {
		r = xp_eq(line, exp, act, varname);
	}
	return r;
}

/*
 * This expects that system call succeeds.
 * This is useful when you expect the system call succeeds but don't know
 * the expected return value, such as open(2).
 */
#define XP_SYS_OK(act)	xp_sys_ok(__LINE__, act, #act)
bool xp_sys_ok(int line, int act, const char *varname)
{
	bool r = true;

	testcount++;
	if (act == -1) {
		r = xp_fail(line, "%s expects success but -1,err#%d(%s)",
		    varname, errno, strerror(errno));
	}
	return r;
}
#define XP_SYS_OK_PTR(act) xp_sys_ok_ptr(__LINE__, act, #act)
bool xp_sys_ok_ptr(int line, void *act, const char *varname)
{
	bool r = true;

	testcount++;
	if (act == (void *)-1) {
		r = xp_fail(line, "%s expects success but -1,err#%d(%s)",
		    varname, errno, strerror(errno));
	}
	return r;
}

/* This expects that the system call fails with 'experrno'. */
#define XP_SYS_NG(experrno, act) xp_sys_ng(__LINE__, experrno, act, #act)
bool xp_sys_ng(int line, int experrno, int act, const char *varname)
{
	bool r = true;

	testcount++;
	if (act != -1) {
		r = xp_fail(line, "%s expects -1,err#%d but %d",
		    varname, experrno, act);
	} else if (experrno != errno) {
		char acterrbuf[100];
		int acterrno = errno;
		strlcpy(acterrbuf, strerror(acterrno), sizeof(acterrbuf));
		r = xp_fail(line, "%s expects -1,err#%d(%s) but -1,err#%d(%s)",
		    varname, experrno, strerror(experrno),
		    acterrno, acterrbuf);
	}
	return r;
}

/* This expects that the system call result is expressed in expr. */
/* GCC extension */
#define XP_SYS_IF(expr) xp_sys_if(__LINE__, (expr), #expr)
bool xp_sys_if(int line, bool expr, const char *exprname)
{
	bool r = true;
	testcount++;
	if (!expr) {
		r = xp_fail(__LINE__, "(%s) is expected but err#%d(%s)",
		    exprname, errno, strerror(errno));
	}
	return r;
}

/*
 * Check ai.*.buffer_size.
 * If exp == true, it expects that buffer_size is non-zero.
 * If exp == false, it expects that buffer_size is zero.
 */
#define XP_BUFFSIZE(exp, act)	\
	xp_buffsize(__LINE__, exp, act, #act)
bool xp_buffsize(int line, bool exp, int act, const char *varname)
{
	bool r = true;

	testcount++;
	if (exp) {
		if (act == 0)
			r = xp_fail(line, "%s expects non-zero but %d",
			    varname, act);
	} else {
		if (act != 0)
			r = xp_fail(line, "%s expects zero but %d",
			    varname, act);
	}
	return r;
}

/*
 * REQUIRED_* return immediately if condition does not meet.
 */
#define REQUIRED_EQ(e, a) do { if (!XP_EQ(e, a)) return; } while (0)
#define REQUIRED_NE(e, a) do { if (!XP_NE(e, a)) return; } while (0)
#define REQUIRED_SYS_EQ(e, a) do { if (!XP_SYS_EQ(e, a)) return; } while (0)
#define REQUIRED_SYS_OK(a)    do { if (!XP_SYS_OK(a))    return; } while (0)
#define REQUIRED_SYS_IF(expr) do { if (!XP_SYS_IF(expr)) return; } while (0)


static const char *openmode_str[] = {
	"O_RDONLY",
	"O_WRONLY",
	"O_RDWR",
};


/*
 * All system calls in following tests should be called with these macros.
 */

#define OPEN(name, mode)	\
	debug_open(__LINE__, name, mode)
int debug_open(int line, const char *name, int mode)
{
	char modestr[32];
	int n;

	if ((mode & 3) != 3) {
		n = snprintf(modestr, sizeof(modestr), "%s",
		    openmode_str[mode & 3]);
	} else {
		n = snprintf(modestr, sizeof(modestr), "%d", mode & 3);
	}
	if ((mode & O_NONBLOCK))
		n += snprintf(modestr + n, sizeof(modestr) - n, "|O_NONBLOCK");

	DPRINTFF(line, "open(\"%s\", %s)", name, modestr);
	int r = rump_or_open(name, mode);
	DRESULT(r);
}

#define WRITE(fd, addr, len)	\
	debug_write(__LINE__, fd, addr, len)
int debug_write(int line, int fd, const void *addr, size_t len)
{
	DPRINTFF(line, "write(%d, %p, %zd)", fd, addr, len);
	int r = rump_or_write(fd, addr, len);
	DRESULT(r);
}

#define READ(fd, addr, len)	\
	debug_read(__LINE__, fd, addr, len)
int debug_read(int line, int fd, void *addr, size_t len)
{
	DPRINTFF(line, "read(%d, %p, %zd)", fd, addr, len);
	int r = rump_or_read(fd, addr, len);
	DRESULT(r);
}

/*
 * addrstr is the comment for debug message.
 *   int onoff = 0;
 *   ioctl(fd, SWITCH, onoff); -> IOCTL(fd, SWITCH, onoff, "off");
 */
#define IOCTL(fd, name, addr, addrfmt...)	\
	debug_ioctl(__LINE__, fd, name, #name, addr, addrfmt)
int debug_ioctl(int line, int fd, u_long name, const char *namestr,
	void *addr, const char *addrfmt, ...)
{
	char addrbuf[100];
	va_list ap;

	va_start(ap, addrfmt);
	vsnprintf(addrbuf, sizeof(addrbuf), addrfmt, ap);
	va_end(ap);
	DPRINTFF(line, "ioctl(%d, %s, %s)", fd, namestr, addrbuf);
	int r = rump_or_ioctl(fd, name, addr);
	DRESULT(r);
}

#define FCNTL(fd, name...)	\
	debug_fcntl(__LINE__, fd, name, #name)
int debug_fcntl(int line, int fd, int name, const char *namestr, ...)
{
	int r;

	switch (name) {
	 case F_GETFL:	/* no arguments */
		DPRINTFF(line, "fcntl(%d, %s)", fd, namestr);
		r = rump_or_fcntl(fd, name);
		break;
	 default:
		__unreachable();
	}
	DRESULT(r);
	return r;
}

#define CLOSE(fd)	\
	debug_close(__LINE__, fd)
int debug_close(int line, int fd)
{
	DPRINTFF(line, "close(%d)", fd);
	int r = rump_or_close(fd);
	DRESULT(r);
}

#define MMAP(ptr, len, prot, flags, fd, offset)	\
	debug_mmap(__LINE__, ptr, len, prot, flags, fd, offset)
void *debug_mmap(int line, void *ptr, size_t len, int prot, int flags, int fd,
	off_t offset)
{
	char protbuf[256];
	char flagbuf[256];
	int n;

#if !defined(NO_RUMP)
	if (use_rump)
		xp_errx(1, __LINE__, "rump doesn't support mmap");
#endif

#define ADDFLAG(buf, var, name)	do {				\
	if (((var) & (name)))					\
		n = strlcat(buf, "|" #name, sizeof(buf));	\
		var &= ~(name);					\
} while (0)

	n = 0;
	protbuf[n] = '\0';
	if (prot == 0) {
		strlcpy(protbuf, "|PROT_NONE", sizeof(protbuf));
	} else {
		ADDFLAG(protbuf, prot, PROT_EXEC);
		ADDFLAG(protbuf, prot, PROT_WRITE);
		ADDFLAG(protbuf, prot, PROT_READ);
		if (prot != 0) {
			snprintf(protbuf + n, sizeof(protbuf) - n,
			    "|prot=0x%x", prot);
		}
	}

	n = 0;
	flagbuf[n] = '\0';
	if (flags == 0) {
		strlcpy(flagbuf, "|MAP_FILE", sizeof(flagbuf));
	} else {
		ADDFLAG(flagbuf, flags, MAP_SHARED);
		ADDFLAG(flagbuf, flags, MAP_PRIVATE);
		ADDFLAG(flagbuf, flags, MAP_FIXED);
		ADDFLAG(flagbuf, flags, MAP_INHERIT);
		ADDFLAG(flagbuf, flags, MAP_HASSEMAPHORE);
		ADDFLAG(flagbuf, flags, MAP_TRYFIXED);
		ADDFLAG(flagbuf, flags, MAP_WIRED);
		ADDFLAG(flagbuf, flags, MAP_ANON);
		if (flags != 0) {
			n += snprintf(flagbuf + n, sizeof(flagbuf) - n,
			    "|flag=0x%x", flags);
		}
	}

	DPRINTFF(line, "mmap(%p, %zd, %s, %s, %d, %jd)",
	    ptr, len, protbuf + 1, flagbuf + 1, fd, offset);
	void *r = mmap(ptr, len, prot, flags, fd, offset);
	DRESULT_PTR(r);
}

#define MUNMAP(ptr, len)	\
	debug_munmap(__LINE__, ptr, len)
int debug_munmap(int line, void *ptr, int len)
{
#if !defined(NO_RUMP)
	if (use_rump)
		xp_errx(1, __LINE__, "rump doesn't support munmap");
#endif
	DPRINTFF(line, "munmap(%p, %d)", ptr, len);
	int r = munmap(ptr, len);
	DRESULT(r);
}

#define POLL(pfd, nfd, timeout)	\
	debug_poll(__LINE__, pfd, nfd, timeout)
int debug_poll(int line, struct pollfd *pfd, int nfd, int timeout)
{
	char buf[256];
	int n = 0;
	buf[n] = '\0';
	for (int i = 0; i < nfd; i++) {
		n += snprintf(buf + n, sizeof(buf) - n, "{fd=%d,events=%d}",
		    pfd[i].fd, pfd[i].events);
	}
	DPRINTFF(line, "poll(%s, %d, %d)", buf, nfd, timeout);
	int r = rump_or_poll(pfd, nfd, timeout);
	DRESULT(r);
}

#define KQUEUE()	\
	debug_kqueue(__LINE__)
int debug_kqueue(int line)
{
	DPRINTFF(line, "kqueue()");
	int r = rump_or_kqueue();
	DRESULT(r);
}

#define KEVENT_SET(kq, kev, nev)	\
	debug_kevent_set(__LINE__, kq, kev, nev)
int debug_kevent_set(int line, int kq, const struct kevent *kev, size_t nev)
{
	DPRINTFF(line, "kevent_set(%d, %p, %zd)", kq, kev, nev);
	int r = rump_or_kevent(kq, kev, nev, NULL, 0, NULL);
	DRESULT(r);
}

#define KEVENT_POLL(kq, kev, nev, ts) \
	debug_kevent_poll(__LINE__, kq, kev, nev, ts)
int debug_kevent_poll(int line, int kq, struct kevent *kev, size_t nev,
	const struct timespec *ts)
{
	char tsbuf[32];

	if (ts == NULL) {
		snprintf(tsbuf, sizeof(tsbuf), "NULL");
	} else if (ts->tv_sec == 0 && ts->tv_nsec == 0) {
		snprintf(tsbuf, sizeof(tsbuf), "0.0");
	} else {
		snprintf(tsbuf, sizeof(tsbuf), "%d.%09ld",
			(int)ts->tv_sec, ts->tv_nsec);
	}
	DPRINTFF(line, "kevent_poll(%d, %p, %zd, %s)", kq, kev, nev, tsbuf);
	int r = rump_or_kevent(kq, NULL, 0, kev, nev, ts);
	DRESULT(r);
}

#define DEBUG_KEV(name, kev)	\
	debug_kev(__LINE__, name, kev)
void debug_kev(int line, const char *name, const struct kevent *kev)
{
	char flagbuf[256];
	const char *filterbuf;
	uint32_t v;
	int n;

	n = 0;
	flagbuf[n] = '\0';
	if (kev->flags == 0) {
		strcpy(flagbuf, "|0?");
	} else {
		v = kev->flags;
		ADDFLAG(flagbuf, v, EV_ADD);
		if (v != 0)
			snprintf(flagbuf + n, sizeof(flagbuf)-n, "|0x%x", v);
	}

	switch (kev->filter) {
	 case EVFILT_READ:	filterbuf = "EVFILT_READ";	break;
	 case EVFILT_WRITE:	filterbuf = "EVFILT_WRITE";	break;
	 default:		filterbuf = "EVFILT_?";		break;
	}

	DPRINTFF(line,
	    "%s={id:%d,%s,%s,fflags:0x%x,data:0x%" PRIx64 ",udata:0x%x}\n",
	    name,
	    (int)kev->ident,
	    flagbuf + 1,
	    filterbuf,
	    kev->fflags,
	    kev->data,
	    (int)(intptr_t)kev->udata);
}

/* XXX rump? */
#define GETUID()	\
	debug_getuid(__LINE__)
uid_t debug_getuid(int line)
{
	DPRINTFF(line, "getuid");
	uid_t r = getuid();
	/* getuid() never fails */
	DPRINTF(" = %u\n", r);
	return r;
}

/* XXX rump? */
#define SETEUID(id)	\
	debug_seteuid(__LINE__, id)
int debug_seteuid(int line, uid_t id)
{
	DPRINTFF(line, "seteuid(%d)", (int)id);
	int r = seteuid(id);
	DRESULT(r);
}

#define SYSCTLBYNAME(name, oldp, oldlenp, newp, newlen)	\
	debug_sysctlbyname(__LINE__, name, oldp, oldlenp, newp, newlen)
int debug_sysctlbyname(int line, const char *name, void *oldp, size_t *oldlenp,
	const void *newp, size_t newlen)
{
	DPRINTFF(line, "sysctlbyname(\"%s\")", name);
	int r = sysctlbyname(name, oldp, oldlenp, newp, newlen);
	DRESULT(r);
}


/* Return openable mode on this hardware property */
int
openable_mode(void)
{
	if (hw_bidir())
		return O_RDWR;
	if (hw_canplay())
		return O_WRONLY;
	else
		return O_RDONLY;
}

int mode2aumode_full[] = {
	                                AUMODE_RECORD,	/* O_RDONLY */
	AUMODE_PLAY | AUMODE_PLAY_ALL,			/* O_WRONLY */
	AUMODE_PLAY | AUMODE_PLAY_ALL | AUMODE_RECORD,	/* O_RDWR   */
};

/* Convert openmode(O_*) to AUMODE_*, with hardware property */
int
mode2aumode(int mode)
{
	int aumode;

	aumode = mode2aumode_full[mode];
	if (hw_canplay() == 0)
		aumode &= ~(AUMODE_PLAY | AUMODE_PLAY_ALL);
	if (hw_canrec() == 0)
		aumode &= ~AUMODE_RECORD;
	return aumode;
}

/* Is this mode + hardware playable? */
int
mode2play(int mode)
{
	int aumode;

	aumode = mode2aumode(mode);
	return ((aumode & AUMODE_PLAY)) ? 1 : 0;
}

/* Is this mode + hardware recordable? */
int
mode2rec(int mode)
{
	int aumode;

	aumode = mode2aumode(mode);
	return ((aumode & AUMODE_RECORD)) ? 1 : 0;
}

/*
 * Tests
 */

void test_open_mode(int);
void test_open_audio(int);
void test_open_sound(int);
void test_open_simul(int, int);
void test_open_multiuser(int);
void test_rdwr_fallback(int, bool, bool);
void test_rdwr_simul(int, int);

#define DEF(name) \
	void test__ ## name (void); \
	void test__ ## name (void)

/*
 * Whether it can be open()ed with specified mode.
 */
void
test_open_mode(int mode)
{
	int fd;
	int r;

	TEST("open_mode_%s", openmode_str[mode] + 2);

	fd = OPEN(devaudio, mode);
	if (mode2aumode(mode) != 0) {
		XP_SYS_OK(fd);
	} else {
		XP_SYS_NG(ENXIO, fd);
	}

	if (fd >= 0) {
		r = CLOSE(fd);
		XP_SYS_EQ(0, r);
	}
}
DEF(open_mode_RDONLY)	{ test_open_mode(O_RDONLY); }
DEF(open_mode_WRONLY)	{ test_open_mode(O_WRONLY); }
DEF(open_mode_RDWR)	{ test_open_mode(O_RDWR);   }


/*
 * The initial parameters are always the same whenever you open /dev/audio.
 */
void
test_open_audio(int mode)
{
	struct audio_info ai;
	struct audio_info ai0;
	int fd;
	int r;
	int can_play;
	int can_rec;
	bool pbuff;
	bool rbuff;

	TEST("open_audio_%s", openmode_str[mode] + 2);

	can_play = mode2play(mode);
	can_rec  = mode2rec(mode);
	if (can_play + can_rec == 0) {
		XP_SKIP("Operation not allowed on this hardware property");
		return;
	}

	/*
	 * NetBSD7,8 always has both buffers for playback and recording.
	 * NetBSD9 only has necessary buffers.
	 */
	if (netbsd < 9) {
		pbuff = true;
		rbuff = true;
	} else {
		pbuff = can_play;
		rbuff = can_rec;
	}

	/*
	 * Open /dev/audio and check parameters
	 */
	fd = OPEN(devaudio, mode);
	REQUIRED_SYS_OK(fd);
	memset(&ai, 0, sizeof(ai));
	r = IOCTL(fd, AUDIO_GETBUFINFO, &ai, "");
	REQUIRED_SYS_EQ(0, r);

	XP_NE(0, ai.blocksize);
		/* hiwat/lowat */
	XP_EQ(mode2aumode(mode), ai.mode);
	/* ai.play */
	XP_EQ(8000, ai.play.sample_rate);
	XP_EQ(1, ai.play.channels);
	XP_EQ(AUDIO_ENCODING_ULAW, ai.play.encoding);
		/* gain */
		/* port */
	XP_EQ(0, ai.play.seek);
		/* avail_ports */
	XP_BUFFSIZE(pbuff, ai.play.buffer_size);
	XP_EQ(0, ai.play.samples);
	XP_EQ(0, ai.play.eof);
	XP_EQ(0, ai.play.pause);
	XP_EQ(0, ai.play.error);
	XP_EQ(0, ai.play.waiting);
		/* balance */
	XP_EQ(can_play, ai.play.open);
	XP_EQ(0, ai.play.active);
	/* ai.record */
	XP_EQ(8000, ai.record.sample_rate);
	XP_EQ(1, ai.record.channels);
	XP_EQ(8, ai.record.precision);
	XP_EQ(AUDIO_ENCODING_ULAW, ai.record.encoding);
		/* gain */
		/* port */
	XP_EQ(0, ai.record.seek);
		/* avail_ports */
	XP_BUFFSIZE(rbuff, ai.record.buffer_size);
	XP_EQ(0, ai.record.samples);
	XP_EQ(0, ai.record.eof);
	XP_EQ(0, ai.record.pause);
	XP_EQ(0, ai.record.error);
	XP_EQ(0, ai.record.waiting);
		/* balance */
	XP_EQ(can_rec, ai.record.open);
	/*
	 * NetBSD7,8 (may?) be active when opened in recording mode but
	 * recording has not started yet. (?)
	 * NetBSD9 is not active at that time.
	 */
	if (netbsd == 9) {
		XP_EQ(0, ai.record.active);
	}
	/* Save it */
	ai0 = ai;

	/*
	 * Change much as possible
	 */
	AUDIO_INITINFO(&ai);
	ai.blocksize = ai0.blocksize * 2;
	if (ai0.hiwat > 0)
		ai.hiwat = ai0.hiwat - 1;
	if (ai0.lowat < ai0.hiwat)
		ai.lowat = ai0.lowat + 1;
	ai.mode = ai.mode & ~AUMODE_PLAY_ALL;
	ai.play.sample_rate = 11025;
	ai.play.channels = 2;
	ai.play.precision = 16;
	ai.play.encoding = AUDIO_ENCODING_SLINEAR_LE;
	ai.play.pause = 1;
	ai.record.sample_rate = 11025;
	ai.record.channels = 2;
	ai.record.precision = 16;
	ai.record.encoding = AUDIO_ENCODING_SLINEAR_LE;
	ai.record.pause = 1;
	r = IOCTL(fd, AUDIO_SETINFO, &ai, "ai");
	REQUIRED_SYS_EQ(0, r);
	r = CLOSE(fd);
	REQUIRED_SYS_EQ(0, r);

	/*
	 * Open /dev/audio again and check
	 */
	fd = OPEN(devaudio, mode);
	REQUIRED_SYS_OK(fd);
	memset(&ai, 0, sizeof(ai));
	r = IOCTL(fd, AUDIO_GETBUFINFO, &ai, "");
	REQUIRED_SYS_EQ(0, r);

	XP_EQ(ai0.blocksize, ai.blocksize);
	XP_EQ(ai0.hiwat, ai.hiwat);
	XP_EQ(ai0.lowat, ai.lowat);
	XP_EQ(mode2aumode(mode), ai.mode);
	/* ai.play */
	XP_EQ(8000, ai.play.sample_rate);
	XP_EQ(1, ai.play.channels);
	XP_EQ(8, ai.play.precision);
	XP_EQ(AUDIO_ENCODING_ULAW, ai.play.encoding);
		/* gain */
		/* port */
	XP_EQ(0, ai.play.seek);
		/* avail_ports */
	XP_EQ(ai0.play.buffer_size, ai.play.buffer_size);
	XP_EQ(0, ai.play.samples);
	XP_EQ(0, ai.play.eof);
	XP_EQ(0, ai.play.pause);
	XP_EQ(0, ai.play.error);
	XP_EQ(0, ai.play.waiting);
		/* balance */
	XP_EQ(can_play, ai.play.open);
	XP_EQ(0, ai.play.active);
	/* ai.record */
	XP_EQ(8000, ai.record.sample_rate);
	XP_EQ(1, ai.record.channels);
	XP_EQ(8, ai.record.precision);
	XP_EQ(AUDIO_ENCODING_ULAW, ai.record.encoding);
		/* gain */
		/* port */
	XP_EQ(0, ai.record.seek);
		/* avail_ports */
	XP_EQ(ai0.record.buffer_size, ai.record.buffer_size);
	XP_EQ(0, ai.record.samples);
	XP_EQ(0, ai.record.eof);
	XP_EQ(0, ai.record.pause);
	XP_EQ(0, ai.record.error);
	XP_EQ(0, ai.record.waiting);
		/* balance */
	XP_EQ(can_rec, ai.record.open);
	if (netbsd == 9) {
		XP_EQ(0, ai.record.active);
	}

	r = CLOSE(fd);
	REQUIRED_SYS_EQ(0, r);
}
DEF(open_audio_RDONLY)	{ test_open_audio(O_RDONLY); }
DEF(open_audio_WRONLY)	{ test_open_audio(O_WRONLY); }
DEF(open_audio_RDWR)	{ test_open_audio(O_RDWR);   }

/*
 * /dev/sound inherits the initial parameters from /dev/sound and /dev/audio.
 */
void
test_open_sound(int mode)
{
	struct audio_info ai;
	struct audio_info ai0;
	int fd;
	int r;
	int can_play;
	int can_rec;
	bool pbuff;
	bool rbuff;

	TEST("open_sound_%s", openmode_str[mode] + 2);

	can_play = mode2play(mode);
	can_rec  = mode2rec(mode);
	if (can_play + can_rec == 0) {
		XP_SKIP("Operation not allowed on this hardware property");
		return;
	}

	/*
	 * NetBSD7,8 always has both buffers for playback and recording.
	 * NetBSD9 only has necessary buffers.
	 */
	if (netbsd < 9) {
		pbuff = true;
		rbuff = true;
	} else {
		pbuff = can_play;
		rbuff = can_rec;
	}

	/*
	 * At first, open /dev/audio to initialize the parameters both of
	 * playback and recording.
	 */
	if (hw_canplay()) {
		fd = OPEN(devaudio, O_WRONLY);
		REQUIRED_SYS_OK(fd);
		r = CLOSE(fd);
		REQUIRED_SYS_EQ(0, r);
	}
	if (hw_canrec()) {
		fd = OPEN(devaudio, O_RDONLY);
		REQUIRED_SYS_OK(fd);
		r = CLOSE(fd);
		REQUIRED_SYS_EQ(0, r);
	}

	/*
	 * Open /dev/sound and check the initial parameters.
	 * It should be the same with /dev/audio's initial parameters.
	 */
	fd = OPEN(devsound, mode);
	REQUIRED_SYS_OK(fd);
	memset(&ai, 0, sizeof(ai));
	r = IOCTL(fd, AUDIO_GETBUFINFO, &ai, "");
	REQUIRED_SYS_EQ(0, r);

	XP_NE(0, ai.blocksize);
		/* hiwat/lowat */
	XP_EQ(mode2aumode(mode), ai.mode);
	/* ai.play */
	XP_EQ(8000, ai.play.sample_rate);
	XP_EQ(1, ai.play.channels);
	XP_EQ(8, ai.play.precision);
	XP_EQ(AUDIO_ENCODING_ULAW, ai.play.encoding);
		/* gain */
		/* port */
	XP_EQ(0, ai.play.seek);
		/* avail_ports */
	XP_BUFFSIZE(pbuff, ai.play.buffer_size);
	XP_EQ(0, ai.play.samples);
	XP_EQ(0, ai.play.eof);
	XP_EQ(0, ai.play.pause);
	XP_EQ(0, ai.play.error);
	XP_EQ(0, ai.play.waiting);
		/* balance */
	XP_EQ(can_play, ai.play.open);
	XP_EQ(0, ai.play.active);
	/* ai.record */
	XP_EQ(8000, ai.record.sample_rate);
	XP_EQ(1, ai.record.channels);
	XP_EQ(8, ai.record.precision);
	XP_EQ(AUDIO_ENCODING_ULAW, ai.record.encoding);
		/* gain */
		/* port */
	XP_EQ(0, ai.record.seek);
		/* avail_ports */
	XP_BUFFSIZE(rbuff, ai.record.buffer_size);
	XP_EQ(0, ai.record.samples);
	XP_EQ(0, ai.record.eof);
	XP_EQ(0, ai.record.pause);
	XP_EQ(0, ai.record.error);
	XP_EQ(0, ai.record.waiting);
		/* balance */
	XP_EQ(can_rec, ai.record.open);
	/*
	 * NetBSD7,8 (may?) be active when opened in recording mode but
	 * recording has not started yet. (?)
	 * NetBSD9 is not active at that time.
	 */
	if (netbsd == 9) {
		XP_EQ(0, ai.record.active);
	}
	/* Save it */
	ai0 = ai;

	/*
	 * Change much as possible
	 */
	AUDIO_INITINFO(&ai);
	ai.blocksize = ai0.blocksize * 2;
	ai.hiwat = ai0.hiwat - 1;
	ai.lowat = ai0.lowat + 1;
	ai.mode = ai0.mode & ~AUMODE_PLAY_ALL;
	ai.play.sample_rate = 11025;
	ai.play.channels = 2;
	ai.play.precision = 16;
	ai.play.encoding = AUDIO_ENCODING_SLINEAR_LE;
	ai.play.pause = 1;
	ai.record.sample_rate = 11025;
	ai.record.channels = 2;
	ai.record.precision = 16;
	ai.record.encoding = AUDIO_ENCODING_SLINEAR_LE;
	ai.record.pause = 1;
	r = IOCTL(fd, AUDIO_SETINFO, &ai, "ai");
	REQUIRED_SYS_EQ(0, r);
	r = IOCTL(fd, AUDIO_GETBUFINFO, &ai0, "ai0");
	REQUIRED_SYS_EQ(0, r);
	r = CLOSE(fd);
	REQUIRED_SYS_EQ(0, r);

	/*
	 * Open /dev/sound again and check
	 */
	fd = OPEN(devsound, mode);
	REQUIRED_SYS_OK(fd);
	memset(&ai, 0, sizeof(ai));
	r = IOCTL(fd, AUDIO_GETBUFINFO, &ai, "");
	REQUIRED_SYS_EQ(0, r);

	XP_EQ(ai0.blocksize, ai.blocksize);
		/* hiwat/lowat */
	XP_EQ(mode2aumode(mode), ai.mode);	/* mode is reset */
	/* ai.play */
	XP_EQ(ai0.play.sample_rate, ai.play.sample_rate);	/* sticky */
	XP_EQ(ai0.play.channels, ai.play.channels);		/* sticky */
	XP_EQ(ai0.play.precision, ai.play.precision);		/* sticky */
	XP_EQ(ai0.play.encoding, ai.play.encoding);		/* sticky */
		/* gain */
		/* port */
	XP_EQ(0, ai.play.seek);
		/* avail_ports */
	XP_BUFFSIZE(pbuff, ai.play.buffer_size);
	XP_EQ(0, ai.play.samples);
	XP_EQ(0, ai.play.eof);
	XP_EQ(ai0.play.pause, ai.play.pause);			/* sticky */
	XP_EQ(0, ai.play.error);
	XP_EQ(0, ai.play.waiting);
		/* balance */
	XP_EQ(can_play, ai.play.open);
	XP_EQ(0, ai.play.active);
	/* ai.record */
	XP_EQ(ai0.record.sample_rate, ai.record.sample_rate);	/* stikey */
	XP_EQ(ai0.record.channels, ai.record.channels);		/* stikey */
	XP_EQ(ai0.record.precision, ai.record.precision);	/* stikey */
	XP_EQ(ai0.record.encoding, ai.record.encoding);		/* stikey */
		/* gain */
		/* port */
	XP_EQ(0, ai.record.seek);
		/* avail_ports */
	XP_BUFFSIZE(rbuff, ai.record.buffer_size);
	XP_EQ(0, ai.record.samples);
	XP_EQ(0, ai.record.eof);
	XP_EQ(ai0.record.pause, ai.record.pause);		/* stikey */
	XP_EQ(0, ai.record.error);
	XP_EQ(0, ai.record.waiting);
		/* balance */
	XP_EQ(can_rec, ai.record.open);
	if (netbsd == 9) {
		XP_EQ(0, ai.record.active);
	}

	r = CLOSE(fd);
	REQUIRED_SYS_EQ(0, r);
}
DEF(open_sound_RDONLY)	{ test_open_sound(O_RDONLY); }
DEF(open_sound_WRONLY)	{ test_open_sound(O_WRONLY); }
DEF(open_sound_RDWR)	{ test_open_sound(O_RDWR);   }

/*
 * Open (1) /dev/sound -> (2) /dev/audio -> (3) /dev/sound,
 * Both of /dev/audio and /dev/sound share the sticky parameters,
 * /dev/sound inherits and use it but /dev/audio initialize and use it.
 * So 2nd audio descriptor affects 3rd sound descriptor.
 */
DEF(open_sound_sticky)
{
	struct audio_info ai;
	int fd;
	int r;
	int openmode;

	TEST("open_sound_sticky");

	openmode = openable_mode();

	/* First, open /dev/sound and change encoding as a delegate */
	fd = OPEN(devsound, openmode);
	REQUIRED_SYS_OK(fd);
	AUDIO_INITINFO(&ai);
	ai.play.encoding = AUDIO_ENCODING_SLINEAR_LE;
	ai.record.encoding = AUDIO_ENCODING_SLINEAR_LE;
	r = IOCTL(fd, AUDIO_SETINFO, &ai, "");
	REQUIRED_SYS_EQ(0, r);
	r = CLOSE(fd);
	REQUIRED_SYS_EQ(0, r);

	/* Next, open /dev/audio.  It makes the encoding mulaw */
	fd = OPEN(devaudio, openmode);
	REQUIRED_SYS_OK(fd);
	r = CLOSE(fd);
	REQUIRED_SYS_EQ(0, r);

	/* And then, open /dev/sound again */
	fd = OPEN(devsound, openmode);
	REQUIRED_SYS_OK(fd);
	memset(&ai, 0, sizeof(ai));
	r = IOCTL(fd, AUDIO_GETBUFINFO, &ai, "");
	REQUIRED_SYS_EQ(0, r);
	XP_EQ(AUDIO_ENCODING_ULAW, ai.play.encoding);
	XP_EQ(AUDIO_ENCODING_ULAW, ai.record.encoding);
	r = CLOSE(fd);
	REQUIRED_SYS_EQ(0, r);
}

/*
 * Open two descriptors simultaneously.
 */
void
test_open_simul(int mode0, int mode1)
{
	struct audio_info ai;
	int fd0, fd1;
	int i;
	int r;
	int actmode;
#define AUMODE_BOTH (AUMODE_PLAY | AUMODE_RECORD)
	struct {
		int mode0;
		int mode1;
	} expfulltable[] = {
		/* expected fd0		expected fd1 (-errno expects error) */
		{ AUMODE_RECORD,	AUMODE_RECORD },	// REC, REC
		{ AUMODE_RECORD,	AUMODE_PLAY },		// REC, PLAY
		{ AUMODE_RECORD,	AUMODE_BOTH },		// REC, BOTH
		{ AUMODE_PLAY,		AUMODE_RECORD },	// PLAY, REC
		{ AUMODE_PLAY,		AUMODE_PLAY },		// PLAY, PLAY
		{ AUMODE_PLAY,		AUMODE_BOTH },		// PLAY, BOTH
		{ AUMODE_BOTH,		AUMODE_RECORD },	// BOTH, REC
		{ AUMODE_BOTH,		AUMODE_PLAY },		// BOTH, PLAY
		{ AUMODE_BOTH,		AUMODE_BOTH },		// BOTH, BOTH
	},
	exphalftable[] = {
		/* expected fd0		expected fd1 (-errno expects error) */
		{ AUMODE_RECORD,	AUMODE_RECORD },	// REC, REC
		{ AUMODE_RECORD,	-ENODEV },		// REC, PLAY
		{ AUMODE_RECORD,	-ENODEV },		// REC, BOTH
		{ AUMODE_PLAY,		-ENODEV },		// PLAY, REC
		{ AUMODE_PLAY,		AUMODE_PLAY },		// PLAY, PLAY
		{ AUMODE_PLAY,		AUMODE_PLAY },		// PLAY, BOTH
		{ AUMODE_PLAY,		-ENODEV },		// BOTH, REC
		{ AUMODE_PLAY,		AUMODE_PLAY },		// BOTH, PLAY
		{ AUMODE_PLAY,		AUMODE_PLAY },		// BOTH, BOTH
	}, *exptable;

	/* The expected values are different in half-duplex or full-duplex */
	if (hw_fulldup()) {
		exptable = expfulltable;
	} else {
		exptable = exphalftable;
	}

	TEST("open_simul_%s_%s",
	    openmode_str[mode0] + 2,
	    openmode_str[mode1] + 2);

	if (netbsd < 8) {
		XP_SKIP("Multiple open is not supported");
		return;
	}

	if (mode2aumode(mode0) == 0 || mode2aumode(mode1) == 0) {
		XP_SKIP("Operation not allowed on this hardware property");
		return;
	}

	i = mode0 * 3 + mode1;

	/* Open first one */
	fd0 = OPEN(devaudio, mode0);
	REQUIRED_SYS_OK(fd0);
	r = IOCTL(fd0, AUDIO_GETBUFINFO, &ai, "");
	REQUIRED_SYS_EQ(0, r);
	actmode = ai.mode & AUMODE_BOTH;
	XP_EQ(exptable[i].mode0, actmode);

	/* Open second one */
	fd1 = OPEN(devaudio, mode1);
	if (exptable[i].mode1 >= 0) {
		/* Case to expect to be able to open */
		REQUIRED_SYS_OK(fd1);
		r = IOCTL(fd1, AUDIO_GETBUFINFO, &ai, "");
		XP_SYS_EQ(0, r);
		if (r == 0) {
			actmode = ai.mode & AUMODE_BOTH;
			XP_EQ(exptable[i].mode1, actmode);
		}
	} else {
		/* Case to expect not to be able to open */
		XP_SYS_NG(ENODEV, fd1);
		if (fd1 == -1) {
			XP_EQ(-exptable[i].mode1, errno);
		} else {
			r = IOCTL(fd1, AUDIO_GETBUFINFO, &ai, "");
			XP_SYS_EQ(0, r);
			if (r == 0) {
				actmode = ai.mode & AUMODE_BOTH;
				XP_FAIL("expects error but %d", actmode);
			}
		}
	}

	if (fd1 >= 0) {
		r = CLOSE(fd1);
		XP_SYS_EQ(0, r);
	}

	r = CLOSE(fd0);
	XP_SYS_EQ(0, r);
}
DEF(open_simul_RDONLY_RDONLY)	{ test_open_simul(O_RDONLY, O_RDONLY);	}
DEF(open_simul_RDONLY_WRONLY)	{ test_open_simul(O_RDONLY, O_WRONLY);	}
DEF(open_simul_RDONLY_RDWR)	{ test_open_simul(O_RDONLY, O_RDWR);	}
DEF(open_simul_WRONLY_RDONLY)	{ test_open_simul(O_WRONLY, O_RDONLY);	}
DEF(open_simul_WRONLY_WRONLY)	{ test_open_simul(O_WRONLY, O_WRONLY);	}
DEF(open_simul_WRONLY_RDWR)	{ test_open_simul(O_WRONLY, O_RDWR);	}
DEF(open_simul_RDWR_RDONLY)	{ test_open_simul(O_RDWR, O_RDONLY);	}
DEF(open_simul_RDWR_WRONLY)	{ test_open_simul(O_RDWR, O_WRONLY);	}
DEF(open_simul_RDWR_RDWR)	{ test_open_simul(O_RDWR, O_RDWR);	}

/*
 * Multiuser open.
 */
void
test_open_multiuser(int multiuser)
{
	char mibname[32];
	int fd0;
	int fd1;
	int r;
	bool newval;
	bool oldval;
	size_t oldlen;
	uid_t ouid;

	TEST("open_multiuser_%d", multiuser);
	if (netbsd < 8) {
		XP_SKIP("Multiple open is not supported");
		return;
	}
	if (geteuid() != 0) {
		XP_SKIP("Must be run as a privileged user");
		return;
	}

	/*
	 * Set multiuser mode (and save the previous one).
	 * NetBSD8 has no way to determine devicename.
	 */
	snprintf(mibname, sizeof(mibname), "hw.%s.multiuser", devicename);
	newval = multiuser;
	oldlen = sizeof(oldval);
	r = SYSCTLBYNAME(mibname, &oldval, &oldlen, &newval, sizeof(newval));
	REQUIRED_SYS_EQ(0, r);

	/* Check */
	r = SYSCTLBYNAME(mibname, &newval, &oldlen, NULL, 0);
	REQUIRED_SYS_EQ(0, r);
	REQUIRED_EQ(multiuser, newval);

	/*
	 * Test1: Open as root first and then unprivileged user.
	 */

	/* At first, open as root */
	fd0 = OPEN(devaudio, openable_mode());
	REQUIRED_SYS_OK(fd0);

	ouid = GETUID();
	r = SETEUID(1);
	REQUIRED_SYS_EQ(0, r);

	/* Then, open as unprivileged user */
	fd1 = OPEN(devaudio, openable_mode());
	if (multiuser) {
		/* If multiuser, another user also can open */
		XP_SYS_OK(fd1);
	} else {
		/* If not multiuser, another user cannot open */
		XP_SYS_NG(EPERM, fd1);
	}
	if (fd1 != -1) {
		r = CLOSE(fd1);
		XP_SYS_EQ(0, r);
	}

	r = SETEUID(ouid);
	REQUIRED_SYS_EQ(0, r);

	r = CLOSE(fd0);
	XP_SYS_EQ(0, r);

	/*
	 * Test2: Open as unprivileged user first and then root.
	 */

	/* At first, open as unprivileged user */
	ouid = GETUID();
	r = SETEUID(1);
	REQUIRED_SYS_EQ(0, r);

	fd0 = OPEN(devaudio, openable_mode());
	REQUIRED_SYS_OK(fd0);

	/* Then open as root */
	r = SETEUID(ouid);
	REQUIRED_SYS_EQ(0, r);

	/* root always can open */
	fd1 = OPEN(devaudio, openable_mode());
	XP_SYS_OK(fd1);
	if (fd1 != -1) {
		r = CLOSE(fd1);
		XP_SYS_EQ(0, r);
	}

	/* Close first one as unprivileged user */
	r = SETEUID(1);
	REQUIRED_SYS_EQ(0, r);
	r = CLOSE(fd0);
	XP_SYS_EQ(0, r);
	r = SETEUID(ouid);
	REQUIRED_SYS_EQ(0, r);

	/* Restore multiuser mode */
	newval = oldval;
	r = SYSCTLBYNAME(mibname, NULL, NULL, &newval, sizeof(newval));
	REQUIRED_SYS_EQ(0, r);
}
DEF(open_multiuser_0)	{ test_open_multiuser(0); }
DEF(open_multiuser_1)	{ test_open_multiuser(1); }

/*
 * Normal playback (with PLAY_ALL).
 * It does not verify real playback data.
 */
DEF(write_PLAY_ALL)
{
	char buf[8000];
	int fd;
	int r;

	TEST("write_PLAY_ALL");

	fd = OPEN(devaudio, O_WRONLY);
	if (hw_canplay()) {
		REQUIRED_SYS_OK(fd);
	} else {
		XP_SYS_NG(ENXIO, fd);
		return;
	}

	/* mulaw 1sec silence */
	memset(buf, 0xff, sizeof(buf));
	r = WRITE(fd, buf, sizeof(buf));
	XP_SYS_EQ(sizeof(buf), r);

	r = CLOSE(fd);
	XP_SYS_EQ(0, r);
}

/*
 * Normal playback (without PLAY_ALL).
 * It does not verify real playback data.
 */
DEF(write_PLAY)
{
	struct audio_info ai;
	char *wav;
	int wavsize;
	int totalsize;
	int fd;
	int r;

	TEST("write_PLAY");

	fd = OPEN(devaudio, O_WRONLY);
	if (hw_canplay()) {
		REQUIRED_SYS_OK(fd);
	} else {
		XP_SYS_NG(ENXIO, fd);
		return;
	}

	/* Drop PLAY_ALL */
	AUDIO_INITINFO(&ai);
	ai.mode = AUMODE_PLAY;
	r = IOCTL(fd, AUDIO_SETINFO, &ai, "mode");
	REQUIRED_SYS_EQ(0, r);

	/* Check mode and get blocksize */
	r = IOCTL(fd, AUDIO_GETBUFINFO, &ai, "");
	REQUIRED_SYS_EQ(0, r);
	XP_EQ(AUMODE_PLAY, ai.mode);

	wavsize = ai.blocksize;
	wav = (char *)malloc(wavsize);
	REQUIRED_SYS_IF(wav != NULL);
	memset(wav, 0xff, wavsize);

	/* Write blocks until 1sec */
	for (totalsize = 0; totalsize < 8000; ) {
		r = WRITE(fd, wav, wavsize);
		XP_SYS_EQ(wavsize, r);
		if (r == -1)
			break;	/* XXX */
		totalsize += r;
	}

	/* XXX What should I test it? */
	/* Check ai.play.error */
	r = IOCTL(fd, AUDIO_GETBUFINFO, &ai, "");
	REQUIRED_SYS_EQ(0, r);
	XP_EQ(0, ai.play.error);

	/* Playback data is no longer necessary */
	r = IOCTL(fd, AUDIO_FLUSH, NULL, "");
	REQUIRED_SYS_EQ(0, r);

	r = CLOSE(fd);
	REQUIRED_SYS_EQ(0, r);

	free(wav);
}

/*
 * Normal recording.
 * It does not verify real recorded data.
 */
DEF(read)
{
	char buf[8000];
	int fd;
	int r;

	TEST("read");

	fd = OPEN(devaudio, O_RDONLY);
	if (hw_canrec()) {
		REQUIRED_SYS_OK(fd);
	} else {
		XP_SYS_NG(ENXIO, fd);
		return;
	}

	/* mulaw 1sec */
	r = READ(fd, buf, sizeof(buf));
	XP_SYS_EQ(sizeof(buf), r);

	r = CLOSE(fd);
	XP_SYS_EQ(0, r);
}

/*
 * Repeat open-write-close cycle.
 */
DEF(rept_write)
{
	struct timeval start, end, result;
	double res;
	char buf[8000];	/* 1sec in 8bit-mulaw,1ch,8000Hz */
	int fd;
	int r;
	int n;

	TEST("rept_write");

	if (hw_canplay() == 0) {
		XP_SKIP("This test is only for playable device");
		return;
	}

	/* XXX It may timeout on some hardware driver. */
	XP_SKIP("not yet");
	return;

	memset(buf, 0xff, sizeof(buf));
	n = 3;
	gettimeofday(&start, NULL);
	for (int i = 0; i < n; i++) {
		fd = OPEN(devaudio, O_WRONLY);
		REQUIRED_SYS_OK(fd);

		r = WRITE(fd, buf, sizeof(buf));
		XP_SYS_EQ(sizeof(buf), r);

		r = CLOSE(fd);
		XP_SYS_EQ(0, r);
	}
	gettimeofday(&end, NULL);
	timersub(&end, &start, &result);
	res = (double)result.tv_sec + (double)result.tv_usec / 1000000;
	/* Make judgement but not too strict */
	if (res >= n * 1.5) {
		XP_FAIL("expects %d sec but %4.1f sec", n, res);
		return;
	}
}

/*
 * Repeat open-read-close cycle.
 */
DEF(rept_read)
{
	struct timeval start, end, result;
	double res;
	char buf[8000];	/* 1sec in 8bit-mulaw,1ch,8000Hz */
	int fd;
	int r;
	int n;

	TEST("rept_read");

	if (hw_canrec() == 0) {
		XP_SKIP("This test is only for recordable device");
		return;
	}

	/* XXX It may timeout on some hardware driver. */
	XP_SKIP("not yet");
	return;

	n = 3;
	gettimeofday(&start, NULL);
	for (int i = 0; i < n; i++) {
		fd = OPEN(devaudio, O_RDONLY);
		REQUIRED_SYS_OK(fd);

		r = READ(fd, buf, sizeof(buf));
		XP_SYS_EQ(sizeof(buf), r);

		r = CLOSE(fd);
		XP_SYS_EQ(0, r);
	}
	gettimeofday(&end, NULL);
	timersub(&end, &start, &result);
	res = (double)result.tv_sec + (double)result.tv_usec / 1000000;
	/* Make judgement but not too strict */
	if (res >= n * 1.5) {
		XP_FAIL("expects %d sec but %4.1f sec", n, res);
		return;
	}
}

/*
 * Opening with O_RDWR on half-duplex hardware falls back to O_WRONLY.
 * expwrite: expected to be able to play.
 * expread : expected to be able to recored.
 */
void
test_rdwr_fallback(int openmode, bool expwrite, bool expread)
{
	struct audio_info ai;
	char buf[10];
	int fd;
	int r;

	TEST("rdwr_fallback_%s", openmode_str[openmode] + 2);

	if (hw_bidir() == 0) {
		XP_SKIP("This test is only for bi-directional device");
		return;
	}

	AUDIO_INITINFO(&ai);
	ai.play.pause = 1;
	ai.record.pause = 1;

	fd = OPEN(devaudio, openmode);
	REQUIRED_SYS_OK(fd);

	/* Set pause not to play noise */
	r = IOCTL(fd, AUDIO_SETINFO, &ai, "pause");
	REQUIRED_SYS_EQ(0, r);

	memset(buf, 0xff, sizeof(buf));
	r = WRITE(fd, buf, sizeof(buf));
	if (expwrite) {
		XP_SYS_EQ(sizeof(buf), r);
	} else {
		XP_SYS_NG(EBADF, r);
	}

	r = READ(fd, buf, 0);
	if (expread) {
		XP_SYS_EQ(0, r);
	} else {
		XP_SYS_NG(EBADF, r);
	}

	r = CLOSE(fd);
	REQUIRED_SYS_EQ(0, r);
}
DEF(rdwr_fallback_RDONLY) { test_rdwr_fallback(O_RDONLY, false, true); }
DEF(rdwr_fallback_WRONLY) { test_rdwr_fallback(O_WRONLY, true, false); }
DEF(rdwr_fallback_RDWR) {
	bool expread;
	/*
	 * On NetBSD7, O_RDWR on half-duplex is accepted. It's possible to
	 * read and write if they don't occur at the same time.
	 * On NetBSD9, O_RDWR on half-duplex falls back O_WRONLY.
	 */
	if (netbsd < 8) {
		expread = true;
	} else {
		expread = hw_fulldup() ? true : false;
	}
	test_rdwr_fallback(O_RDWR, true, expread);
}

/*
 * Read/Write the second descriptors.
 * On full-duplex hardware, the second descriptor's readablity/writability
 * is not depend on the first descriptor's open mode.
 * On half-duplex hardware, it depends on the first descriptor's open mode.
 */
void
test_rdwr_simul(int mode0, int mode1)
{
	struct audio_info ai;
	char wbuf[100];	/* 1/80sec in 8bit-mulaw,1ch,8000Hz */
	char rbuf[100];	/* 1/80sec in 8bit-mulaw,1ch,8000Hz */
	bool canopen;
	bool canwrite;
	bool canread;
	int fd0;
	int fd1;
	int r;
	struct {
		bool canopen;
		bool canwrite;
		bool canread;
	} exptable_full[] = {
	/*	open write read	   1st, 2nd mode */
		{ 1, 0, 1 },	/* REC, REC */
		{ 1, 1, 0 },	/* REC, PLAY */
		{ 1, 1, 1 },	/* REC, BOTH */
		{ 1, 0, 1 },	/* PLAY, REC */
		{ 1, 1, 0 },	/* PLAY, PLAY */
		{ 1, 1, 1 },	/* PLAY, BOTH */
		{ 1, 0, 1 },	/* BOTH, REC */
		{ 1, 1, 0 },	/* BOTH, PLAY */
		{ 1, 1, 1 },	/* BOTH, BOTH */
	},
	exptable_half[] = {
		{ 1, 0, 1 },	/* REC, REC */
		{ 0, 0, 0 },	/* REC, PLAY */
		{ 0, 0, 0 },	/* REC, BOTH */
		{ 0, 0, 0 },	/* PLAY, REC */
		{ 1, 1, 0 },	/* PLAY, PLAY */
		{ 1, 1, 0 },	/* PLAY, BOTH */
		{ 0, 0, 0 },	/* BOTH, REC */
		{ 1, 1, 0 },	/* BOTH, PLAY */
		{ 0, 0, 0 },	/* BOTH, BOTH */
	}, *exptable;

	TEST("rdwr_simul_%s_%s",
	    openmode_str[mode0] + 2,
	    openmode_str[mode1] + 2);

	if (netbsd < 8) {
		XP_SKIP("Multiple open is not supported");
		return;
	}

	exptable = hw_fulldup() ? exptable_full : exptable_half;

	canopen  = exptable[mode0 * 3 + mode1].canopen;
	canwrite = exptable[mode0 * 3 + mode1].canwrite;
	canread  = exptable[mode0 * 3 + mode1].canread;

	if (!canopen) {
		XP_SKIP("This combination is not openable on half duplex");
		return;
	}

	fd0 = OPEN(devaudio, mode0);
	REQUIRED_SYS_OK(fd0);

	fd1 = OPEN(devaudio, mode1);
	REQUIRED_SYS_OK(fd1);

	/* Silent data to make no sound */
	memset(&wbuf, 0xff, sizeof(wbuf));
	/* Pause to make no sound */
	AUDIO_INITINFO(&ai);
	ai.play.pause = 1;
	r = IOCTL(fd0, AUDIO_SETINFO, &ai, "pause");
	XP_SYS_EQ(0, r);

	/* write(fd1) */
	r = WRITE(fd1, wbuf, sizeof(wbuf));
	if (canwrite) {
		XP_SYS_EQ(100, r);
	} else {
		XP_SYS_NG(EBADF, r);
	}

	/* read(fd1) */
	r = READ(fd1, rbuf, sizeof(rbuf));
	if (canread) {
		XP_SYS_EQ(100, r);
	} else {
		XP_SYS_NG(EBADF, r);
	}

	r = CLOSE(fd0);
	XP_SYS_EQ(0, r);
	r = CLOSE(fd1);
	XP_SYS_EQ(0, r);
}
DEF(rdwr_simul_RDONLY_RDONLY)	{ test_rdwr_simul(O_RDONLY, O_RDONLY);	}
DEF(rdwr_simul_RDONLY_WRONLY)	{ test_rdwr_simul(O_RDONLY, O_WRONLY);	}
DEF(rdwr_simul_RDONLY_RDWR)	{ test_rdwr_simul(O_RDONLY, O_RDWR);	}
DEF(rdwr_simul_WRONLY_RDONLY)	{ test_rdwr_simul(O_WRONLY, O_RDONLY);	}
DEF(rdwr_simul_WRONLY_WRONLY)	{ test_rdwr_simul(O_WRONLY, O_WRONLY);	}
DEF(rdwr_simul_WRONLY_RDWR)	{ test_rdwr_simul(O_WRONLY, O_RDWR);	}
DEF(rdwr_simul_RDWR_RDONLY)	{ test_rdwr_simul(O_RDWR, O_RDONLY);	}
DEF(rdwr_simul_RDWR_WRONLY)	{ test_rdwr_simul(O_RDWR, O_WRONLY);	}
DEF(rdwr_simul_RDWR_RDWR)	{ test_rdwr_simul(O_RDWR, O_RDWR);	}

/*
 * DRAIN should work even on incomplete data left.
 */
DEF(drain_incomplete)
{
	struct audio_info ai;
	int r;
	int fd;

	TEST("drain_incomplete");

	if (hw_canplay() == 0) {
		XP_SKIP("This test is only for playable device");
		return;
	}

	fd = OPEN(devaudio, O_WRONLY);
	REQUIRED_SYS_OK(fd);

	AUDIO_INITINFO(&ai);
	/* let precision > 8 */
	ai.play.encoding = AUDIO_ENCODING_SLINEAR_LE;
	ai.play.precision = 16;
	ai.mode = AUMODE_PLAY;
	r = IOCTL(fd, AUDIO_SETINFO, &ai, "");
	REQUIRED_SYS_EQ(0, r);
	/* Write one byte and then close */
	r = WRITE(fd, &r, 1);
	XP_SYS_EQ(1, r);
	r = CLOSE(fd);
	XP_SYS_EQ(0, r);
}

/*
 * DRAIN should work even in pause.
 */
DEF(drain_pause)
{
	struct audio_info ai;
	int r;
	int fd;

	TEST("drain_pause");

	if (hw_canplay() == 0) {
		XP_SKIP("This test is only for playable device");
		return;
	}

	fd = OPEN(devaudio, O_WRONLY);
	REQUIRED_SYS_OK(fd);

	/* Set pause */
	AUDIO_INITINFO(&ai);
	ai.play.pause = 1;
	r = IOCTL(fd, AUDIO_SETINFO, &ai, "");
	XP_SYS_EQ(0, r);
	/* Write some data and then close */
	r = WRITE(fd, &r, 4);
	XP_SYS_EQ(4, r);
	r = CLOSE(fd);
	XP_SYS_EQ(0, r);
}

/*
 * DRAIN does not affect for record-only descriptor.
 */
DEF(drain_onrec)
{
	int fd;
	int r;

	TEST("drain_onrec");

	if (hw_canrec() == 0) {
		XP_SKIP("This test is only for recordable device");
		return;
	}

	fd = OPEN(devaudio, O_RDONLY);
	REQUIRED_SYS_OK(fd);

	r = IOCTL(fd, AUDIO_DRAIN, NULL, "");
	XP_SYS_EQ(0, r);

	r = CLOSE(fd);
	XP_SYS_EQ(0, r);
}


#define ENT(x) { #x, test__ ## x }
struct testentry testtable[] = {
	ENT(open_mode_RDONLY),
	ENT(open_mode_WRONLY),
	ENT(open_mode_RDWR),
	ENT(open_audio_RDONLY),
	ENT(open_audio_WRONLY),
	ENT(open_audio_RDWR),
	ENT(open_sound_RDONLY),
	ENT(open_sound_WRONLY),
	ENT(open_sound_RDWR),
	ENT(open_sound_sticky),
	ENT(open_simul_RDONLY_RDONLY),
	ENT(open_simul_RDONLY_WRONLY),
	ENT(open_simul_RDONLY_RDWR),
	ENT(open_simul_WRONLY_RDONLY),
	ENT(open_simul_WRONLY_WRONLY),
	ENT(open_simul_WRONLY_RDWR),
	ENT(open_simul_RDWR_RDONLY),
	ENT(open_simul_RDWR_WRONLY),
	ENT(open_simul_RDWR_RDWR),
	ENT(open_multiuser_0),
	ENT(open_multiuser_1),
	ENT(write_PLAY_ALL),
	ENT(write_PLAY),
	ENT(read),
	ENT(rept_write),
	ENT(rept_read),
	ENT(rdwr_fallback_RDONLY),
	ENT(rdwr_fallback_WRONLY),
	ENT(rdwr_fallback_RDWR),
	ENT(rdwr_simul_RDONLY_RDONLY),
	ENT(rdwr_simul_RDONLY_WRONLY),
	ENT(rdwr_simul_RDONLY_RDWR),
	ENT(rdwr_simul_WRONLY_RDONLY),
	ENT(rdwr_simul_WRONLY_WRONLY),
	ENT(rdwr_simul_WRONLY_RDWR),
	ENT(rdwr_simul_RDWR_RDONLY),
	ENT(rdwr_simul_RDWR_WRONLY),
	ENT(rdwr_simul_RDWR_RDWR),
	ENT(drain_incomplete),
	ENT(drain_pause),
	ENT(drain_onrec),
	{ NULL, NULL },
};
