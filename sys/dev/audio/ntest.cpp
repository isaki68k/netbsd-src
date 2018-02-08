/*
 * 実環境での動作テスト用
 */

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <util.h>
#include <sys/audioio.h>
#include <sys/cdefs.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/param.h>
#include <sys/sysctl.h>
#include <sys/wait.h>

struct testtable {
	const char *name;
	void (*func)(void);
};

void init(int);

int debug;
int netbsd;
int props;
int hwfull;
char hwconfig[16];
char hwconfig9[16];	// audio%d
int x68k;
char testname[100];
int testcount;
int failcount;
int skipcount;
char devaudio[16];
char devsound[16];
char devaudioctl[16];
extern struct testtable testtable[];

void __attribute__((__noreturn__))
usage()
{
	// test は複数列挙できる。
	printf("usage: %s [<options>] {-a | <testname...>}\n", getprogname());
	printf("  -d: debug\n");
	printf("  -u <unit>: audio/sound device unit number (defualt:0)\n");
	printf(" testname:\n");
	for (int i = 0; testtable[i].name != NULL; i++) {
		printf("\t%s\n", testtable[i].name);
	}
	exit(1);
}

int
main(int ac, char *av[])
{
	int i;
	int c;
	int opt_all;
	int unit;

	testname[0] = '\0';
	props = -1;
	hwfull = 0;
	x68k = 0;
	unit = 0;

	// global option
	opt_all = 0;
	while ((c = getopt(ac, av, "adu:")) != -1) {
		switch (c) {
		 case 'a':
			opt_all = 1;
			break;
		 case 'd':
			debug++;
			break;
		 case 'u':
			unit = atoi(optarg);
			if (unit < 0) {
				printf("invalid device unit: %d\n", unit);
				exit(1);
			}
			break;
		 default:
			usage();
		}
	}
	ac -= optind;
	av += optind;

	init(unit);

	if (opt_all) {
		// -a なら引数なしで、全項目テスト
		if (ac > 0)
			usage();

		for (int j = 0; testtable[j].name != NULL; j++) {
			testtable[j].func();
			testname[0] = '\0';
		}
	} else {
		// -a なしなら test
		if (ac == 0)
			usage();

		// そうでなければ指定されたやつ(前方一致)を順にテスト
		for (i = 0; i < ac; i++) {
			bool found = false;
			for (int j = 0; testtable[j].name != NULL; j++) {
				if (strncmp(av[i], testtable[j].name, strlen(av[i])) == 0) {
					found = true;
					testtable[j].func();
					testname[0] = '\0';
				}
			}
			if (found == false) {
				printf("test not found: %s\n", av[i]);
				exit(1);
			}
		}
	}
	if (testcount > 0) {
		printf("Result: %d tests, %d success",
			testcount, testcount - failcount);
		if (failcount > 0)
			printf(", %d failed", failcount);
		if (skipcount > 0)
			printf(" (, %d skipped)", skipcount);
		printf("\n");
	}
	return 0;
}

// err(3) ぽい自前関数
#define err(code, fmt...)	xp_err(code, __LINE__, fmt)
void xp_err(int, int, const char *, ...) __printflike(3, 4) __dead;
void
xp_err(int code, int line, const char *fmt, ...)
{
	va_list ap;
	int backup_errno;

	backup_errno = errno;
	printf(" ERR %d: ", line);
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	printf(": %s\n", strerror(backup_errno));

	exit(code);
}

#define errx(code, fmt...)	xp_errx(code, __LINE__, fmt)
void xp_errx(int, int, const char *, ...) __printflike(3, 4) __dead;
void
xp_errx(int code, int line, const char *fmt, ...)
{
	va_list ap;

	printf(" ERR %d: ", line);
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	printf("\n");

	exit(code);
}

void
init(int unit)
{
	audio_device_t dev;
	char name[256];
	size_t len;
	int r;
	int rel;
	int fd;

	snprintf(devaudio, sizeof(devaudio), "/dev/audio%d", unit);
	snprintf(devsound, sizeof(devsound), "/dev/sound%d", unit);
	snprintf(devaudioctl, sizeof(devaudioctl), "/dev/audioctl%d", unit);
	snprintf(hwconfig9, sizeof(hwconfig9), "audio%d", unit);
	if (debug)
		printf("unit = %d\n", unit);

	// バージョンを適当に判断。
	// 7 系なら 7
	// 8 系なら 8
	// 8.99 系で AUDIO2 なら 9
	// 8.99 系でそれ以外なら 8
	len = sizeof(rel);
	r = sysctlbyname("kern.osrevision", &rel, &len, NULL, 0);
	if (r == -1)
		err(1, "sysctl: osrevision");
	netbsd = rel / 100000000;
	if (netbsd == 8) {
		len = sizeof(name);
		r = sysctlbyname("kern.version", name, &len, NULL, 0);
		if (r == -1)
			err(1, "sysctl: version");
		if (strstr(name, "AUDIO2"))
			netbsd++;
	}

	if (debug)
		printf("netbsd = %d\n", netbsd);

	// デバイスのプロパティを取得
	// GETPROPS のための open/ioctl/close が信頼できるかどうかは悩ましいが
	// とりあえずね

	fd = open(devaudio, O_WRONLY);
	if (fd == -1)
		err(1, "init: open: %s", devaudio);
	r = ioctl(fd, AUDIO_GETPROPS, &props);
	if (r == -1)
		err(1, "init:AUDIO_GETPROPS");
	hwfull = (props & AUDIO_PROP_FULLDUPLEX) ? 1 : 0;

	// NetBSD8 の sysctl 用に MD 名を取得。
	// GETDEV で得られる値の定義がないので、現物合わせでよしなに頑張るorz
	r = ioctl(fd, AUDIO_GETDEV, &dev);
	if (r == -1)
		err(1, "init:AUDIO_GETDEV");
	if (strcmp(dev.config, "eap") == 0 ||
	    strcmp(dev.config, "vs") == 0)
	{
		// config に MD デバイス名(unit 番号なし)が入っているパターン
		// o eap (VMware Player)
		// o vs (x68k)
		// サウンドカードが2つはないのでへーきへーき…
		snprintf(hwconfig, sizeof(hwconfig), "%s0", dev.config);
	} else {
		// そうでなければとりあえず
		// config に MD デバイス名(unit 番号あり)が入ってるパターン
		// o auich (VirtualBox)
		strlcpy(hwconfig, dev.config, sizeof(hwconfig));
	}

	// ショートカット
	if (strcmp(dev.config, "vs") == 0)
		x68k = 1;
	close(fd);
}

// テスト名
static inline void TEST(const char *, ...) __printflike(1, 2);
static inline void
TEST(const char *name, ...)
{
	va_list ap;

	va_start(ap, name);
	vsnprintf(testname, sizeof(testname), name, ap);
	va_end(ap);
	printf("%s\n", testname);
	fflush(stdout);
}

// 検査
#define XP_FAIL(fmt...)	xp_fail(__LINE__, fmt)
void xp_fail(int line, const char *fmt, ...)
{
	va_list ap;

	printf(" FAIL %d: %s ", line, testname);
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	printf("\n");
	fflush(stdout);
	failcount++;
}
#define XP_SKIP(fmt...)	xp_skip(__LINE__, fmt)
void xp_skip(int line, const char *fmt, ...)
{
	va_list ap;

	printf(" SKIP %d: ", line);
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	printf("\n");
	fflush(stdout);
	skipcount++;
}

#define XP_EQ(exp, act)	xp_eq(__LINE__, exp, act, #act)
void xp_eq(int line, int exp, int act, const char *varname)
{
	testcount++;
	if (exp != act)
		xp_fail(line, "%s expects %d but %d", varname, exp, act);
}
#define XP_NE(exp, act)	xp_ne(__LINE__, exp, act, #act)
void xp_ne(int line, int exp, int act, const char *varname)
{
	testcount++;
	if (exp == act)
		xp_fail(line, "%s expects != %d but %d", varname, exp, act);
}

// システムコールの結果 exp になることを期待
#define XP_SYS_EQ(exp, act)	xp_sys_eq(__LINE__, exp, act, #act)
void xp_sys_eq(int line, int exp, int act, const char *varname)
{
	testcount++;
	if (exp != act) {
		if (act == -1) {
			xp_fail(line, "%s expects %d but -1,err#%d(%s)", varname, exp,
				errno, strerror(errno));
		} else {
			xp_eq(line, exp, act, varname);
		}
	}
}
// システムコールの結果成功することを期待
// open(2) のように返ってくる成功値が分からない場合用
#define XP_SYS_OK(act)	xp_sys_ok(__LINE__, act, #act)
void xp_sys_ok(int line, int act, const char *varname)
{
	testcount++;
	if (act == -1)
		xp_fail(line, "%s expects success but -1,err#%d(%s)",
			varname, errno, strerror(errno));
}
void xp_sys_ok(int line, void *act, const char *varname)
{
	testcount++;
	if (act == (void *)-1)
		xp_fail(line, "%s expects success but -1,err#%d(%s)",
			varname, errno, strerror(errno));
}

// システムコールがexperrnoで失敗することを期待
#define XP_SYS_NG(experrno, act) xp_sys_ng(__LINE__, experrno, act, #act)
void xp_sys_ng(int line, int experrno, int act, const char *varname)
{
	testcount++;
	if (act != -1) {
		xp_fail(line, "%s expects -1,err#%d but %d",
			varname, experrno, act);
	} else if (experrno != errno) {
		char acterrbuf[100];
		int acterrno = errno;
		strlcpy(acterrbuf, strerror(acterrno), sizeof(acterrbuf));
		xp_fail(line, "%s expects -1,err#%d(%s) but -1,err#%d(%s)",
			varname, experrno, strerror(experrno),
			acterrno, acterrbuf);
	}
}
void xp_sys_ng(int line, int experrno, void *act, const char *varname)
{
	testcount++;
	if (act != (void *)-1) {
		xp_fail(line, "%s expects -1,err#%d but %p",
			varname, experrno, act);
	} else if (experrno != errno) {
		char acterrbuf[100];
		int acterrno = errno;
		strlcpy(acterrbuf, strerror(acterrno), sizeof(acterrbuf));
		xp_fail(line, "%s expects -1,err#%d(%s) but -1,err#%d(%s)",
			varname, experrno, strerror(experrno),
			acterrno, acterrbuf);
	}
}

// ai.*.buffer_size が期待通りか調べる
// bool exp が true なら buffer_size の期待値は非ゼロ、
// exp が false なら buffer_size の期待値はゼロ。
#define XP_BUFFSIZE(exp, act)	xp_buffsize(__LINE__, exp, act, #act)
void xp_buffsize(int line, bool exp, int act, const char *varname)
{
	testcount++;
	if (exp) {
		if (act == 0)
			xp_fail(line, "%s expects non-zero but %d", varname, act);
	} else {
		if (act != 0)
			xp_fail(line, "%s expects zero but %d", varname, act);
	}
}

#define DPRINTF(fmt...)	do {	\
	if (debug)	\
		printf(fmt);	\
} while (0)

#define DPRINTFF(line, fmt...)	do {	\
	if (debug) {	\
		printf("  > %d: ", line);	\
		DPRINTF(fmt);	\
		fflush(stdout);	\
	}	\
} while (0)

#define DRESULT(r)	do {	\
	int backup_errno = errno;	\
	if ((r) == -1) {	\
		DPRINTF(" = %d, err#%d %s\n",	\
			r, backup_errno, strerror(backup_errno));	\
	} else {	\
		DPRINTF(" = %d\n", r);	\
	}	\
	errno = backup_errno;	\
	return r;	\
} while (0)

// ポインタ版 (mmap)
// -1 は -1 と表示してくれたほうが分かりやすい
#define DRESULT_PTR(r)	do {	\
	int backup_errno = errno;	\
	if ((r) == (void *)-1) {	\
		DPRINTF(" = -1, err#%d %s\n",	\
			backup_errno, strerror(backup_errno));	\
	} else {	\
		DPRINTF(" = %p\n", r);	\
	}	\
	errno = backup_errno;	\
	return r;	\
} while (0)

static const char *openmodetable[] = {
	"O_RDONLY",
	"O_WRONLY",
	"O_RDWR",
};
static const char *aumodetable[] = {
	"RECORD",
	"PLAY",
	"PLAY|REC",
	"AUMODE_0",
};

// システムコールはこのマクロを経由して呼ぶ
#define OPEN(name, mode)	debug_open(__LINE__, name, mode)
int debug_open(int line, const char *name, int mode)
{
	if (0 <= mode && mode <= 2)
		DPRINTFF(line, "open(\"%s\", %s)", name, openmodetable[mode]);
	else
		DPRINTFF(line, "open(\"%s\", %d)", name, mode);
	int r = open(name, mode);
	DRESULT(r);
}

#define WRITE(fd, addr, len)	debug_write(__LINE__, fd, addr, len)
int debug_write(int line, int fd, const void *addr, size_t len)
{
	DPRINTFF(line, "write(%d, %p, %zd)", fd, addr, len);
	int r = write(fd, addr, len);
	DRESULT(r);
}

#define READ(fd, addr, len)	debug_read(__LINE__, fd, addr, len)
int debug_read(int line, int fd, void *addr, size_t len)
{
	DPRINTFF(line, "read(%d, %p, %zd)", fd, addr, len);
	int r = read(fd, addr, len);
	DRESULT(r);
}

// addrstr は値についてのコメント。ex.
//	int onoff = 0;
//	ioctl(fd, SWITCH, onoff); -> IOCTL(fd, SWITCH, onoff, "off")
#define IOCTL(fd, name, addr, addrstr)	\
	debug_ioctl(__LINE__, fd, name, #name, addr, addrstr)
int debug_ioctl(int line, int fd, u_long name, const char *namestr,
	void *addr, const char *addrstr)
{
	DPRINTFF(line, "ioctl(%d, %s, %s)", fd, namestr, addrstr);
	int r = ioctl(fd, name, addr);
	DRESULT(r);
}

#define CLOSE(fd)	debug_close(__LINE__, fd)
int debug_close(int line, int fd)
{
	DPRINTFF(line, "close(%d)", fd);
	int r = close(fd);
	DRESULT(r);
}

#define MMAP(ptr, len, prot, flags, fd, offset)	\
	debug_mmap(__LINE__, ptr, len, prot, flags, fd, offset)
void *debug_mmap(int line, void *ptr, int len, int prot, int flags, int fd,
	int offset)
{
	char protbuf[256];
	char flagbuf[256];
	int n;

#define ADDFLAG(buf, var, name)	do {	\
	if (((var) & (name)))	\
		n = strlcat(buf, "|" #name, sizeof(buf));	\
		var &= ~(name);	\
} while (0)

	n = 0;
	protbuf[n] = '\0';
	if (prot == 0) {
		strlcpy(protbuf, "|PROT_NONE", sizeof(protbuf));
	} else {
		ADDFLAG(protbuf, prot, PROT_EXEC);
		ADDFLAG(protbuf, prot, PROT_WRITE);
		ADDFLAG(protbuf, prot, PROT_READ);
		if (prot != 0)
			snprintf(protbuf + n, sizeof(protbuf) - n, "|prot=0x%x", prot);
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
		if ((flags & MAP_ALIGNMENT_MASK)) {
			n += snprintf(flagbuf + n, sizeof(flagbuf) - n, "|MAP_ALIGN(%d)",
				((flags & MAP_ALIGNMENT_MASK) >> MAP_ALIGNMENT_SHIFT));
			flags &= ~MAP_ALIGNMENT_MASK;
		}
		if (flags != 0)
			n += snprintf(flagbuf + n, sizeof(flagbuf) - n, "|flag=0x%x",
				flags);
	}

	DPRINTFF(line, "mmap(%p, %d, %s, %s, %d, %d)",
		ptr, len, protbuf + 1, flagbuf + 1, fd, offset);
	void *r = mmap(ptr, len, prot, flags, fd, offset);
	DRESULT_PTR(r);
}

#define MUNMAP(ptr, len)	debug_munmap(__LINE__, ptr, len)
int debug_munmap(int line, void *ptr, int len)
{
	DPRINTFF(line, "munmap(%p, %d)", ptr, len);
	int r = munmap(ptr, len);
	DRESULT(r);
}

#define GETUID()	debug_getuid(__LINE__)
uid_t debug_getuid(int line)
{
	DPRINTFF(line, "getuid");
	uid_t r = getuid();
	DRESULT(r);
}

#define SETEUID(id)	debug_seteuid(__LINE__, id)
int debug_seteuid(int line, uid_t id)
{
	DPRINTFF(line, "seteuid(%d)", (int)id);
	int r = seteuid(id);
	DRESULT(r);
}

#define SYSCTLBYNAME(name, oldp, oldlenp, newp, newlen, msg)	\
	debug_sysctlbyname(__LINE__, name, oldp, oldlenp, newp, newlen, msg)
int debug_sysctlbyname(int line, const char *name, void *oldp, size_t *oldlenp,
	const void *newp, size_t newlen, const char *msg)
{
	DPRINTFF(line, "sysctlbyname(\"%s\", %s)", name, msg);
	int r = sysctlbyname(name, oldp, oldlenp, newp, newlen);
	DRESULT(r);
}

#define SYSTEM(cmd)	debug_system(__LINE__, cmd)
int debug_system(int line, const char *cmd)
{
	DPRINTFF(line, "system(\"%s\")", cmd);
	int r = system(cmd);
	DRESULT(r);
}

// ---

// sysctl で使う HW デバイス名を返します。
// N8 では MD 名(vs0 とか)、
// N9 では audioN です。
const char *
hwconfigname()
{
	if (netbsd <= 8) {
		return hwconfig;
	} else {
		return hwconfig9;
	}
}

// ---

// O_* を AUMODE_* に変換。HW Full 固定
int mode2aumode_full[] = {
	AUMODE_RECORD,
	AUMODE_PLAY | AUMODE_PLAY_ALL,
	AUMODE_PLAY | AUMODE_PLAY_ALL | AUMODE_RECORD,
};
// O_* を AUMODE_* に変換 (hwfull を考慮)
int mode2aumode(int mode)
{
	int aumode = mode2aumode_full[mode];
	if (hwfull == 0 && mode == O_RDWR)
		aumode &= ~AUMODE_RECORD;
	return aumode;
}

// O_* を PLAY 側がオープンされているかに変換。HW Full 固定
int mode2popen_full[] = {
	0, 1, 1,
};
// O_* を PLAY 側がオープンされてるかに変換 (hwfull を考慮、ただし同じになる)
int mode2popen(int mode)
{
	return mode2popen_full[mode];
}

// O_* を RECORD 側がオープンされてるかに変換。HW Full 固定
int mode2ropen_full[] = {
	1, 0, 1,
};
// O_* を RECORD 側がオープンされてるかに変換 (hwfull を考慮)
int mode2ropen(int mode)
{
	int rec = mode2ropen_full[mode];
	if (hwfull == 0 && mode == O_RDWR)
		rec = 0;
	return rec;
}


// オープンモードによるオープン直後の状態を調べる
void
test_open_1(void)
{
	struct audio_info ai;
	int fd;
	int r;

	// 再生専用デバイスのテストとかはまた
	for (int mode = 0; mode <= 2; mode++) {
		TEST("open_1(%s)", openmodetable[mode]);
		fd = OPEN(devaudio, mode);
		XP_SYS_OK(fd);

		memset(&ai, 0, sizeof(ai));
		r = IOCTL(fd, AUDIO_GETBUFINFO, &ai, "");
		XP_SYS_EQ(0, r);
		XP_EQ(0, ai.play.pause);
		XP_EQ(0, ai.record.pause);
		XP_EQ(mode2popen_full[mode], ai.play.open);
		XP_EQ(mode2ropen_full[mode], ai.record.open);
		// ai.mode は open_5 で調べている

		if (netbsd <= 8) {
			// N7、N8 では使わないほうのトラックのバッファも常にある
			XP_NE(0, ai.play.buffer_size);
			XP_NE(0, ai.record.buffer_size);
		} else {
			// AUDIO2 では使わないほうのバッファは確保してない
			XP_BUFFSIZE(mode2popen_full[mode], ai.play.buffer_size);
			XP_BUFFSIZE(mode2ropen_full[mode], ai.record.buffer_size);
		}

		r = CLOSE(fd);
		XP_SYS_EQ(0, r);
	}
}

// /dev/audio は何回開いても初期値は同じ。
// /dev/audio の初期値確認、いろいろ変更して close、もう一度開いて初期値確認。
void
test_open_2(void)
{
	struct audio_info ai, ai0;
	int channels;
	int fd;
	int r;
	bool pbuff, rbuff;

	for (int mode = 0; mode <= 2; mode++) {
		TEST("open_2(%s)", openmodetable[mode]);

		// N7、N8 では常に両方のバッファが存在する
		// AUDIO2 では mode による
		if (netbsd <= 8) {
			pbuff = true;
			rbuff = true;
		} else {
			pbuff = mode2popen_full[mode];
			rbuff = mode2ropen_full[mode];
		}

		// オープンして初期値をチェック
		fd = OPEN(devaudio, mode);
		if (fd == -1)
			err(1, "open");
		memset(&ai, 0, sizeof(ai));
		r = IOCTL(fd, AUDIO_GETBUFINFO, &ai, "");
		XP_SYS_EQ(0, r);

		XP_NE(0, ai.blocksize);
		XP_NE(0, ai.hiwat);
		XP_NE(0, ai.lowat);
		XP_EQ(mode2aumode(mode), ai.mode);

		// play
		XP_EQ(8000, ai.play.sample_rate);
		XP_EQ(1, ai.play.channels);
		XP_EQ(8, ai.play.precision);
		XP_EQ(AUDIO_ENCODING_ULAW, ai.play.encoding);
		// gain
		// port
		XP_EQ(0, ai.play.seek);
		// avail_ports
		XP_BUFFSIZE(pbuff, ai.play.buffer_size);
		XP_EQ(0, ai.play.samples);
		XP_EQ(0, ai.play.eof);
		XP_EQ(0, ai.play.pause);
		XP_EQ(0, ai.play.error);
		XP_EQ(0, ai.play.waiting);
		// balance
		XP_EQ(mode2popen_full[mode], ai.play.open);
		XP_EQ(0, ai.play.active);
		// record
		XP_EQ(8000, ai.record.sample_rate);
		XP_EQ(1, ai.record.channels);
		XP_EQ(8, ai.record.precision);
		XP_EQ(AUDIO_ENCODING_ULAW, ai.record.encoding);
		// gain
		// port
		XP_EQ(0, ai.record.seek);
		// avail_ports
		XP_BUFFSIZE(rbuff, ai.record.buffer_size);
		XP_EQ(0, ai.record.samples);
		XP_EQ(0, ai.record.eof);
		XP_EQ(0, ai.record.pause);
		XP_EQ(0, ai.record.error);
		XP_EQ(0, ai.record.waiting);
		// balance
		XP_EQ(mode2ropen_full[mode], ai.record.open);
		if (netbsd <= 7) {
			// N7 は録音が有効ならオープン直後から active になるらしい。
			XP_EQ(mode2ropen(mode), ai.record.active);
		} else {
			// オープンしただけではまだアクティブにならない。
			XP_EQ(0, ai.record.active);
		}
		// これを保存しておく
		ai0 = ai;

		// できるだけ変更
		channels = (netbsd <= 7 && x68k) ? 1 : 2;
		AUDIO_INITINFO(&ai);
		if (ai0.hiwat > 0)
			ai.hiwat = ai0.hiwat - 1;
		if (ai0.lowat < ai0.hiwat)
			ai.lowat = ai0.lowat + 1;
		ai.mode = ai.mode & ~AUMODE_PLAY_ALL;
		ai.play.sample_rate = 11025;
		ai.play.channels = channels;
		ai.play.precision = 16;
		ai.play.encoding = AUDIO_ENCODING_SLINEAR_LE;
		ai.play.pause = 1;
		ai.record.sample_rate = 11025;
		ai.record.channels = channels;
		ai.record.precision = 16;
		ai.record.encoding = AUDIO_ENCODING_SLINEAR_LE;
		ai.record.pause = 1;
		r = IOCTL(fd, AUDIO_SETINFO, &ai, "ai");
		if (r == -1)
			err(1, "AUDIO_SETINFO");
		r = CLOSE(fd);
		XP_SYS_EQ(0, r);

		// 再オープンしてチェック
		fd = OPEN(devaudio, mode);
		if (fd == -1)
			err(1, "open");
		memset(&ai, 0, sizeof(ai));
		r = IOCTL(fd, AUDIO_GETBUFINFO, &ai, "");
		XP_SYS_EQ(0, r);

		XP_NE(0, ai.blocksize);
		XP_NE(0, ai.hiwat);
		XP_NE(0, ai.lowat);
		XP_EQ(mode2aumode(mode), ai.mode);
		// play
		XP_EQ(8000, ai.play.sample_rate);
		XP_EQ(1, ai.play.channels);
		XP_EQ(8, ai.play.precision);
		XP_EQ(AUDIO_ENCODING_ULAW, ai.play.encoding);
		// gain
		// port
		XP_EQ(0, ai.play.seek);
		// avail_ports
		XP_BUFFSIZE(pbuff, ai.play.buffer_size);
		XP_EQ(0, ai.play.samples);
		XP_EQ(0, ai.play.eof);
		XP_EQ(0, ai.play.pause);
		XP_EQ(0, ai.play.error);
		XP_EQ(0, ai.play.waiting);
		// balance
		XP_EQ(mode2popen_full[mode], ai.play.open);
		XP_EQ(0, ai.play.active);
		// record
		XP_EQ(8000, ai.record.sample_rate);
		XP_EQ(1, ai.record.channels);
		XP_EQ(8, ai.record.precision);
		XP_EQ(AUDIO_ENCODING_ULAW, ai.record.encoding);
		// gain
		// port
		XP_EQ(0, ai.record.seek);
		// avail_ports
		XP_BUFFSIZE(rbuff, ai.record.buffer_size);
		XP_EQ(0, ai.record.samples);
		XP_EQ(0, ai.record.eof);
		XP_EQ(0, ai.record.pause);
		XP_EQ(0, ai.record.error);
		XP_EQ(0, ai.record.waiting);
		// balance
		XP_EQ(mode2ropen_full[mode], ai.record.open);
		if (netbsd <= 7) {
			// N7 は録音が有効ならオープン直後から active になるらしい。
			XP_EQ(mode2ropen(mode), ai.record.active);
		} else {
			// オープンしただけではまだアクティブにならない。
			XP_EQ(0, ai.record.active);
		}

		r = CLOSE(fd);
		XP_SYS_EQ(0, r);
	}
}

// /dev/sound は前回の値がみえる
// /dev/audio を一旦開いて初期化しておき、
// /dev/sound の初期値がそれになることを確認、
// いろいろ変更して close、もう一度開いて残っていることを確認。
void
test_open_3(void)
{
	struct audio_info ai, ai0;
	int channels;
	int fd;
	int r;
	int aimode;
	bool pbuff, rbuff;

	// N8 eap だと panic する。
	// 録音を止めてないのか分からないけど、O_RDWR オープンですでに
	// eap の録音側が動いてるみたいな感じで怒られる。しらん。
	if (netbsd == 8 && strncmp(hwconfig, "eap", 3) == 0) {
		XP_FAIL("it causes panic on NetBSD8 + eap");
		return;
	}

	for (int mode = 0; mode <= 2; mode++) {
		TEST("open_3(%s)", openmodetable[mode]);

		// N7、N8 では常に両方のバッファが存在する
		// AUDIO2 では mode による
		if (netbsd <= 8) {
			pbuff = true;
			rbuff = true;
		} else {
			pbuff = mode2popen_full[mode];
			rbuff = mode2ropen_full[mode];
		}

		// まず /dev/audio を RDWR で開いて両方初期化させておく。
		// ただし NetBSD8 は audio と sound が分離されてるので別コード。
		if (netbsd == 8) {
			fd = OPEN(devsound, O_RDWR);
			if (fd == -1)
				err(1, "open");
			AUDIO_INITINFO(&ai);
			ai.play.encoding = AUDIO_ENCODING_ULAW;
			ai.play.precision = 8;
			ai.play.channels = 1;
			ai.play.sample_rate = 8000;
			ai.play.pause = 0;
			ai.record = ai.play;
			r = IOCTL(fd, AUDIO_SETINFO, &ai, "");
			XP_SYS_EQ(0, r);
		} else {
			fd = OPEN(devaudio, O_RDWR);
			if (fd == -1)
				err(1, "open");
		}
		r = CLOSE(fd);
		XP_SYS_EQ(0, r);

		// オープンして初期値をチェック
		fd = OPEN(devsound, mode);
		if (fd == -1)
			err(1, "open");
		memset(&ai, 0, sizeof(ai));
		r = IOCTL(fd, AUDIO_GETBUFINFO, &ai, "");
		XP_SYS_EQ(0, r);

		// audio の初期値と同じものが見えるはず
		XP_NE(0, ai.blocksize);
		// hiwat, lowat
		XP_EQ(mode2aumode(mode), ai.mode);
		aimode = ai.mode;
		// play
		XP_EQ(8000, ai.play.sample_rate);
		XP_EQ(1, ai.play.channels);
		XP_EQ(8, ai.play.precision);
		XP_EQ(AUDIO_ENCODING_ULAW, ai.play.encoding);
		// gain
		// port
		XP_EQ(0, ai.play.seek);
		// avail_ports
		XP_BUFFSIZE(pbuff, ai.play.buffer_size);
		XP_EQ(0, ai.play.samples);
		XP_EQ(0, ai.play.eof);
		XP_EQ(0, ai.play.pause);
		XP_EQ(0, ai.play.error);
		XP_EQ(0, ai.play.waiting);
		// balance
		XP_EQ(mode2popen_full[mode], ai.play.open);
		XP_EQ(0, ai.play.active);
		// record
		XP_EQ(8000, ai.record.sample_rate);
		XP_EQ(1, ai.record.channels);
		XP_EQ(8, ai.record.precision);
		XP_EQ(AUDIO_ENCODING_ULAW, ai.record.encoding);
		// gain
		// port
		XP_EQ(0, ai.record.seek);
		// avail_ports
		XP_BUFFSIZE(rbuff, ai.record.buffer_size);
		XP_EQ(0, ai.record.samples);
		XP_EQ(0, ai.record.eof);
		XP_EQ(0, ai.record.pause);
		XP_EQ(0, ai.record.error);
		XP_EQ(0, ai.record.waiting);
		// balance
		XP_EQ(mode2ropen_full[mode], ai.record.open);
		XP_EQ(0, ai.record.active);

		// できるだけ変更
		channels = (netbsd <= 7 && x68k) ? 1 : 2;
		AUDIO_INITINFO(&ai);
		ai.mode = aimode & ~AUMODE_PLAY_ALL;
		ai.play.sample_rate = 11025;
		ai.play.channels = channels;
		ai.play.precision = 16;
		ai.play.encoding = AUDIO_ENCODING_SLINEAR_LE;
		ai.play.pause = 1;
		ai.record.sample_rate = 11025;
		ai.record.channels = channels;
		ai.record.precision = 16;
		ai.record.encoding = AUDIO_ENCODING_SLINEAR_LE;
		ai.record.pause = 1;
		r = IOCTL(fd, AUDIO_SETINFO, &ai, "ai");
		if (r == -1)
			err(1, "AUDIO_SETINFO");
		r = IOCTL(fd, AUDIO_GETBUFINFO, &ai0, "ai0");
		if (r == -1)
			err(1, "AUDIO_GETBUFINFO");
		r = CLOSE(fd);
		XP_SYS_EQ(0, r);

		// 再オープンしてチェック
		fd = OPEN(devsound, mode);
		if (fd == -1)
			err(1, "open");
		memset(&ai, 0, sizeof(ai));
		r = IOCTL(fd, AUDIO_GETBUFINFO, &ai, "");
		XP_SYS_EQ(0, r);

		XP_NE(0, ai.blocksize);
		// hiwat, lowat は変化する
		// mode は引き継がない
		XP_EQ(mode2aumode(mode), ai.mode);
		// play
		XP_EQ(ai0.play.sample_rate, ai.play.sample_rate);
		XP_EQ(ai0.play.channels, ai.play.channels);
		XP_EQ(ai0.play.precision, ai.play.precision);
		XP_EQ(ai0.play.encoding, ai.play.encoding);
		// gain
		// port
		XP_EQ(0, ai.play.seek);
		// avail_ports
		XP_BUFFSIZE(pbuff, ai.play.buffer_size);
		XP_EQ(0, ai.play.samples);
		XP_EQ(0, ai.play.eof);
		XP_EQ(ai0.play.pause, ai.play.pause);
		XP_EQ(0, ai.play.error);
		XP_EQ(0, ai.play.waiting);
		// balance
		XP_EQ(mode2popen_full[mode], ai.play.open);
		XP_EQ(0, ai.play.active);
		// record
		XP_EQ(ai0.record.sample_rate, ai.record.sample_rate);
		XP_EQ(ai0.record.channels, ai.record.channels);
		XP_EQ(ai0.record.precision, ai.record.precision);
		XP_EQ(ai0.record.encoding, ai.record.encoding);
		// gain
		// port
		XP_EQ(0, ai.record.seek);
		// avail_ports
		XP_BUFFSIZE(rbuff, ai.record.buffer_size);
		XP_EQ(0, ai.record.samples);
		XP_EQ(0, ai.record.eof);
		XP_EQ(ai0.record.pause, ai.record.pause);
		XP_EQ(0, ai.record.error);
		XP_EQ(0, ai.record.waiting);
		// balance
		XP_EQ(mode2ropen_full[mode], ai.record.open);
		XP_EQ(0, ai.record.active);

		r = CLOSE(fd);
		XP_SYS_EQ(0, r);
	}
}

// /dev/sound -> /dev/audio -> /dev/sound と開くと2回目の sound は
// audio の設定に影響される。
// /dev/audio と /dev/sound の設定は互いに独立しているわけではなく、
// 内部設定は1つで、
// /dev/sound はそれを継承して使い、/dev/audio はそれを初期化して使う。
// というイメージのようだ。
void
test_open_4(void)
{
	struct audio_info ai, ai0;
	int fd;
	int r;

	TEST("open_4");

	// まず /dev/sound を開いて適当に設定。
	// 代表値でエンコーディングだけ変える。
	// このケースでだけ open_2、open_3 とは挙動が違う項目が一つだけ
	// あるとかだと捕捉できないが、さすがにいいだろう…。
	fd = OPEN(devsound, O_WRONLY);
	if (fd == -1)
		err(1, "open");
	AUDIO_INITINFO(&ai);
	ai.play.encoding = AUDIO_ENCODING_SLINEAR_LE;
	r = IOCTL(fd, AUDIO_SETINFO, &ai, "");
	if (r == -1)
		err(1, "AUDIO_SETINFO");
	CLOSE(fd);

	// 続いて /dev/audio をオープン。オープンしただけで自身は mulaw になる
	fd = OPEN(devaudio, O_WRONLY);
	if (fd == -1)
		err(1, "open");
	CLOSE(fd);

	// 再び /dev/sound をオープン。
	fd = OPEN(devsound, O_WRONLY);
	if (fd == -1)
		err(1, "open");
	memset(&ai, 0, sizeof(ai));
	r = IOCTL(fd, AUDIO_GETBUFINFO, &ai, "");
	if (r == -1)
		err(1, "AUDIO_GETBUFINFO");
	XP_EQ(AUDIO_ENCODING_ULAW, ai.play.encoding);
	CLOSE(fd);
}

// オープン、多重オープン時の mode
// N8 では HW Full/Half によらず常に full duplex かのようにオープン出来るが
// read/write がおかしいので、よくない。
// AUDIO2 では HW Half ならおかしい組み合わせは弾く。
void
test_open_5()
{
	struct audio_info ai;
	int fd0, fd1;
	int r;
	int actmode;
#define AUMODE_BOTH (AUMODE_PLAY | AUMODE_RECORD)
	struct {
		int mode0;
		int mode1;
	} expfulltable[] = {
		// fd0 の期待値		fd1 の期待値(-errnoはエラー)
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
		// fd0 の期待値		fd1 の期待値(-errnoはエラー)
		{ AUMODE_RECORD,	AUMODE_RECORD },	// REC, REC
		{ AUMODE_RECORD,	-ENODEV },			// REC, PLAY
		{ AUMODE_RECORD,	-ENODEV },			// REC, BOTH
		{ AUMODE_PLAY,		-ENODEV },			// PLAY, REC
		{ AUMODE_PLAY,		AUMODE_PLAY },		// PLAY, PLAY
		{ AUMODE_PLAY,		AUMODE_PLAY },		// PLAY, BOTH
		{ AUMODE_PLAY,		-ENODEV },			// BOTH, REC
		{ AUMODE_PLAY,		AUMODE_PLAY },		// BOTH, PLAY
		{ AUMODE_PLAY,		AUMODE_PLAY },		// BOTH, BOTH
	}, *exptable;

	// HW が Full/Half で期待値が違う
	if (hwfull) {
		exptable = expfulltable;
	} else {
		exptable = exphalftable;
	}

	// 1本目をオープン
	for (int i = 0; i <= 2; i++) {
		for (int j = 0; j <= 2; j++) {
			TEST("open_5(%s,%s)",
				openmodetable[i], openmodetable[j]);

			// 1本目
			fd0 = OPEN(devaudio, i);
			if (fd0 == -1)
				err(1, "open");
			r = IOCTL(fd0, AUDIO_GETBUFINFO, &ai, "");
			if (r == -1)
				err(1, "ioctl");
			actmode = ai.mode & (AUMODE_PLAY | AUMODE_RECORD);
			XP_EQ(exptable[i * 3 + j].mode0, actmode);

			// N7 では多重オープンはできない
			if (netbsd >= 8) {
				// 2本目
				fd1 = OPEN(devaudio, j);
				if (exptable[i * 3 + j].mode1 >= 0) {
					// オープンできることを期待するケース
					XP_SYS_OK(fd1);
					r = IOCTL(fd1, AUDIO_GETBUFINFO, &ai, "");
					XP_SYS_EQ(0, r);
					if (r == 0) {
						actmode = ai.mode & (AUMODE_PLAY | AUMODE_RECORD);
						XP_EQ(exptable[i * 3 + j].mode1, actmode);
					}
				} else {
					// オープンできないことを期待するケース
					XP_SYS_NG(ENODEV, fd1);
					if (fd1 == -1) {
						XP_EQ(-exptable[i * 3 + j].mode1, errno);
					} else {
						r = IOCTL(fd1, AUDIO_GETBUFINFO, &ai, "");
						XP_EQ(0, r);
						if (r == 0) {
							actmode = ai.mode & (AUMODE_PLAY | AUMODE_RECORD);
							XP_FAIL("expects error but %d", actmode);
						}
					}
				}
				if (fd1 >= 0)
					CLOSE(fd1);
			}
			CLOSE(fd0);
		}
	}
}

// 別ユーザとのオープン
void
test_open_6()
{
	char name[32];
	char cmd[64];
	int multiuser;
	int fd0;
	int fd1;
	int r;
	uid_t ouid;

	if (geteuid() != 0) {
		TEST("open_6");
		XP_SKIP("This test must be priviledged user");
		return;
	}

	for (int i = 0; i <= 1; i++) {
		// N7 には multiuser の概念がない
		// AUDIO2 は未実装
		if (netbsd != 8) {
			if (i == 1)
				break;
			multiuser = 0;
			TEST("open_6");
		} else {
			multiuser = 1 - i;
			TEST("open_6(multiuser%d)", multiuser);

			snprintf(name, sizeof(name), "hw.%s.multiuser", hwconfigname());
			snprintf(cmd, sizeof(cmd),
				"sysctl -w %s=%d > /dev/null", name, multiuser);
			r = SYSTEM(cmd);
			if (r == -1)
				err(1, "system: %s", cmd);
			if (r != 0)
				errx(1, "system failed: %s", cmd);

			// 確認
			int newval = 0;
			size_t len = sizeof(newval);
			r = SYSCTLBYNAME(name, &newval, &len, NULL, 0, "get");
			if (r == -1)
				err(1, "multiuser");
			if (newval != multiuser)
				errx(1, "set multiuser=%d failed", multiuser);
		}

		fd0 = OPEN(devaudio, O_WRONLY);
		if (fd0 == -1)
			err(1, "open");

		ouid = GETUID();
		r = SETEUID(1);
		if (r == -1)
			err(1, "setuid");

		fd1 = OPEN(devaudio, O_WRONLY);
		if (multiuser) {
			// 別ユーザもオープン可能
			XP_SYS_OK(fd1);
		} else {
			// 別ユーザはオープンできない
			// N7 は EBUSY (Device Busy)
			// N8 は EPERM (Operation not permitted)
			// N7 はデバイス1つなので Device Busy は適切だと思う。
			if (netbsd == 7)
				XP_SYS_NG(EBUSY, fd1);
			else
				XP_SYS_NG(EPERM, fd1);
		}
		if (fd1 != -1) {
			r = CLOSE(fd1);
			XP_SYS_EQ(0, r);
		}

		r = SETEUID(ouid);
		if (r == -1)
			err(1, "setuid");

		r = CLOSE(fd0);
		XP_SYS_EQ(0, r);
	}
}

// SETINFO の受付エンコーディング
// 正しく変換できるかどうかまではここでは調べない。
void
test_encoding_1(void)
{
	int fd;
	int r;

	// リニア、正常系
	int enctable[] = {
		AUDIO_ENCODING_SLINEAR_LE,
		AUDIO_ENCODING_SLINEAR_BE,
		AUDIO_ENCODING_ULINEAR_LE,
		AUDIO_ENCODING_ULINEAR_BE,
		AUDIO_ENCODING_SLINEAR,		// for backward compatibility
		AUDIO_ENCODING_ULINEAR,		// for backward compatibility
	};
	int prectable[] = {
		8, 16, 32,
	};
	int chtable[] = {
		1, 2, 4, 8, 12,
	};
	int freqtable[] = {
		1000, 192000,
	};
	for (int i = 0; i < __arraycount(enctable); i++) {
		int enc = enctable[i];
		for (int j = 0; j < __arraycount(prectable); j++) {
			int prec = prectable[j];

			// 実際には HW 依存だが確認方法がないので
			// とりあえず手元の hdafg(4) で動く組み合わせだけ
			if (prec >= 24 && netbsd <= 7)
				continue;

			for (int k = 0; k < __arraycount(chtable); k++) {
				int ch = chtable[k];

				// 実際には HW 依存だが確認方法がないので
				if (ch > 1 && netbsd <= 7)
					continue;

				for (int m = 0; m < __arraycount(freqtable); m++) {
					int freq = freqtable[m];

					// AUDIO2/x68k ではメモリ足りなくてこける可能性
					if (netbsd >= 9 && x68k) {
						if (ch == 12)
							continue;
						if (freq == 192000)
							continue;
					}

					char buf[100];
					snprintf(buf, sizeof(buf), "enc=%d,prec=%d,ch=%d,freq=%d",
						enc, prec, ch, freq);

					TEST("encoding_1(%s)", buf);
					fd = OPEN(devaudio, O_WRONLY);
					if (fd == -1)
						err(1, "open");

					struct audio_info ai;
					AUDIO_INITINFO(&ai);
					ai.play.encoding = enc;
					ai.play.precision = prec;
					ai.play.channels = ch;
					ai.play.sample_rate = freq;
					ai.mode = AUMODE_PLAY_ALL;
					r = IOCTL(fd, AUDIO_SETINFO, &ai, "play");
					if (netbsd >= 8) {
						XP_SYS_EQ(0, r);
					} else {
						// N7 は失敗しても気にしないことにする
						if (r == 0) {
							XP_SYS_EQ(0, r);
						} else {
							XP_SKIP("XXX not checked");
						}
					}

					CLOSE(fd);
				}
			}
		}
	}
}

void
test_encoding_2()
{
	int fd;
	int r;

	// リニア、異常系

	// 本当はサポートしている以外全部なんだが
	int prectable[] = {
		0, 4, 24,
	};
	for (int i = 0; i < __arraycount(prectable); i++) {
		int prec = prectable[i];
		TEST("encoding_2(prec=%d)", prec);
		fd = OPEN(devaudio, O_WRONLY);
		if (fd == -1)
			err(1, "open");

		struct audio_info ai;
		AUDIO_INITINFO(&ai);
		ai.play.encoding = AUDIO_ENCODING_SLINEAR_LE;
		ai.play.precision = prec;
		ai.play.channels = 1;
		ai.play.sample_rate = 1000;
		ai.mode = AUMODE_PLAY_ALL;
		r = IOCTL(fd, AUDIO_SETINFO, &ai, "");
		XP_SYS_NG(EINVAL, r);

		CLOSE(fd);
	}

	int chtable[] = {
		0, 13,
	};
	for (int i = 0; i < __arraycount(chtable); i++) {
		int ch = chtable[i];
		TEST("encoding_2(ch=%d)", ch);
		fd = OPEN(devaudio, O_WRONLY);
		if (fd == -1)
			err(1, "open");

		struct audio_info ai;
		AUDIO_INITINFO(&ai);
		ai.play.encoding = AUDIO_ENCODING_SLINEAR_LE;
		ai.play.precision = 16;
		ai.play.channels = ch;
		ai.play.sample_rate = 1000;
		ai.mode = AUMODE_PLAY_ALL;
		r = IOCTL(fd, AUDIO_SETINFO, &ai, "");
		XP_SYS_NG(EINVAL, r);

		CLOSE(fd);
	}

	int freqtable[] = {
		0, 999,	192001,
	};
	for (int i = 0; i < __arraycount(freqtable); i++) {
		int freq = freqtable[i];
		TEST("encoding_3(freq=%d)", freq);

		// XXX freq=0 は NetBSD<=8 ではプロセスが無限ループに入ってしまう
		if (freq == 0 && netbsd <= 8) {
			XP_FAIL("not tested: it causes infinate loop");
			continue;
		}
		// N7 は周波数のチェックも MI レベルではしない (ただ freq 0 はアカン)
		// N8 はチェックすべきだと思うけどシラン
		if (netbsd <= 8)
			continue;

		fd = OPEN(devaudio, O_WRONLY);
		if (fd == -1)
			err(1, "open");

		struct audio_info ai;
		AUDIO_INITINFO(&ai);
		ai.play.encoding = AUDIO_ENCODING_SLINEAR_LE;
		ai.play.precision = 16;
		ai.play.channels = 2;
		ai.play.sample_rate = freq;
		ai.mode = AUMODE_PLAY_ALL;
		r = IOCTL(fd, AUDIO_SETINFO, &ai, "");
		XP_SYS_NG(EINVAL, r);

		CLOSE(fd);
	}
}

void
test_drain_1(void)
{
	int r;
	int fd;

	TEST("drain_1");

	fd = open(devaudio, O_WRONLY);
	if (fd == -1)
		err(1, "open");

	struct audio_info ai;
	AUDIO_INITINFO(&ai);
	// 1フレーム複数バイト、PLAY に設定
	ai.play.encoding = AUDIO_ENCODING_SLINEAR_LE;
	ai.play.precision = 16;
	ai.play.channels = (netbsd <= 7 && x68k) ? 1 : 2;
	ai.play.sample_rate = 11050;
	ai.mode = AUMODE_PLAY;
	r = IOCTL(fd, AUDIO_SETINFO, &ai, "");
	if (r == -1)
		err(1, "AUDIO_SETINFO");
	// 1バイト書いて close
	r = WRITE(fd, &r, 1);
	XP_SYS_EQ(1, r);
	r = CLOSE(fd);
	XP_SYS_EQ(0, r);
}

// pause したまま drain
void
test_drain_2(void)
{
	int r;
	int fd;

	TEST("drain_2");

	fd = open(devaudio, O_WRONLY);
	if (fd == -1)
		err(1, "open");

	struct audio_info ai;
	AUDIO_INITINFO(&ai);
	ai.play.encoding = AUDIO_ENCODING_SLINEAR_LE;
	ai.play.precision = 16;
	ai.play.channels = (netbsd <= 7 && x68k) ? 1 : 2;
	ai.play.sample_rate = 11050;
	ai.mode = AUMODE_PLAY;
	ai.play.pause = 1;
	r = IOCTL(fd, AUDIO_SETINFO, &ai, "");
	if (r == -1)
		err(1, "AUDIO_SETINFO");
	// 4バイト書いて close
	r = WRITE(fd, &r, 4);
	XP_SYS_EQ(4, r);
	r = CLOSE(fd);
	XP_SYS_EQ(0, r);
}

// PLAY_SYNC でブロックサイズずつ書き込む
// 期待通りの音が出るかは分からないので、play.error が0なことだけ確認
void
test_playsync_1(void)
{
	struct audio_info ai;
	char *wav;
	int wavsize;
	int fd;
	int r;
	int n;

	TEST("playsync_1");

	fd = OPEN(devaudio, O_WRONLY);
	if (fd == -1)
		err(1, "open");

	AUDIO_INITINFO(&ai);
	ai.mode = AUMODE_PLAY;
	r = IOCTL(fd, AUDIO_SETINFO, &ai, "mode");
	XP_SYS_EQ(0, r);

	r = IOCTL(fd, AUDIO_GETBUFINFO, &ai, "");
	XP_SYS_EQ(0, r);

	wavsize = ai.blocksize;
	wav = (char *)malloc(wavsize);
	if (wav == NULL)
		err(1, "malloc");
	memset(wav, 0, wavsize);

	for (int i = 0; i < 5; i++) {
		r = WRITE(fd, wav, wavsize);
		XP_SYS_EQ(wavsize, r);
	}

	// ブロックサイズで書き込めばエラーにはならないらしいが、
	// ブロックサイズ未満で書き込んでエラーになるかどうかの条件が分からないので
	// ブロックサイズ未満のテストは保留。
	r = IOCTL(fd, AUDIO_GETBUFINFO, &ai, "");
	XP_SYS_EQ(0, r);
	XP_EQ(0, ai.play.error);

	r = CLOSE(fd);
	XP_SYS_EQ(0, r);

	free(wav);
}

// HWFull/Half によらず open mode の方の操作(read/write)はできる。
void
test_readwrite_1(void)
{
	struct audio_info ai;
	char buf[10];
	int fd0, fd1;
	int r;
	int n;
	struct {
		int openmode;
		bool canwrite;
		bool canread;
	} exp7table[] = {
		{ O_RDONLY,	0, 1 },
		{ O_WRONLY,	1, 0 },
		{ O_RDWR,	1, 1 },
		{ -O_RDWR,	1, 1 },	// Half でも一応両方アクセスは出来る(ダミー)
		{ 99, },			// 仕方ないので番兵
	}, exp9table[] = {
		{ O_RDONLY,	0, 1 },
		{ O_WRONLY,	1, 0 },
		{ O_RDWR,	1, 1 },
		{ -O_RDWR,	1, 0 },	// Half なら Play と同等になる
		{ 99, },			// 仕方ないので番兵
	}, *exptable;

	if (netbsd <= 8)
		exptable = exp7table;
	else
		exptable = exp9table;

	AUDIO_INITINFO(&ai);
	ai.play.pause = 1;
	ai.record.pause = 1;

	for (int i = 0; exptable[i].openmode != 99 ; i++) {
		int openmode = exptable[i].openmode;
		bool canwrite = exptable[i].canwrite;
		bool canread = exptable[i].canread;
		if (hwfull) {
			// HWFull なら O_RDWR のほう
			if (openmode < 0)
				continue;
		} else {
			// HWHalf なら O_RDWR は負数のほう
			if (openmode == O_RDWR)
				continue;
			if (openmode == -O_RDWR) {
				openmode = O_RDWR;
			}
		}
		TEST("readwrite_1(%s)", openmodetable[openmode]);

		fd0 = OPEN(devaudio, openmode);
		if (fd0 == -1)
			err(1, "open");

		// 書き込みを伴うので音がでないよう pause しとく
		r = IOCTL(fd0, AUDIO_SETINFO, &ai, "pause");
		if (r == -1)
			err(1, "ioctl");

		// write は mode2popen[] が期待値
		memset(buf, 0, sizeof(buf));
		r = WRITE(fd0, buf, sizeof(buf));
		if (canwrite) {
			XP_SYS_EQ(10, r);
		} else {
			XP_SYS_NG(EBADF, r);
		}

		// read は mode2ropen[] が期待値
		// N7 は 1バイト以上 read しようとするとブロックする?
		r = READ(fd0, buf, 0);
		if (canread) {
			XP_SYS_EQ(0, r);
		} else {
			XP_SYS_NG(EBADF, r);
		}

		CLOSE(fd0);
	}
}

// N8 は HWFull/Half によらず1本目も2本目も互いに影響なく各自の
// open mode でオープンでき、(open mode によって許されている) read/write は
// 時系列的に衝突しなければできるようだ。pause してるからだけかもしれない。
//
// AUDIO2 では HWHalf なら full duplex 操作になる open を禁止する。
void
test_readwrite_2(void)
{
	struct audio_info ai;
	char buf[10];
	int fd0, fd1;
	int r;
	int n;
	struct {
		bool canopen;
		bool canwrite;
		bool canread;
	} expfulltable[] = {
		{ 1, 0, 1 },	// REC, REC
		{ 1, 1, 0 },	// REC, PLAY
		{ 1, 1, 1 },	// REC, BOTH
		{ 1, 0, 1 },	// PLAY, REC
		{ 1, 1, 0 },	// PLAY, PLAY
		{ 1, 1, 1 },	// PLAY, BOTH
		{ 1, 0, 1 },	// BOTH, REC
		{ 1, 1, 0 },	// BOTH, PLAY
		{ 1, 1, 1 },	// BOTH, BOTH
	},
	exphalftable[] = {
		{ 1, 0, 1 },	// REC, REC
		{ 0, 0, 0 },	// REC, PLAY
		{ 0, 0, 0 },	// REC, BOTH
		{ 0, 0, 0 },	// PLAY, REC
		{ 1, 1, 0 },	// PLAY, PLAY
		{ 1, 1, 0 },	// PLAY, BOTH
		{ 0, 0, 0 },	// BOTH, REC
		{ 1, 1, 0 },	// BOTH, PLAY
		{ 0, 0, 0 },	// BOTH, BOTH
	}, *exptable;

	// N7 は多重オープンはできない
	if (netbsd <= 7) {
		TEST("readwrite_2");
		XP_SKIP("N7 does not support multi-open");
		return;
	}
	// HW が Full/Half で期待値が違う
	if (hwfull) {
		exptable = expfulltable;
	} else {
		exptable = exphalftable;
	}

	AUDIO_INITINFO(&ai);
	ai.play.pause = 1;
	ai.record.pause = 1;

	for (int i = 0; i <= 2; i++) {
		for (int j = 0; j <= 2; j++) {
			TEST("readwrite_2(%s,%s)", openmodetable[i], openmodetable[j]);
			bool canopen  = exptable[i * 3 + j].canopen;
			bool canwrite = exptable[i * 3 + j].canwrite;
			bool canread  = exptable[i * 3 + j].canread;

			// XXX オープンできない組み合わせはオープンできない検査すべき?
			if (canopen == false) {
				XP_SKIP("XXX");
				continue;
			}

			fd0 = OPEN(devaudio, i);
			if (fd0 == -1)
				err(1, "open");

			fd1 = OPEN(devaudio, j);
			if (fd1 == -1)
				err(1, "open");

			// 書き込みを伴うので音がでないよう pause しとく
			r = IOCTL(fd1, AUDIO_SETINFO, &ai, "pause");
			if (r == -1)
				err(1, "ioctl");

			// write は mode2popen[] が期待値
			memset(buf, 0, sizeof(buf));
			r = WRITE(fd1, buf, sizeof(buf));
			if (canwrite) {
				XP_SYS_EQ(10, r);
			} else {
				XP_SYS_NG(EBADF, r);
			}

			// read は mode2ropen[] が期待値
			r = READ(fd1, buf, 0);
			if (canread) {
				XP_SYS_EQ(0, r);
			} else {
				XP_SYS_NG(EBADF, r);
			}

			CLOSE(fd0);
			CLOSE(fd1);
		}
	}
}

// 別ディスクリプタを同時に読み書き
// HW Half ではこの操作は行えない
void
test_readwrite_3()
{
	char buf[1024];
	int fd0, fd1;
	int r;
	int status;
	pid_t pid;

	TEST("readwrite_3");
	// N7 では多重オープンは出来ないので、このテストは無効
	if (netbsd <= 7) {
		XP_SKIP("N7 does not support multi-open");
		return;
	}
	if (hwfull == 0) {
		// N8 では read がブロックするバグ
		if (netbsd <= 8) {
			XP_FAIL("not tested; it will block");
			return;
		}
		// AUDIO2 では HalfHW に対して R+W の多重オープンはできない
		XP_SKIP("AUDIO2 does not support R+W open on half HW");
		return;
	}

	memset(buf, 0, sizeof(buf));

	fd0 = OPEN(devaudio, O_WRONLY);
	if (fd0 == -1)
		err(1, "open");

	fd1 = OPEN(devaudio, O_RDONLY);
	if (fd1 == -1)
		err(1, "open");

	// 事前に吐き出しておかないと fork 後に出力が重複する?
	fflush(stdout);
	fflush(stderr);
	pid = fork();
	if (pid == -1)
		err(1, "fork");
	if (pid == 0) {
		// child (read)
		for (int i = 0; i < 10; i++) {
			r = READ(fd1, buf, sizeof(buf));
			if (r == -1)
				err(1, "read(i=%d)", i);
		}
		exit(0);
	} else {
		// parent (write)
		for (int i = 0; i < 10; i++) {
			r = WRITE(fd0, buf, sizeof(buf));
			if (r == -1)
				err(1, "write(i=%d)", i);
		}
		waitpid(pid, &status, 0);
	}

	CLOSE(fd0);
	CLOSE(fd1);
	// ここまで来れば自動的に成功とする
	XP_EQ(0, 0);
}

// mmap できる mode と prot の組み合わせ
// それと mmap 出来たら read/write は出来ないのテスト
void
test_mmap_1()
{
	struct audio_info ai;
	int fd;
	int r;
	int len;
	int prot;
	void *ptr;
	struct {
		int mode;
		int prot;
		int exp;	// AUDIO2 で mmap が成功するか
	} exptable[] = {
		// 現状 VM システムの制約で mmap は再生バッファに対してのみのようだ。
		// prot はぶっちゃけ見ていないようだ。
		// N7 では open mode に関わらず再生バッファは存在するので mmap は
		// 常に成功する。
		// AUDIO2 では再生バッファがあれば成功なので O_RDONLY 以外なら成功
		{ O_RDONLY,	PROT_NONE,				0 },
		{ O_RDONLY,	PROT_READ,				0 },
		{ O_RDONLY,	PROT_WRITE,				0 },
		{ O_RDONLY,	PROT_READ | PROT_WRITE,	0 },

		{ O_WRONLY,	PROT_NONE,				1 },
		{ O_WRONLY,	PROT_READ,				1 },
		{ O_WRONLY,	PROT_WRITE,				1 },
		{ O_WRONLY,	PROT_READ | PROT_WRITE,	1 },

		// HWFull の場合
		{ O_RDWR,	PROT_NONE,				1 },
		{ O_RDWR,	PROT_READ,				1 },
		{ O_RDWR,	PROT_WRITE,				1 },
		{ O_RDWR,	PROT_READ | PROT_WRITE,	1 },

		// HWHalf の場合 (-O_RDWR を取り出した時に加工する)
		{ -O_RDWR,	PROT_NONE,				1 },
		{ -O_RDWR,	PROT_READ,				1 },
		{ -O_RDWR,	PROT_WRITE,				1 },
		{ -O_RDWR,	PROT_READ | PROT_WRITE,	1 },
	};

	if ((props & AUDIO_PROP_MMAP) == 0) {
		TEST("mmap_1");
		return;
	}

	for (int i = 0; i < __arraycount(exptable); i++) {
		int mode = exptable[i].mode;
		int prot = exptable[i].prot;
		int expected = exptable[i].exp;
		int half;

		half = 0;
		if (hwfull) {
			// HWFull なら O_RDWR のほう
			if (mode < 0)
				continue;
		} else {
			// HWHalf なら O_RDWR は負数のほう
			if (mode == O_RDWR)
				continue;
			if (mode == -O_RDWR) {
				mode = O_RDWR;
				half = 1;
			}
		}

		// N7、N8 なら mmap 自体は常に成功する
		if (netbsd <= 8)
			expected = 1;

		char protbuf[32];
		if (prot == 0) {
			strlcpy(protbuf, "PROT_NONE", sizeof(protbuf));
		} else {
			snprintb_m(protbuf, sizeof(protbuf),
				"\177\020" "b\1PROT_WRITE\0b\0PROT_READ\0", prot, 0);
		}
		TEST("mmap_1(%s,%s)", openmodetable[mode], protbuf);

		fd = OPEN(devaudio, mode);
		if (fd == -1)
			err(1, "open");

		r = IOCTL(fd, AUDIO_GETBUFINFO, &ai, "get");
		if (r == -1)
			err(1, "AUDIO_GETINFO");

		// 再生側しかサポートしていないのでこれでいい
		len = ai.play.buffer_size;

		// pause にしとく
		AUDIO_INITINFO(&ai);
		ai.play.pause = 1;
		ai.record.pause = 1;
		r = IOCTL(fd, AUDIO_SETINFO, &ai, "pause");
		if (r == -1)
			err(1, "AUDIO_SETINFO");

		ptr = MMAP(NULL, len, prot, MAP_FILE, fd, 0);
		if (expected == 0) {
			XP_SYS_NG(EACCES, ptr);
		} else {
			XP_SYS_OK(ptr);

			// read トラックは mmap フラグ立ってないので read は出来てしまう
			if (mode == O_RDONLY || (mode == O_RDWR && hwfull)) {
				r = READ(fd, &ai, 0);
				XP_SYS_EQ(0, r);
			}
			// write は出来なくなること
			if (mode == O_WRONLY || mode == O_RDWR) {
				r = WRITE(fd, &ai, 4);
				if (netbsd <= 8)
					XP_SYS_NG(EINVAL, r);
				else
					XP_SYS_NG(EPERM, r);
			}

			r = MUNMAP(ptr, len);
			XP_SYS_EQ(0, r);
		}

		// 再生側の pause が効いてること
		// 録音側はどっちらけなので調べない
		if (mode != O_RDONLY) {
			r = IOCTL(fd, AUDIO_GETBUFINFO, &ai, "");
			XP_SYS_EQ(0, r);
			XP_EQ(1, ai.play.pause);
		}

		r = CLOSE(fd);
		XP_SYS_EQ(0, r);

		// N7 では mmap したディスクリプタをクローズした直後は
		// オープンが失敗する気がする…。
		// なのでここで一回オープンしてリセット(?)しておく。
		if (netbsd <= 7) {
			fd = OPEN(devaudio, mode);
			if (fd != -1)
				CLOSE(fd);
		}
	}
}

// mmap の len と offset パラメータ
void
test_mmap_2()
{
	struct audio_info ai;
	int fd;
	int r;
	int prot;
	size_t len;
	off_t offset;
	void *ptr;
	int bufsize;
	int pagesize;

	if ((props & AUDIO_PROP_MMAP) == 0) {
		TEST("mmap_2");
		return;
	}

	// とりあえず再生側のことしか考えなくていいか。

	len = sizeof(pagesize);
	r = SYSCTLBYNAME("hw.pagesize", &pagesize, &len, NULL, 0, "get");
	if (r == -1)
		err(1, "sysctl");

	fd = OPEN(devaudio, O_WRONLY);
	if (fd == -1)
		err(1, "open");

	// buffer_size 取得
	r = IOCTL(fd, AUDIO_GETBUFINFO, &ai, "");
	if (r == -1)
		err(1, "AUDIO_GETINFO");
	bufsize = ai.play.buffer_size;

	// バッファサイズのほうが大きい場合とページサイズのほうが大きい場合が
	// あって、どっちらけ。
	int lsize = roundup2(bufsize, pagesize);

	struct {
		size_t len;
		off_t offset;
		int exp;
	} table[] = {
		// len		offset	expected
		{ 0,		0,		0 },		// len=0 だけど構わないらしい
		{ 1,		0,		0 },		// len が短くても構わないらしい
		{ lsize,	0,		0 },		// 大きい方ぴったり
		{ lsize + 1,0,		EOVERFLOW },// それを超えてはいけない

		{ 0,		-1,		EINVAL },	// 負数
		{ 0,	lsize,		0 },		// これは意味ないはずだが計算上 OK...
		{ 0,	lsize + 1,	EOVERFLOW },// 足して超えるので NG
		{ 1,	lsize,		EOVERFLOW },// 足して超えるので NG
	};

	for (int i = 0; i < __arraycount(table); i++) {
		len = table[i].len;
		offset = table[i].offset;
		int exp = table[i].exp;
		TEST("mmap_2(len=%d,offset=%d)", (int)len, (int)offset);

		ptr = MMAP(NULL, len, PROT_WRITE, MAP_FILE, fd, offset);
		if (exp == 0) {
			XP_SYS_OK(ptr);
		} else {
			// N7 時点ではまだ EOVERFLOW のチェックがなかった
			if (netbsd <= 7 && exp == EOVERFLOW)
				exp = EINVAL;
			XP_SYS_NG(exp, ptr);
		}

		if (ptr != MAP_FAILED) {
			r = MUNMAP(ptr, len);
			XP_SYS_EQ(0, r);
		}
	}

	r = CLOSE(fd);
	XP_SYS_EQ(0, r);

	// N7 では mmap したディスクリプタをクローズした直後は
	// オープンが失敗する。
	// なのでここで一回オープンしてリセット(?)しておく。
	if (netbsd <= 7) {
		fd = OPEN(devaudio, O_WRONLY);
		if (fd != -1)
			CLOSE(fd);
	}
}

// mmap するとすぐに動き始める
void
test_mmap_3()
{
	struct audio_info ai;
	int fd;
	int r;
	int len;
	int prot;
	void *ptr;

	// とりあえず再生側のことしか考えなくていいか。
	TEST("mmap_3");
	if ((props & AUDIO_PROP_MMAP) == 0) {
		return;
	}

	fd = OPEN(devaudio, O_WRONLY);
	if (fd == -1)
		err(1, "open");

	r = IOCTL(fd, AUDIO_GETBUFINFO, &ai, "get");
	if (r == -1)
		err(1, "AUDIO_GETINFO");
	len = ai.play.buffer_size;

	ptr = MMAP(NULL, len, prot, MAP_FILE, fd, 0);
	XP_SYS_OK(ptr);

	r = IOCTL(fd, AUDIO_GETBUFINFO, &ai, "get");
	XP_SYS_EQ(0, r);
	XP_EQ(1, ai.play.active);

	r = CLOSE(fd);
	XP_SYS_EQ(0, r);

	// N7 では mmap したディスクリプタをクローズした直後は
	// オープンが失敗する。
	// なのでここで一回オープンしてリセット(?)しておく。
	if (netbsd <= 7) {
		fd = OPEN(devaudio, O_WRONLY);
		if (fd != -1)
			CLOSE(fd);
	}
}

// 同一ディスクリプタで二重 mmap
void
test_mmap_4()
{
	struct audio_info ai;
	int fd;
	int r;
	int len;
	int prot;
	void *ptr;
	void *ptr2;

	TEST("mmap_4");
	if ((props & AUDIO_PROP_MMAP) == 0) {
		return;
	}

	fd = OPEN(devaudio, O_WRONLY);
	if (fd == -1)
		err(1, "open");

	r = IOCTL(fd, AUDIO_GETBUFINFO, &ai, "get");
	if (r == -1)
		err(1, "AUDIO_GETINFO");
	len = ai.play.buffer_size;

	ptr = MMAP(NULL, len, prot, MAP_FILE, fd, 0);
	XP_SYS_OK(ptr);

	// N7 では成功するようだが意図してるのかどうか分からん。
	// N8 も成功するようだが意図してるのかどうか分からん。
	ptr2 = MMAP(NULL, len, prot, MAP_FILE, fd, 0);
	XP_SYS_OK(ptr2);
	if (ptr2 != MAP_FAILED) {
		r = MUNMAP(ptr2, len);
		XP_SYS_EQ(0, r);
	}

	r = MUNMAP(ptr, len);
	XP_SYS_EQ(0, r);

	r = CLOSE(fd);
	XP_SYS_EQ(0, r);

	// N7 では mmap したディスクリプタをクローズした直後は
	// オープンが失敗する。
	// なのでここで一回オープンしてリセット(?)しておく。
	if (netbsd <= 7) {
		fd = OPEN(devaudio, O_WRONLY);
		if (fd != -1)
			CLOSE(fd);
	}
}

// 別ディスクリプタで mmap
void
test_mmap_5()
{
	struct audio_info ai;
	int fd0;
	int fd1;
	int r;
	int len;
	int prot;
	void *ptr0;
	void *ptr1;

	TEST("mmap_5");
	// 多重オープンはできない
	if (netbsd <= 7)
		return;
	if ((props & AUDIO_PROP_MMAP) == 0) {
		return;
	}

	fd0 = OPEN(devaudio, O_WRONLY);
	if (fd0 == -1)
		err(1, "open");

	r = IOCTL(fd0, AUDIO_GETBUFINFO, &ai, "get");
	if (r == -1)
		err(1, "AUDIO_GETINFO");
	len = ai.play.buffer_size;

	fd1 = OPEN(devaudio, O_WRONLY);
	if (fd1 == -1)
		err(1, "open");

	ptr0 = MMAP(NULL, len, prot, MAP_FILE, fd0, 0);
	XP_SYS_OK(ptr0);

	ptr1 = MMAP(NULL, len, prot, MAP_FILE, fd1, 0);
	XP_SYS_OK(ptr1);

	r = MUNMAP(ptr1, len);
	XP_SYS_EQ(0, r);

	r = CLOSE(fd1);
	XP_SYS_EQ(0, r);

	r = MUNMAP(ptr0, len);
	XP_SYS_EQ(0, r);

	r = CLOSE(fd0);
	XP_SYS_EQ(0, r);
}

// mmap 前に GET[IO]OFFS すると、既に次のポジションがセットされているようだ。
// N7 だと RDONLY でも WRONLY でも同じ値が読めるようだ。
// GETIOFFS は初期オフセット 0 だがどうせこっちは動いてないはず。
void
test_mmap_6()
{
	struct audio_info ai;
	struct audio_offset ao;
	int fd;
	int r;

	for (int mode = 0; mode <= 2; mode++) {
		TEST("mmap_6(%s)", openmodetable[mode]);

		fd = OPEN(devaudio, mode);
		if (fd == -1)
			err(1, "open");

		r = IOCTL(fd, AUDIO_GETBUFINFO, &ai, "");
		if (r == -1)
			err(1, "AUDIO_GETINFO");

		r = IOCTL(fd, AUDIO_GETOOFFS, &ao, "");
		XP_SYS_EQ(0, r);
		XP_EQ(0, ao.samples);			// 転送バイト数 0
		XP_EQ(0, ao.deltablks);			// 前回チェック時の転送ブロック数 0
		if (netbsd == 9 && mode == O_RDONLY) {
			// N7, N8 は常にトラックとかがあるのでいつでも ao.offset が
			// 次のブロックを指せるが、AUDIO2 では O_RDONLY だと再生トラックは
			// 存在しないので、再生ブロックサイズもない。
			// これはやむを得ないか。
			XP_EQ(0, ao.offset);
		} else {
			XP_EQ(ai.blocksize, ao.offset);	// これから転送する位置は次ブロック
		}

		r = IOCTL(fd, AUDIO_GETIOFFS, &ao, "");
		XP_SYS_EQ(0, r);
		XP_EQ(0, ao.samples);			// 転送バイト数 0
		XP_EQ(0, ao.deltablks);			// 前回チェック時の転送ブロック数 0
		XP_EQ(0, ao.offset);			// 0

		r = CLOSE(fd);
		XP_SYS_EQ(0, r);
	}
}

// mmap_7, 8 の共通部
void
test_mmap_7_8_common(int type)
{
	struct audio_info ai;
	struct audio_offset ao;
	char *buf;
	int fd;
	int r;
	int blocksize;

	// N7、N8 はなぜかこの PR に書いてあるとおりにならない
	if (netbsd <= 8) {
		XP_SKIP("This test does not work on N7/N8");
		return;
	}
	// A2 の type0 は今のところ仕様
	if (netbsd == 9 && type == 0) {
		XP_SKIP("On AUDIO2 it can not set blocksize");
		return;
	}

	fd = OPEN(devaudio, O_WRONLY);
	if (fd == -1)
		err(1, "open");

	AUDIO_INITINFO(&ai);
	ai.play.pause = 1;
	if (type == 0)
		ai.blocksize = 1024;
	r = IOCTL(fd, AUDIO_SETINFO, &ai, "");
	if (r == -1)
		err(1, "AUDIO_SETINFO");

	r = IOCTL(fd, AUDIO_GETBUFINFO, &ai, "");
	if (r == -1)
		err(1, "AUDIO_GETINFO");
	if (type == 0) {
		// 1024バイトが設定できること
		XP_EQ(1024, ai.blocksize);
		blocksize = 1024;
	} else {
		// カーネルのブロックサイズを取得
		blocksize = ai.blocksize;
	}

	buf = (char *)malloc(blocksize);
	if (buf == NULL)
		err(1, "malloc");
	memset(buf, 0, blocksize);

	// オープン直後の GETOOFFS
	r = IOCTL(fd, AUDIO_GETOOFFS, &ao, "");
	XP_SYS_EQ(0, r);
	XP_EQ(0, ao.samples);			// まだ何も書いてない
	XP_EQ(0, ao.deltablks);
	XP_EQ(blocksize, ao.offset);	// 次ブロックを指している

	// 1ブロック書き込み
	r = WRITE(fd, buf, blocksize);
	XP_SYS_EQ(blocksize, r);

	// 1ブロック書き込み後の GETOOFFS
	r = IOCTL(fd, AUDIO_GETOOFFS, &ao, "");
	XP_SYS_EQ(0, r);
	XP_EQ(blocksize, ao.samples);	// 1ブロック
	XP_EQ(1, ao.deltablks);				// 前回チェック時の転送ブロック数
	XP_EQ(blocksize * 2, ao.offset);	// 次ブロック

	// pause 解除
	AUDIO_INITINFO(&ai);
	ai.play.pause = 0;
	r = IOCTL(fd, AUDIO_SETINFO, &ai, "");
	XP_SYS_EQ(0, r);

	// pause 解除後の GETOOFFS
	r = IOCTL(fd, AUDIO_GETOOFFS, &ao, "");
	XP_SYS_EQ(0, r);
	XP_EQ(blocksize, ao.samples);
	XP_EQ(0, ao.deltablks);
	XP_EQ(blocksize * 2, ao.offset);

	// もう1ブロック書き込み
	r = WRITE(fd, buf, blocksize);
	XP_SYS_EQ(blocksize, r);
	r = IOCTL(fd, AUDIO_DRAIN, NULL, "");
	XP_SYS_EQ(0, r);

	// 書き込み完了後の GETOOFFS
	r = IOCTL(fd, AUDIO_GETOOFFS, &ao, "");
	XP_SYS_EQ(0, r);
	XP_EQ(blocksize * 2, ao.samples);
	XP_EQ(1, ao.deltablks);
	XP_EQ(blocksize * 3, ao.offset);

	r = CLOSE(fd);
	XP_SYS_EQ(0, r);

	free(buf);
}

// 1ブロック write した後に GETOOFFS する。
// 1ブロックは 1024 バイト。
// PR kern/50613
void
test_mmap_7()
{
	TEST("mmap_7");
	test_mmap_7_8_common(0);
}

// 1ブロック write した後に GETOOFFS する。
// 1ブロックはネイティブブロックサイズ。
void
test_mmap_8()
{
	TEST("mmap_8");
	test_mmap_7_8_common(1);
}

// mmap 開始
void
test_mmap_9()
{
	struct audio_info ai;
	struct audio_offset ao;
	char *ptr;
	int fd;
	int r;

	TEST("mmap_9");
	if (x68k && netbsd <= 7) {
		// HW エンコードにセットするあたりのテストが面倒
		return;
	}

	fd = OPEN(devaudio, O_WRONLY);
	if (fd == -1)
		err(1, "open");

	AUDIO_INITINFO(&ai);
	ai.play.encoding = AUDIO_ENCODING_SLINEAR_LE;
	ai.play.precision = 16;
	ai.play.channels = 2;
	ai.play.sample_rate = 48000;
	r = IOCTL(fd, AUDIO_SETINFO, &ai, "");
	if (r == -1)
		err(1, "AUDIO_SETINFO");

	r = IOCTL(fd, AUDIO_GETBUFINFO, &ai, "");
	if (r == -1)
		err(1, "AUDIO_GETINFO");

	ptr = (char *)MMAP(NULL, ai.play.buffer_size, 0, 0, fd, 0);
	XP_SYS_OK(ptr);

	// 雑なテスト
	// mmap 直後は理想的には samples = 0、offset = blksize になる。

	r = IOCTL(fd, AUDIO_GETOOFFS, &ao, "");
	XP_SYS_EQ(0, r);
	XP_EQ(0, ao.samples);			// まだ書き込みはない
	XP_EQ(0, ao.deltablks);			// 前回チェック時の転送ブロック数
	XP_EQ(ai.blocksize, ao.offset);	//

	usleep(50 * 1000);
	// 50msec 後には理想的には samples は 1ブロック分、offset ももう
	// 1ブロック分進んでいるはず。

	r = IOCTL(fd, AUDIO_GETOOFFS, &ao, "");
	XP_SYS_EQ(0, r);
	XP_EQ(ai.blocksize, ao.samples);// 1ブロック書き込み済み
	XP_EQ(1, ao.deltablks);			// 前回チェック時の転送ブロック数
	XP_EQ(ai.blocksize * 2, ao.offset);	//

	r = CLOSE(fd);
	XP_SYS_EQ(0, r);

	// N7 では mmap したディスクリプタをクローズした直後は
	// オープンが失敗する。
	// なのでここで一回オープンしてリセット(?)しておく。
	if (netbsd <= 7) {
		fd = OPEN(devaudio, O_WRONLY);
		if (fd != -1)
			CLOSE(fd);
	}
}

// FIOASYNC が同時に2人設定できるか
void
test_FIOASYNC_1(void)
{
	int r;
	int fd0, fd1;
	int val;

	TEST("FIOASYNC_1");
	if (netbsd < 8) {
		XP_SKIP("NetBSD7 does not support multi-open");
		return;
	}

	// 1人目が ASYNC on
	fd0 = OPEN(devaudio, O_WRONLY);
	if (fd0 == -1)
		err(1, "open");
	val = 1;
	r = IOCTL(fd0, FIOASYNC, &val, "on");
	XP_SYS_EQ(0, r);

	// 続いて2人目が ASYNC on
	fd1 = OPEN(devaudio, O_WRONLY);
	if (fd0 == -1)
		err(1, "open");
	val = 1;
	r = IOCTL(fd1, FIOASYNC, &val, "on");
	XP_SYS_EQ(0, r);

	CLOSE(fd0);
	CLOSE(fd1);
}

// FIOASYNC が別トラックに影響を与えないこと
void
test_FIOASYNC_2(void)
{
	int r;
	int fd0, fd1;
	int val;

	TEST("FIOASYNC_2");
	if (netbsd < 8) {
		XP_SKIP("NetBSD7 does not support multi-open");
		return;
	}

	// 1人目が ASYNC on
	fd0 = OPEN(devaudio, O_WRONLY);
	if (fd0 == -1)
		err(1, "open");
	val = 1;
	r = IOCTL(fd0, FIOASYNC, &val, "on");
	XP_EQ(0, r);

	// 続いて2人目が ASYNC off
	fd1 = OPEN(devaudio, O_WRONLY);
	if (fd0 == -1)
		err(1, "open");
	val = 0;
	r = IOCTL(fd1, FIOASYNC, &val, "off");
	XP_EQ(0, r);

	CLOSE(fd0);
	CLOSE(fd1);
}

// FIOASYNC リセットのタイミング
void
test_FIOASYNC_3(void)
{
	int r;
	int fd0, fd1;
	int val;

	TEST("FIOASYNC_3");
	if (netbsd < 8) {
		XP_SKIP("NetBSD7 does not support multi-open");
		return;
	}

	// 1人目がオープン
	fd0 = OPEN(devaudio, O_WRONLY);
	if (fd0 == -1)
		err(1, "open");

	// 2人目が ASYNC on してクローズ。2人目の ASYNC 状態は無効
	fd1 = OPEN(devaudio, O_WRONLY);
	if (fd0 == -1)
		err(1, "open");
	val = 1;
	r = IOCTL(fd1, FIOASYNC, &val, "on");
	XP_SYS_EQ(0, r);
	CLOSE(fd1);

	// もう一回2人目がオープンして ASYNC on
	fd1 = OPEN(devaudio, O_WRONLY);
	if (fd0 == -1)
		err(1, "open");
	val = 1;
	r = IOCTL(fd1, FIOASYNC, &val, "on");
	XP_SYS_EQ(0, r);
	CLOSE(fd1);
	CLOSE(fd0);
}

volatile int sigio_caught;
void
signal_FIOASYNC_4(int signo)
{
	if (signo == SIGIO)
		sigio_caught = 1;
}

// 書き込みで SIGIO が飛んでくるか
// いまいち N7 でシグナルが飛んでくる条件が分からん。
// PLAY_ALL でブロックサイズ書き込んだら飛んでくるようだ。
// PLAY/PLAY_ALL で 4バイト書き込むとかではだめだった。
void
test_FIOASYNC_4(void)
{
	struct audio_info ai;
	int r;
	int fd;
	int val;
	char *data;

	TEST("FIOASYNC_4");
	signal(SIGIO, signal_FIOASYNC_4);
	sigio_caught = 0;

	fd = OPEN(devaudio, O_WRONLY);
	if (fd == -1)
		err(1, "open");

	r = IOCTL(fd, AUDIO_GETBUFINFO, &ai, "");
	if (r == -1)
		err(1, "ioctl");
	if (ai.blocksize == 0)
		errx(1, "blocksize == 0");
	data = (char *)malloc(ai.blocksize);
	if (data == NULL)
		err(1, "malloc");
	memset(data, 0, ai.blocksize);

	val = 1;
	r = IOCTL(fd, FIOASYNC, &val, "on");
	XP_SYS_EQ(0, r);

	r = WRITE(fd, data, ai.blocksize);
	XP_SYS_EQ(ai.blocksize, r);

	for (int i = 0; i < 10 && sigio_caught == 0; i++) {
		usleep(10000);
	}
	XP_EQ(1, sigio_caught);

	CLOSE(fd);
	free(data);

	signal(SIGIO, SIG_IGN);
	sigio_caught = 0;
}

// 録音で SIGIO が飛んでくるか
void
test_FIOASYNC_5(void)
{
	int r;
	int fd;
	int val;

	TEST("FIOASYNC_5");
	signal(SIGIO, signal_FIOASYNC_4);
	sigio_caught = 0;

	fd = OPEN(devaudio, O_RDONLY);
	if (fd == -1)
		err(1, "open");

	val = 1;
	r = IOCTL(fd, FIOASYNC, &val, "on");
	XP_SYS_EQ(0, r);

	r = READ(fd, &r, 4);
	XP_SYS_EQ(4, r);

	for (int i = 0; i < 10 && sigio_caught == 0; i++) {
		usleep(10000);
	}
	XP_EQ(1, sigio_caught);

	CLOSE(fd);

	signal(SIGIO, SIG_IGN);
	sigio_caught = 0;
}

// AUDIO_WSEEK の動作確認
void
test_AUDIO_WSEEK_1(void)
{
	int r;
	int fd;
	int n;

	TEST("AUDIO_WSEEK_1");

	fd = OPEN(devaudio, O_WRONLY);
	if (fd == -1)
		err(1, "open: %s", devaudio);

	struct audio_info ai;
	AUDIO_INITINFO(&ai);
	ai.play.encoding = AUDIO_ENCODING_SLINEAR_LE;
	ai.play.precision = 8;
	ai.play.channels = (netbsd <= 7 && x68k) ? 1 : 2;
	ai.play.sample_rate = 11050;
	ai.play.pause = 1;
	ai.mode = AUMODE_PLAY_ALL;
	r = IOCTL(fd, AUDIO_SETINFO, &ai, "pause=1");
	if (r == -1)
		err(1, "AUDIO_SETINFO.pause");

	// 初期状態だと 0 バイトになる
	n = 0;
	r = IOCTL(fd, AUDIO_WSEEK, &n, "");
	XP_SYS_EQ(0, r);
	XP_EQ(0, n);

	// 4バイト書き込むと 4になる
	// N7 では 0 になる。要調査。
	r = WRITE(fd, &r, 4);
	if (r == -1)
		err(1, "write(4)");
	r = IOCTL(fd, AUDIO_WSEEK, &n, "");
	XP_SYS_EQ(0, r);
	XP_EQ(4, n);

	CLOSE(fd);
}

// SETFD は N7 ならオープンモードに関わらず audio layer の状態を変えるの意
// (で同時に MD の状態も必要なら変える)。GETFD は audio layer の duplex 状態
// を取得するだけ。
// N8 はソースコード上踏襲しているので見た目の動作は同じだが、検討した上での
// それかどうかが謎。
// AUDIO2 では GETFD は実質 HW の duplex を取得するのと等価。SETFD は
// 今とは違う方には変更できない。
void
test_AUDIO_SETFD_ONLY(void)
{
	struct audio_info ai;
	int r;
	int fd;
	int n;

	for (int mode = 0; mode <= 1; mode++) {
		TEST("AUDIO_SETFD_ONLY(%s)", openmodetable[mode]);

		fd = OPEN(devaudio, mode);
		if (fd == -1)
			err(1, "open: %s", devaudio);

		// オープン直後は常に Half
		n = 0;
		r = IOCTL(fd, AUDIO_GETFD, &n, "");
		XP_SYS_EQ(0, r);
		XP_EQ(0, n);

		// Full duplex に設定しようとすると、
		// N7: HW Full なら設定できる、HW Half ならエラー。
		// AUDIO2: 変更は出来ない
		n = 1;
		r = IOCTL(fd, AUDIO_SETFD, &n, "on");
		if (netbsd <= 7) {
			if (hwfull) {
				XP_SYS_EQ(0, r);
			} else {
				XP_SYS_NG(ENOTTY, r);
			}
		} else {
			XP_SYS_NG(ENOTTY, r);
		}

		// 取得してみると、
		// N7: HW Full なら 1、HW Half なら 0 のまま。
		// AUDIO2: 変わっていないこと。
		n = 0;
		r = IOCTL(fd, AUDIO_GETFD, &n, "");
		XP_SYS_EQ(0, r);
		if (netbsd <= 7) {
			XP_EQ(hwfull, n);
		} else {
			XP_EQ(0, n);
		}

		// GETINFO の ai.*.open などトラック状態は変化しない。
		r = IOCTL(fd, AUDIO_GETBUFINFO, &ai, "");
		XP_SYS_EQ(0, r);
		XP_EQ(mode2popen_full[mode], ai.play.open);
		XP_EQ(mode2ropen_full[mode], ai.record.open);

		// Half duplex に設定しようとすると、
		// N7: HW Full なら設定できる、HW Half なら何も起きず成功。
		// AUDIO2: 実質変わらないので成功する。
		n = 0;
		r = IOCTL(fd, AUDIO_SETFD, &n, "off");
		XP_SYS_EQ(0, r);

		// 取得してみると、
		// N7: HW Full なら 0、HW Half なら 0 のまま。
		// AUDIO2: 変わっていないこと。
		n = 0;
		r = IOCTL(fd, AUDIO_GETFD, &n, "");
		XP_SYS_EQ(0, r);
		XP_EQ(0, n);

		// GETINFO の ai.*.open などトラック状態は変化しない。
		r = IOCTL(fd, AUDIO_GETBUFINFO, &ai, "");
		XP_SYS_EQ(0, r);
		XP_EQ(mode2popen_full[mode], ai.play.open);
		XP_EQ(mode2ropen_full[mode], ai.record.open);

		CLOSE(fd);
	}
}

void
test_AUDIO_SETFD_RDWR(void)
{
	struct audio_info ai;
	int r;
	int fd;
	int n;

	TEST("AUDIO_SETFD_RDWR");

	fd = OPEN(devaudio, O_RDWR);
	if (fd == -1)
		err(1, "open: %s", devaudio);

	// O_RDWR オープン直後は必ず Half と manpage に書いてあるが
	// N7: HW Full なら Full になる。manpage のバグということにしておく。
	// AUDIO2: HW Full なら Full、HW Half なら Half になる。
	n = 0;
	r = IOCTL(fd, AUDIO_GETFD, &n, "");
	XP_SYS_EQ(0, r);
	XP_EQ(hwfull, n);

	// Full duplex に設定しようとすると
	// HW Full なら設定できる(何も起きない)、HW Half ならエラー
	n = 1;
	r = IOCTL(fd, AUDIO_SETFD, &n, "on");
	if (hwfull) {
		XP_SYS_EQ(0, r);
	} else {
		XP_SYS_NG(ENOTTY, r);
	}

	// 取得してみると、
	// HW Full なら 1、HW Half なら 0 のまま。
	n = 0;
	r = IOCTL(fd, AUDIO_GETFD, &n, "");
	XP_SYS_EQ(0, r);
	XP_EQ(hwfull, n);

	// GETINFO の ai.*.open などトラック状態は変化しない。
	r = IOCTL(fd, AUDIO_GETBUFINFO, &ai, "");
	XP_SYS_EQ(0, r);
	XP_EQ(1, ai.play.open);
	XP_EQ(1, ai.record.open);

	// Half duplex に設定しようとすると、
	// N7: HW Full なら設定できる、HW Half なら何も起きず成功。
	// AUDIO2: HW Full ならエラー、HW Half なら何も起きず成功。
	n = 0;
	r = IOCTL(fd, AUDIO_SETFD, &n, "off");
	if (netbsd <= 7) {
		XP_SYS_EQ(0, r);
	} else {
		if (hwfull) {
			XP_SYS_NG(ENOTTY, r);
		} else {
			XP_SYS_EQ(0, r);
		}
	}

	// 取得してみると、
	// N7: HW Full なら 0、HW Half なら 0 のまま。
	// AUDIO2: HW Full なら 1、HW Half なら 0 のまま。
	n = 0;
	r = IOCTL(fd, AUDIO_GETFD, &n, "");
	XP_SYS_EQ(0, r);
	if (netbsd <= 7) {
		XP_EQ(0, n);
	} else {
		XP_EQ(hwfull, n);
	}

	// GETINFO の ai.*.open などトラック状態は変化しない。
	r = IOCTL(fd, AUDIO_GETBUFINFO, &ai, "");
	XP_SYS_EQ(0, r);
	XP_EQ(1, ai.play.open);
	XP_EQ(1, ai.record.open);

	CLOSE(fd);
}

// ai.play.seek、ai.play.samples が取得できるか確認
void
test_AUDIO_GETINFO_seek()
{
	struct audio_info ai;
	int r;
	int fd;
	char *buf;
	int bufsize;

	TEST("AUDIO_GETINFO_seek");

	fd = OPEN(devaudio, O_WRONLY);
	if (fd == -1)
		err(1, "open");

	// 1サンプル != 1バイトにしたい
	AUDIO_INITINFO(&ai);
	ai.play.encoding = AUDIO_ENCODING_SLINEAR_LE;
	ai.play.precision = 16;
	ai.play.channels = x68k ? 1 : 2;
	ai.play.sample_rate = 16000;
	r = IOCTL(fd, AUDIO_SETINFO, &ai, "");
	XP_SYS_EQ(0, r);

	// 最初は seek も samples もゼロ
	r = IOCTL(fd, AUDIO_GETBUFINFO, &ai, "");
	XP_SYS_EQ(0, r);
	XP_EQ(0, ai.play.seek);
	XP_EQ(0, ai.play.samples);
	// ついでにここでバッファサイズを取得
	bufsize = ai.play.buffer_size;

	buf = (char *)malloc(bufsize);
	if (buf == NULL)
		err(1, "malloc");
	memset(buf, 0, bufsize);

	// 残り全部を書き込む (リングバッファのポインタが先頭に戻るはず)
	r = WRITE(fd, buf, bufsize);
	XP_SYS_EQ(bufsize, r);
	r = IOCTL(fd, AUDIO_DRAIN, NULL, "");
	XP_SYS_EQ(0, r);

	// seek は折り返して 0 に戻ること?
	// samples は計上を続けることだけど、
	// N7 はカウントがぴったり合わないようだ
	r = IOCTL(fd, AUDIO_GETBUFINFO, &ai, "");
	XP_SYS_EQ(0, r);
	XP_EQ(0, ai.play.seek);
	if (netbsd <= 8) {
		if (bufsize != ai.play.samples && ai.play.samples > bufsize * 9 / 10) {
			XP_SKIP("ai.play.samples expects %d but %d"
			        " (unknown few drops?)",
				bufsize, ai.play.samples);
		} else {
			XP_EQ(bufsize, ai.play.samples);
		}
	} else {
		XP_EQ(bufsize, ai.play.samples);
	}

	r = CLOSE(fd);
	XP_SYS_EQ(0, r);

	free(buf);
}

void
test_AUDIO_GETINFO_eof(void)
{
	struct audio_info ai;
	int r;
	int fd, fd1;
	int n;

	TEST("AUDIO_GETINFO_eof");

	fd = OPEN(devaudio, O_RDWR);
	if (fd == -1)
		err(1, "open");

	// 書き込みを伴うので pause にしとく
	AUDIO_INITINFO(&ai);
	ai.play.pause = 1;
	r = IOCTL(fd, AUDIO_SETINFO, &ai, "pause");
	if (r == -1)
		err(1, "ioctl");

	// 最初は 0
	r = IOCTL(fd, AUDIO_GETBUFINFO, &ai, "");
	XP_SYS_EQ(0, r);
	XP_EQ(0, ai.play.eof);
	XP_EQ(0, ai.record.eof);

	// 0バイト書き込むと上がる
	r = WRITE(fd, &r, 0);
	if (r == -1)
		err(1, "write");
	r = IOCTL(fd, AUDIO_GETBUFINFO, &ai, "");
	XP_SYS_EQ(0, r);
	XP_EQ(1, ai.play.eof);
	XP_EQ(0, ai.record.eof);

	// 1バイト以上を書き込んでも上がらない
	r = WRITE(fd, &r, 4);
	if (r == -1)
		err(1, "write");
	memset(&ai, 0, sizeof(ai));
	r = IOCTL(fd, AUDIO_GETBUFINFO, &ai, "");
	XP_SYS_EQ(0, r);
	XP_EQ(1, ai.play.eof);
	XP_EQ(0, ai.record.eof);

	// もう一度0バイト書き込むと上がる
	r = WRITE(fd, &r, 0);
	if (r == -1)
		err(1, "write");
	memset(&ai, 0, sizeof(ai));
	r = IOCTL(fd, AUDIO_GETBUFINFO, &ai, "");
	XP_SYS_EQ(0, r);
	XP_EQ(2, ai.play.eof);
	XP_EQ(0, ai.record.eof);

	// 0バイト読んでも上がらない
	if (hwfull) {
		r = READ(fd, &r, 0);
		if (r == -1)
			err(1, "read");
		memset(&ai, 0, sizeof(ai));
		r = IOCTL(fd, AUDIO_GETBUFINFO, &ai, "");
		XP_SYS_EQ(0, r);
		XP_EQ(2, ai.play.eof);
		XP_EQ(0, ai.record.eof);
	}

	// 別ディスクリプタと干渉しないこと
	if (netbsd >= 8) {
		fd1 = OPEN(devaudio, O_RDWR);
		if (fd1 == -1)
			err(1, "open");
		memset(&ai, 0, sizeof(ai));
		r = IOCTL(fd1, AUDIO_GETBUFINFO, &ai, "");
		XP_SYS_EQ(0, r);
		XP_EQ(0, ai.play.eof);
		XP_EQ(0, ai.record.eof);
		CLOSE(fd1);
	}

	CLOSE(fd);

	// オープンしなおすとリセット
	fd = OPEN(devaudio, O_RDWR);
	if (fd == -1)
		err(1, "open");

	r = IOCTL(fd, AUDIO_GETBUFINFO, &ai, "");
	XP_SYS_EQ(0, r);
	XP_EQ(0, ai.play.eof);
	XP_EQ(0, ai.record.eof);

	CLOSE(fd);
}

// オープン直後の mode と、
// SETINFO で mode が切り替わるケース
void
test_AUDIO_SETINFO_mode()
{
	struct audio_info ai;
	char buf[10];
	int r;
	int fd;
	int n;
	int mode;
	struct {
		int openmode;	// オープン時のモード (O_*)
		int inimode;	// オープン直後の aumode 期待値
		int setmode;	// aumode 設定値
		int expmode7;	// 設定変更後のaumode期待値(N7)
		int expmode;	// 設定変更後のaumode期待値
#define P	AUMODE_PLAY
#define A	AUMODE_PLAY_ALL
#define R	AUMODE_RECORD
	} exptable[] = {
		// open		inimode	setmode		N7expmode	expmode
		{ O_RDONLY,	R,		  0x0,		  0x0,		R },
		{ O_RDONLY,	R,		    P,		    P,		R },
		{ O_RDONLY,	R,		  A  ,		  A|P,		R },
		{ O_RDONLY,	R,		  A|P,		  A|P,		R },
		{ O_RDONLY,	R,		R    ,		R    ,		R },
		{ O_RDONLY,	R,		R|  P,		    P,		R },
		{ O_RDONLY,	R,		R|A  ,		  A|P,		R },
		{ O_RDONLY,	R,		R|A|P,		  A|P,		R },
		{ O_RDONLY,	R,		  0x8,		  0x8,		R },
		{ O_RDONLY,	R,		  0x9,		  0x9,		R },

		{ O_WRONLY,	  A|P,	  0x0,		  0x0,		    P },
		{ O_WRONLY,	  A|P,	    P,		    P,		    P },
		{ O_WRONLY,	  A|P,	  A  ,		  A|P,		  A|P },
		{ O_WRONLY,	  A|P,	  A|P,		  A|P,		  A|P },
		{ O_WRONLY,	  A|P,	R    ,		R    ,		    P },
		{ O_WRONLY,	  A|P,	R|  P,		    P,		    P },
		{ O_WRONLY,	  A|P,	R|A  ,		  A|P,		  A|P },
		{ O_WRONLY,	  A|P,	R|A|P,		  A|P,		  A|P },
		{ O_WRONLY,	  A|P,	  0x8,		  0x8,		    P },
		{ O_WRONLY,	  A|P,	  0x9,		  0x9,		    P },

		// HWFull の場合
		{ O_RDWR,	R|A|P,	  0x0,		  0x0,		R|  P },
		{ O_RDWR,	R|A|P,	    P,		    P,		R|  P },
		{ O_RDWR,	R|A|P,	  A  ,		  A|P,		R|A|P },
		{ O_RDWR,	R|A|P,	  A|P,		  A|P,		R|A|P },
		{ O_RDWR,	R|A|P,	R    ,		R    ,		R|  P },
		{ O_RDWR,	R|A|P,	R|  P,		R|  P,		R|  P },
		{ O_RDWR,	R|A|P,	R|A  ,		R|A|P,		R|A|P },
		{ O_RDWR,	R|A|P,	R|A|P,		R|A|P,		R|A|P },
		{ O_RDWR,	R|A|P,	  0x8,		  0x8,		R|  P },
		{ O_RDWR,	R|A|P,	  0x9,		  0x9,		R|  P },

		// HWHalf の場合 (-O_RDWR を取り出した時に加工する)
		{ -O_RDWR,	  A|P,	  0x0,		  0x0,		    P },
		{ -O_RDWR,	  A|P,	    P,		    P,		    P },
		{ -O_RDWR,	  A|P,	  A  ,		  A|P,		  A|P },
		{ -O_RDWR,	  A|P,	  A|P,		  A|P,		  A|P },
		{ -O_RDWR,	  A|P,	R    ,		R    ,		    P },
		{ -O_RDWR,	  A|P,	R|  P,		    P,		    P },
		{ -O_RDWR,	  A|P,	R|A  ,		  A|P,		  A|P },
		{ -O_RDWR,	  A|P,	R|A|P,		  A|P,		  A|P },
		{ -O_RDWR,	  A|P,	  0x8,		  0x8,		    P },
		{ -O_RDWR,	  A|P,	  0x9,		  0x9,		    P },
	};
#undef P
#undef A
#undef R

	for (int i = 0; i < __arraycount(exptable); i++) {
		int openmode = exptable[i].openmode;
		int inimode = exptable[i].inimode;
		int setmode = exptable[i].setmode;
		int expmode = (netbsd <= 8)
			? exptable[i].expmode7
			: exptable[i].expmode;
		int half;

		half = 0;
		if (hwfull) {
			// HWFull なら O_RDWR のほう
			if (openmode < 0)
				continue;
		} else {
			// HWHalf なら O_RDWR は負数のほう
			if (openmode == O_RDWR)
				continue;
			if (openmode == -O_RDWR) {
				openmode = O_RDWR;
				half = 1;
			}
		}

		char setmodestr[32];
		snprintb_m(setmodestr, sizeof(setmodestr),
			"\177\020b\1REC\0b\2ALL\0b\0PLAY\0", setmode, 0);

		TEST("AUDIO_SETINFO_mode(%s%s,%s)",
			half ? "H:" : "",
			openmodetable[openmode], setmodestr);

		fd = OPEN(devaudio, openmode);
		if (fd == -1)
			err(1, "open");

		// オープンした直後の状態
		memset(&ai, 0, sizeof(ai));
		r = IOCTL(fd, AUDIO_GETINFO, &ai, "");
		if (r == -1)
			err(1, "ioctl");
		XP_EQ(inimode, ai.mode);
		XP_EQ(mode2popen_full[openmode], ai.play.open);
		XP_EQ(mode2ropen_full[openmode], ai.record.open);
		// N7、N8 では buffer_size は常に非ゼロなので調べない
		// A2: バッファは O_RDWR なら HWHalf でも両方確保される。
		// Half なのを判定するほうが後なのでやむをえないか。
		// 確保されてたらいけないわけでもないだろうし(無駄ではあるけど)。
		if (netbsd >= 9) {
			XP_BUFFSIZE(mode2popen_full[openmode], ai.play.buffer_size);
			XP_BUFFSIZE(mode2ropen_full[openmode], ai.record.buffer_size);
		}

		// mode を変える
		// ついでに pause にしとく
		ai.mode = setmode;
		ai.play.pause = 1;
		ai.record.pause = 1;
		r = IOCTL(fd, AUDIO_SETINFO, &ai, "mode");
		XP_SYS_EQ(0, r);
		if (r == 0) {
			r = IOCTL(fd, AUDIO_GETINFO, &ai, "");
			XP_SYS_EQ(0, r);
			XP_EQ(expmode, ai.mode);
			// mode に関係なく当初のオープンモードを維持するようだ
			XP_EQ(mode2popen_full[openmode], ai.play.open);
			XP_EQ(mode2ropen_full[openmode], ai.record.open);
			// N7、N8 では buffer_size は常に非ゼロなので調べない
			if (netbsd >= 9) {
				XP_BUFFSIZE(mode2popen_full[openmode], ai.play.buffer_size);
				XP_BUFFSIZE(mode2ropen_full[openmode], ai.record.buffer_size);
			}
		}

		// 書き込みが出来るかどうかは
		// N7: オープン時の openmode による
		// A2: オープン時の inimode による、としたい
		// オープン後に変えた mode は適用されない。
		bool canwrite = (netbsd <= 8)
			? (openmode != O_RDONLY)
			: ((inimode & AUMODE_PLAY) != 0);
		r = WRITE(fd, buf, 0);
		if (canwrite) {
			XP_SYS_EQ(0, r);
		} else {
			XP_SYS_NG(EBADF, r);
		}

		// 読み込みが出来るかどうかは
		// N7: オープン時の openmode による
		// A2: オープン時の inimode による、としたい
		// オープン後に変えた mode は適用されない。
		bool canread = (netbsd <= 8)
			? (openmode != O_WRONLY)
			: ((inimode & AUMODE_RECORD) != 0);
		r = READ(fd, buf, 0);
		if (canread) {
			XP_SYS_EQ(0, r);
		} else {
			XP_SYS_NG(EBADF, r);
		}

		CLOSE(fd);
	}
}

// {play,rec}.params の設定がきくか。
void
test_AUDIO_SETINFO_params()
{
	struct audio_info ai;
	int r;
	int fd;

	for (int openmode = 0; openmode <= 2; openmode++) {
		for (int aimode = 0; aimode <= 1; aimode++) {
			for (int pause = 0; pause <= 1; pause++) {
				// aimode は ai.mode を変更するかどうか
				// pause は ai.*.pause を変更するかどうか

				// rec のみだと ai.mode は変更できない
				if (openmode == O_RDONLY && aimode == 1)
					continue;

				// half だと O_RDWR は O_WRONLY と同じ
				if (hwfull == 0 && openmode == O_RDWR)
					continue;

				TEST("AUDIO_SETINFO_params(%s,mode%d,pause%d)",
					openmodetable[openmode], aimode, pause);

				fd = OPEN(devaudio, openmode);
				if (fd == -1)
					err(1, "open");

				AUDIO_INITINFO(&ai);
				// params が全部独立して効くかどうかは大変なのでとりあえず
				// sample_rate で代表させる。
				ai.play.sample_rate = 11025;
				ai.record.sample_rate = 11025;
				if (aimode)
					ai.mode = mode2aumode_full[openmode] & ~AUMODE_PLAY_ALL;
				if (pause) {
					ai.play.pause = 1;
					ai.record.pause = 1;
				}

				r = IOCTL(fd, AUDIO_SETINFO, &ai, "");
				XP_SYS_EQ(0, r);

				r = IOCTL(fd, AUDIO_GETBUFINFO, &ai, "");
				XP_SYS_EQ(0, r);
				int expmode = (aimode)
					? mode2aumode_full[openmode] & ~AUMODE_PLAY_ALL
					: mode2aumode_full[openmode];
				XP_EQ(expmode, ai.mode);
				if (openmode == O_RDONLY) {
					// play なし
					if (netbsd <= 8)
						XP_EQ(pause, ai.play.pause);
					else
						XP_EQ(0, ai.play.pause);
				} else {
					// play あり
					XP_EQ(11025, ai.play.sample_rate);
					XP_EQ(pause, ai.play.pause);
				}
				if (openmode == O_WRONLY) {
					// rec なし
					if (netbsd <= 8)
						XP_EQ(pause, ai.record.pause);
					else
						XP_EQ(0, ai.record.pause);
				} else {
					// rec あり
					XP_EQ(11025, ai.record.sample_rate);
					XP_EQ(pause, ai.record.pause);
				}

				r = CLOSE(fd);
				XP_SYS_EQ(0, r);
			}
		}
	}
}

// 別のディスクリプタでの SETINFO に干渉されないこと
void
test_AUDIO_SETINFO_params2()
{
	struct audio_info ai;
	int r;
	int fd0;
	int fd1;

	TEST("AUDIO_SETINFO_params2");
	// N7 は多重オープンはできない
	if (netbsd <= 7) {
		XP_SKIP("NetBSD7 does not support multi-open");
		return;
	}

	fd0 = OPEN(devaudio, O_WRONLY);
	if (fd0 == -1)
		err(1, "open");

	// 1本目のパラメータを変える
	AUDIO_INITINFO(&ai);
	ai.play.sample_rate = 11025;
	r = IOCTL(fd0, AUDIO_SETINFO, &ai, "");
	XP_SYS_EQ(0, r);

	fd1 = OPEN(devaudio, O_WRONLY);
	if (fd1 == -1)
		err(1, "open");

	// 2本目で同じパラメータを変える
	AUDIO_INITINFO(&ai);
	ai.play.sample_rate = 16000;
	r = IOCTL(fd1, AUDIO_SETINFO, &ai, "");
	XP_SYS_EQ(0, r);

	// 1本目のパラメータ変更の続き
	AUDIO_INITINFO(&ai);
	ai.play.encoding = AUDIO_ENCODING_SLINEAR_LE;
	r = IOCTL(fd0, AUDIO_SETINFO, &ai, "");
	XP_SYS_EQ(0, r);

	// sample_rate は 11k のままであること
	r = IOCTL(fd0, AUDIO_GETBUFINFO, &ai, "");
	XP_SYS_EQ(0, r);
	XP_EQ(AUDIO_ENCODING_SLINEAR_LE, ai.play.encoding);
	XP_EQ(11025, ai.play.sample_rate);

	r = CLOSE(fd0);
	XP_SYS_EQ(0, r);
	r = CLOSE(fd1);
	XP_SYS_EQ(0, r);
}

void
test_AUDIO_SETINFO_params3()
{
	struct audio_info ai;
	int fd0;
	int fd1;
	int r;

	TEST("AUDIO_SETINFO_params3");
	if (netbsd <= 7) {
		XP_SKIP("NetBSD7 does not support multi-open");
		return;
	}

	// 1本目 /dev/audio を再生側だけ開く
	fd0 = OPEN(devaudio, O_WRONLY);
	if (fd0 == -1)
		err(1, "open");

	// 2本目 /dev/audio を両方開く
	fd1 = OPEN(devaudio, O_RDWR);
	if (fd1 == -1)
		err(1, "open");

	// 2本目で両トラックを SETINFO
	AUDIO_INITINFO(&ai);
	ai.play.sample_rate = 11025;
	ai.record.sample_rate = 11025;
	r = IOCTL(fd1, AUDIO_SETINFO, &ai, "");
	XP_SYS_EQ(0, r);

	// 1本目で GETINFO しても両トラックとも影響を受けていないこと
	memset(&ai, 0, sizeof(ai));
	r = IOCTL(fd0, AUDIO_GETINFO, &ai, "");
	XP_SYS_EQ(0, r);
	XP_EQ(8000, ai.play.sample_rate);
	XP_EQ(8000, ai.record.sample_rate);

	r = CLOSE(fd0);
	XP_SYS_EQ(0, r);
	r = CLOSE(fd1);
	XP_SYS_EQ(0, r);
}

// pause の設定がきくか。
// play のみ、rec のみ、play/rec 両方について
// ai.mode 変更ありなし、ai.play.* 変更ありなしについて調べる
void
test_AUDIO_SETINFO_pause()
{
	struct audio_info ai;
	int r;
	int fd;

	for (int openmode = 0; openmode <= 2; openmode++) {
		for (int aimode = 0; aimode <= 1; aimode++) {
			for (int param = 0; param <= 1; param++) {
				// aimode は ai.mode を変更するかどうか
				// param は ai.*.param を変更するかどうか

				// rec のみだと ai.mode は変更できない
				if (openmode == O_RDONLY && aimode == 1)
					continue;

				// half だと O_RDWR は O_WRONLY と同じ
				if (hwfull == 0 && openmode == O_RDWR)
					continue;

				TEST("AUDIO_SETINFO_pause(%s,mode%d,param%d)",
					openmodetable[openmode], aimode, param);

				fd = OPEN(devaudio, openmode);
				if (fd == -1)
					err(1, "open");

				// pause をあげる
				AUDIO_INITINFO(&ai);
				ai.play.pause = 1;
				ai.record.pause = 1;
				if (aimode)
					ai.mode = mode2aumode_full[openmode] & ~AUMODE_PLAY_ALL;
				if (param) {
					ai.play.sample_rate = 11025;
					ai.record.sample_rate = 11025;
				}

				r = IOCTL(fd, AUDIO_SETINFO, &ai, "");
				XP_SYS_EQ(0, r);

				r = IOCTL(fd, AUDIO_GETBUFINFO, &ai, "");
				XP_SYS_EQ(0, r);
				int expmode = (aimode)
					? mode2aumode_full[openmode] & ~AUMODE_PLAY_ALL
					: mode2aumode_full[openmode];
				XP_EQ(expmode, ai.mode);
				if (openmode == O_RDONLY) {
					// play がない
					if (netbsd <= 8)
						XP_EQ(1, ai.play.pause);
					else
						XP_EQ(0, ai.play.pause);
				} else {
					// play がある
					XP_EQ(1, ai.play.pause);
					XP_EQ(param ? 11025 : 8000, ai.play.sample_rate);
				}
				if (openmode == O_WRONLY) {
					// rec がない
					if (netbsd <= 8)
						XP_EQ(1, ai.record.pause);
					else
						XP_EQ(0, ai.record.pause);
				} else {
					XP_EQ(1, ai.record.pause);
					XP_EQ(param ? 11025 : 8000, ai.record.sample_rate);
				}

				// pause を下げるテストもやる?
				AUDIO_INITINFO(&ai);
				ai.play.pause = 0;
				ai.record.pause = 0;
				if (aimode)
					ai.mode = mode2aumode_full[openmode];
				if (param) {
					ai.play.sample_rate = 16000;
					ai.record.sample_rate = 16000;
				}

				r = IOCTL(fd, AUDIO_SETINFO, &ai, "");
				XP_SYS_EQ(0, r);

				r = IOCTL(fd, AUDIO_GETBUFINFO, &ai, "");
				XP_SYS_EQ(0, r);
				XP_EQ(mode2aumode_full[openmode], ai.mode);
				XP_EQ(0, ai.play.pause);
				XP_EQ(0, ai.record.pause);
				if (openmode != O_RDONLY)
					XP_EQ(param ? 16000 : 8000, ai.play.sample_rate);
				if (openmode != O_WRONLY)
					XP_EQ(param ? 16000 : 8000, ai.record.sample_rate);

				r = CLOSE(fd);
				XP_SYS_EQ(0, r);
			}
		}
	}
}

// audio デバイスオープン中に audioctl がオープンできること
void
test_audioctl_open_1()
{
	int fd;
	int ctl;
	int r;
	int fmode;
	int cmode;

	for (fmode = 0; fmode <= 2; fmode++) {
		if (fmode == O_WRONLY && (props & AUDIO_PROP_PLAYBACK) == 0)
			continue;
		if (fmode == O_RDONLY && (props & AUDIO_PROP_CAPTURE) == 0)
			continue;

		for (cmode = 0; cmode <= 2; cmode++) {
			TEST("audioctl_open_1(%s,%s)",
				openmodetable[fmode], openmodetable[cmode]);

			fd = OPEN(devaudio, fmode);
			if (fd == -1)
				err(1, "open");

			ctl = OPEN(devaudioctl, cmode);
			XP_SYS_OK(ctl);

			r = CLOSE(ctl);
			XP_SYS_EQ(0, r);

			r = CLOSE(fd);
			XP_SYS_EQ(0, r);
		}
	}
}

// audioctl デバイスオープン中に audio がオープンできること
void
test_audioctl_open_2()
{
	int fd;
	int ctl;
	int r;
	int fmode;
	int cmode;

	for (fmode = 0; fmode <= 2; fmode++) {
		if (fmode == O_WRONLY && (props & AUDIO_PROP_PLAYBACK) == 0)
			continue;
		if (fmode == O_RDONLY && (props & AUDIO_PROP_CAPTURE) == 0)
			continue;

		for (cmode = 0; cmode <= 2; cmode++) {
			TEST("audioctl_open_2(%s,%s)",
				openmodetable[fmode], openmodetable[cmode]);

			ctl = OPEN(devaudioctl, cmode);
			XP_SYS_OK(ctl);

			fd = OPEN(devaudio, fmode);
			XP_SYS_OK(fd);

			r = CLOSE(fd);
			XP_SYS_EQ(0, r);

			r = CLOSE(ctl);
			XP_SYS_EQ(0, r);
		}
	}
}

// audioctl の多重オープン
void
test_audioctl_open_3()
{
	int ctl0;
	int ctl1;
	int r;
	uid_t ouid;

	TEST("audioctl_open_3");

	ctl0 = OPEN(devaudioctl, O_RDWR);
	if (ctl0 == -1)
		err(1, "open");

	ctl1 = OPEN(devaudioctl, O_RDWR);
	XP_SYS_OK(ctl1);

	r = CLOSE(ctl0);
	XP_SYS_EQ(0, r);

	r = CLOSE(ctl1);
	XP_SYS_EQ(0, r);
}

// audio とは別ユーザでも audioctl はオープンできること
// パーミッションはもう _1 と _2 でやったのでいいか
void
test_audioctl_open_4()
{
	char name[32];
	char cmd[64];
	int fd;
	int fc;
	int r;
	int multiuser;
	uid_t ouid;

	if (geteuid() != 0) {
		TEST("audioctl_open_4");
		XP_SKIP("This test must be priviledged user");
		return;
	}

	// /dev/audio を root がオープンした後で
	// /dev/audioctl を一般ユーザがオープンする
	for (int i = 0; i <= 1; i++) {
		// N7 には multiuser の概念がない
		// AUDIO2 は未実装
		if (netbsd != 8) {
			if (i == 1)
				break;
			TEST("audioctl_open_4");
		} else {
			multiuser = 1 - i;
			TEST("audioctl_open_4(multiuser%d)", multiuser);

			snprintf(name, sizeof(name), "hw.%s.multiuser", hwconfigname());
			snprintf(cmd, sizeof(cmd),
				"sysctl -w %s=%d > /dev/null", name, multiuser);
			r = SYSTEM(cmd);
			if (r == -1)
				err(1, "system: %s", cmd);
			if (r != 0)
				errx(1, "system failed: %s", cmd);

			// 確認
			int newval = 0;
			size_t len = sizeof(newval);
			r = SYSCTLBYNAME(name, &newval, &len, NULL, 0, "get");
			if (r == -1)
				err(1, "multiuser");
			if (newval != multiuser)
				errx(1, "set multiuser=%d failed", multiuser);
		}

		fd = OPEN(devaudio, O_RDWR);
		if (fd == -1)
			err(1, "open");

		ouid = GETUID();
		r = SETEUID(1);
		if (r == -1)
			err(1, "setuid");

		fc = OPEN(devaudioctl, O_RDWR);
		XP_SYS_OK(fc);
		if (fc != -1) {
			r = CLOSE(fc);
			XP_SYS_EQ(0, r);
		}

		r = SETEUID(ouid);
		if (r == -1)
			err(1, "setuid");

		r = CLOSE(fd);
		XP_SYS_EQ(0, r);
	}
}

// audioctl とは別ユーザでも audio はオープンできること
void
test_audioctl_open_5()
{
	char name[32];
	char cmd[64];
	int fd;
	int fc;
	int r;
	int multiuser;
	uid_t ouid;

	if (geteuid() != 0) {
		TEST("audioctl_open_5");
		XP_SKIP("This test must be priviledged user");
		return;
	}

	// /dev/audioctl を root がオープンした後で
	// /dev/audio を一般ユーザがオープンする
	for (int i = 0; i <= 1; i++) {
		// N7 には multiuser の概念がない
		// AUDIO2 は未実装
		if (netbsd != 8) {
			if (i == 1)
				break;
			TEST("audioctl_open_5");
		} else {
			multiuser = 1 - i;
			TEST("audioctl_open_5(multiuser%d)", multiuser);

			snprintf(name, sizeof(name), "hw.%s.multiuser", hwconfigname());
			snprintf(cmd, sizeof(cmd),
				"sysctl -w %s=%d > /dev/null", name, multiuser);
			r = SYSTEM(cmd);
			if (r == -1)
				err(1, "system: %s", cmd);
			if (r != 0)
				errx(1, "system failed: %s", cmd);

			// 確認
			int newval = 0;
			size_t len = sizeof(newval);
			r = SYSCTLBYNAME(name, &newval, &len, NULL, 0, "get");
			if (r == -1)
				err(1, "multiuser");
			if (newval != multiuser)
				errx(1, "set multiuser=%d failed", multiuser);
		}

		fc = OPEN(devaudioctl, O_RDWR);
		if (fc == -1)
			err(1, "open");

		ouid = GETUID();
		r = SETEUID(1);
		if (r == -1)
			err(1, "setuid");

		fd = OPEN(devaudio, O_RDWR);
		XP_SYS_OK(fd);
		if (fd != -1) {
			r = CLOSE(fd);
			XP_SYS_EQ(0, r);
		}

		r = SETEUID(ouid);
		if (r == -1)
			err(1, "setuid");

		r = CLOSE(fc);
		XP_SYS_EQ(0, r);
	}
}

// テスト一覧
#define DEF(x)	{ #x, test_ ## x }
struct testtable testtable[] = {
	DEF(open_1),
	DEF(open_2),
	DEF(open_3),
	DEF(open_4),
	DEF(open_5),
	DEF(open_6),
	DEF(encoding_1),
	DEF(encoding_2),
	DEF(drain_1),
	DEF(drain_2),
	DEF(playsync_1),
	DEF(readwrite_1),
	DEF(readwrite_2),
	DEF(readwrite_3),
	DEF(mmap_1),
	DEF(mmap_2),
	DEF(mmap_3),
	DEF(mmap_4),
	DEF(mmap_5),
	DEF(mmap_6),
	DEF(mmap_7),
	DEF(mmap_8),
	DEF(mmap_9),
	DEF(FIOASYNC_1),
	DEF(FIOASYNC_2),
	DEF(FIOASYNC_3),
	DEF(FIOASYNC_4),
	DEF(FIOASYNC_5),
	DEF(AUDIO_WSEEK_1),
	DEF(AUDIO_SETFD_ONLY),
	DEF(AUDIO_SETFD_RDWR),
	DEF(AUDIO_GETINFO_seek),
	DEF(AUDIO_GETINFO_eof),
	DEF(AUDIO_SETINFO_mode),
	DEF(AUDIO_SETINFO_params),
	DEF(AUDIO_SETINFO_params2),
	DEF(AUDIO_SETINFO_params3),
	DEF(AUDIO_SETINFO_pause),
	DEF(audioctl_open_1),
	DEF(audioctl_open_2),
	DEF(audioctl_open_3),
	DEF(audioctl_open_4),
	DEF(audioctl_open_5),
	{ NULL, NULL },
};

