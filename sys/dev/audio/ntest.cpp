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
int x68k;
char testname[100];
int testcount;
int failcount;
char devaudio[16];
char devsound[16];
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

	r = ioctl(fd, AUDIO_GETDEV, &dev);
	if (r == -1)
		err(1, "init:AUDIO_GETDEV");
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
	failcount++;
}
#define XP_SKIP()	xp_skip(__LINE__)
void xp_skip(int line)
{
	/* nothing to do */
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

// ---

// O_* を AUMODE_* に変換
int mode2aumode[] = {
	AUMODE_RECORD,
	AUMODE_PLAY | AUMODE_PLAY_ALL,
	AUMODE_PLAY | AUMODE_PLAY_ALL | AUMODE_RECORD,
};
// O_* を PLAY 側がオープンされてるかに変換
int mode2popen[] = {
	0, 1, 1,
};
// O_* を RECORD 側がオープンされてるかに変換
int mode2ropen[] = {
	1, 0, 1,
};

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
		XP_EQ(mode2popen[mode], ai.play.open);
		XP_EQ(mode2ropen[mode], ai.record.open);
		// ai.mode は open_5 で調べている

		if (netbsd <= 8) {
			// N7、N8 では使わないほうのトラックのバッファも常にある
			XP_NE(0, ai.play.buffer_size);
			XP_NE(0, ai.record.buffer_size);
		} else {
			// AUDIO2 では使わないほうのバッファは確保してない
			XP_BUFFSIZE(mode2popen[mode], ai.play.buffer_size);
			XP_BUFFSIZE(mode2ropen[mode], ai.record.buffer_size);
		}

		r = CLOSE(fd);
		XP_SYS_EQ(0, r);
	}
}

// /dev/audio は何回開いても初期値は同じ
void
test_open_2(void)
{
	struct audio_info ai, ai0;
	int channels;
	int fd;
	int r;

	TEST("open_2");

	// オープンして初期値をチェック
	fd = OPEN(devaudio, O_WRONLY);
	if (fd == -1)
		err(1, "open");
	memset(&ai, 0, sizeof(ai));
	r = IOCTL(fd, AUDIO_GETBUFINFO, &ai, "");
	XP_SYS_EQ(0, r);

	XP_NE(0, ai.blocksize);
	XP_NE(0, ai.hiwat);
	XP_NE(0, ai.lowat);
	XP_EQ(AUMODE_PLAY | AUMODE_PLAY_ALL, ai.mode);
	// play
	XP_EQ(8000, ai.play.sample_rate);
	XP_EQ(1, ai.play.channels);
	XP_EQ(8, ai.play.precision);
	XP_EQ(AUDIO_ENCODING_ULAW, ai.play.encoding);
	// gain
	// port
	XP_EQ(0, ai.play.seek);
	// avail_ports
	XP_BUFFSIZE(1, ai.play.buffer_size);
	XP_EQ(0, ai.play.samples);
	XP_EQ(0, ai.play.eof);
	XP_EQ(0, ai.play.pause);
	XP_EQ(0, ai.play.error);
	XP_EQ(0, ai.play.waiting);
	// balance
	XP_EQ(1, ai.play.open);
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
	XP_BUFFSIZE((netbsd <= 8), ai.record.buffer_size);
	XP_EQ(0, ai.record.samples);
	XP_EQ(0, ai.record.eof);
	XP_EQ(0, ai.record.pause);
	XP_EQ(0, ai.record.error);
	XP_EQ(0, ai.record.waiting);
	// balance
	XP_EQ(0, ai.record.open);
	XP_EQ(0, ai.record.active);
	// これを保存しておく
	ai0 = ai;

	// できるだけ変更
	channels = (netbsd <= 7 && x68k) ? 1 : 2;
	AUDIO_INITINFO(&ai);
	if (ai0.hiwat > 0)
		ai.hiwat = ai0.hiwat - 1;
	if (ai0.lowat < ai0.hiwat)
		ai.lowat = ai0.lowat + 1;
	ai.mode = AUMODE_PLAY;
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
	CLOSE(fd);

	// 再オープンしてチェック
	fd = OPEN(devaudio, O_WRONLY);
	if (fd == -1)
		err(1, "open");
	memset(&ai, 0, sizeof(ai));
	r = IOCTL(fd, AUDIO_GETBUFINFO, &ai, "");
	XP_SYS_EQ(0, r);

	XP_NE(0, ai.blocksize);
	XP_NE(0, ai.hiwat);
	XP_NE(0, ai.lowat);
	XP_EQ(AUMODE_PLAY | AUMODE_PLAY_ALL, ai.mode);
	// play
	XP_EQ(8000, ai.play.sample_rate);
	XP_EQ(1, ai.play.channels);
	XP_EQ(8, ai.play.precision);
	XP_EQ(AUDIO_ENCODING_ULAW, ai.play.encoding);
	// gain
	// port
	XP_EQ(0, ai.play.seek);
	// avail_ports
	XP_NE(0, ai.play.buffer_size);
	XP_EQ(0, ai.play.samples);
	XP_EQ(0, ai.play.eof);
	XP_EQ(0, ai.play.pause);
	XP_EQ(0, ai.play.error);
	XP_EQ(0, ai.play.waiting);
	// balance
	XP_EQ(1, ai.play.open);
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
	XP_BUFFSIZE((netbsd <= 8), ai.record.buffer_size);
	XP_EQ(0, ai.record.samples);
	XP_EQ(0, ai.record.eof);
	XP_EQ(0, ai.record.pause);
	XP_EQ(0, ai.record.error);
	XP_EQ(0, ai.record.waiting);
	// balance
	XP_EQ(0, ai.record.open);
	XP_EQ(0, ai.record.active);

	CLOSE(fd);
}

// /dev/sound は前回の値がみえる
void
test_open_3(void)
{
	struct audio_info ai, ai0;
	int channels;
	int fd;
	int r;

	TEST("open_3");

	// まず /dev/audio 開いて初期化させておく
	fd = OPEN(devaudio, O_RDONLY);
	if (fd == -1)
		err(1, "open");
	CLOSE(fd);

	// オープンして初期値をチェック
	fd = OPEN(devsound, O_WRONLY);
	if (fd == -1)
		err(1, "open");
	memset(&ai, 0, sizeof(ai));
	r = IOCTL(fd, AUDIO_GETBUFINFO, &ai, "");
	XP_SYS_EQ(0, r);

	// audio の初期値と同じものが見えるはず
	XP_NE(0, ai.blocksize);
	// hiwat, lowat
	XP_EQ(AUMODE_PLAY | AUMODE_PLAY_ALL, ai.mode);
	// play
	XP_EQ(8000, ai.play.sample_rate);
	XP_EQ(1, ai.play.channels);
	XP_EQ(8, ai.play.precision);
	XP_EQ(AUDIO_ENCODING_ULAW, ai.play.encoding);
	// gain
	// port
	XP_EQ(0, ai.play.seek);
	// avail_ports
	XP_NE(0, ai.play.buffer_size);
	XP_EQ(0, ai.play.samples);
	XP_EQ(0, ai.play.eof);
	XP_EQ(0, ai.play.pause);
	XP_EQ(0, ai.play.error);
	XP_EQ(0, ai.play.waiting);
	// balance
	XP_EQ(1, ai.play.open);
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
	XP_BUFFSIZE((netbsd <= 8), ai.record.buffer_size);
	XP_EQ(0, ai.record.samples);
	XP_EQ(0, ai.record.eof);
	XP_EQ(0, ai.record.pause);
	XP_EQ(0, ai.record.error);
	XP_EQ(0, ai.record.waiting);
	// balance
	XP_EQ(0, ai.record.open);
	XP_EQ(0, ai.record.active);

	// できるだけ変更
	channels = (netbsd <= 7 && x68k) ? 1 : 2;
	AUDIO_INITINFO(&ai);
	ai.mode = AUMODE_PLAY_ALL;
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
	CLOSE(fd);

	// 再オープンしてチェック
	fd = OPEN(devsound, O_WRONLY);
	if (fd == -1)
		err(1, "open");
	memset(&ai, 0, sizeof(ai));
	r = IOCTL(fd, AUDIO_GETBUFINFO, &ai, "");
	XP_SYS_EQ(0, r);

	XP_NE(0, ai.blocksize);
	// hiwat, lowat は変化する
	XP_EQ(AUMODE_PLAY | AUMODE_PLAY_ALL, ai.mode);
	// play
	XP_EQ(ai0.play.sample_rate, ai.play.sample_rate);
	XP_EQ(ai0.play.channels, ai.play.channels);
	XP_EQ(ai0.play.precision, ai.play.precision);
	XP_EQ(ai0.play.encoding, ai.play.encoding);
	// gain
	// port
	XP_EQ(0, ai.play.seek);
	// avail_ports
	XP_NE(0, ai.play.buffer_size);
	XP_EQ(0, ai.play.samples);
	XP_EQ(0, ai.play.eof);
	XP_EQ(ai0.play.pause, ai.play.pause);
	XP_EQ(0, ai.play.error);
	XP_EQ(0, ai.play.waiting);
	// balance
	XP_EQ(1, ai.play.open);
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
	XP_BUFFSIZE((netbsd <= 8), ai.record.buffer_size);
	XP_EQ(0, ai.record.samples);
	XP_EQ(0, ai.record.eof);
	XP_EQ(ai0.record.pause, ai.record.pause);
	XP_EQ(0, ai.record.error);
	XP_EQ(0, ai.record.waiting);
	// balance
	XP_EQ(0, ai.record.open);
	XP_EQ(0, ai.record.active);

	CLOSE(fd);
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
							XP_SKIP();
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
		bool canwrite;
		bool canread;
	} expfulltable[] = {
		{ 0, 1 },	// REC
		{ 1, 0 },	// PLAY
		{ 1, 1 },	// BOTH
	}, exphalftable[] = {
		{ 0, 1 },	// REC
		{ 1, 0 },	// PLAY
		{ 1, 0 },	// BOTH
	}, *exptable;

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
		TEST("readwrite_1(%s)", openmodetable[i]);
		bool canwrite = exptable[i].canwrite;
		bool canread = exptable[i].canread;

		fd0 = OPEN(devaudio, i);
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
		XP_SKIP();
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

			if (canopen == false) {
				XP_SKIP();
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
		XP_SKIP();
		return;
	}
	if (hwfull == 0) {
		// N8 では read がブロックするバグ
		if (netbsd <= 8) {
			XP_FAIL("not tested; it will block");
			return;
		}
		// AUDIO2 では HalfHW に対して R+W の多重オープンはできない
		XP_SKIP();
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

// FIOASYNC が同時に2人設定できるか
void
test_FIOASYNC_1(void)
{
	int r;
	int fd0, fd1;
	int val;

	TEST("FIOASYNC_1");
	if (netbsd < 8) {
		XP_SKIP();
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
		XP_SKIP();
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
		XP_SKIP();
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
		XP_EQ(mode2popen[mode], ai.play.open);
		XP_EQ(mode2ropen[mode], ai.record.open);

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
		XP_EQ(mode2popen[mode], ai.play.open);
		XP_EQ(mode2ropen[mode], ai.record.open);

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
		XP_EQ(mode2popen[openmode], ai.play.open);
		XP_EQ(mode2ropen[openmode], ai.record.open);
		// N7、N8 では buffer_size は常に非ゼロなので調べない
		// A2: バッファは O_RDWR なら HWHalf でも両方確保される。
		// Half なのを判定するほうが後なのでやむをえないか。
		// 確保されてたらいけないわけでもないだろうし(無駄ではあるけど)。
		if (netbsd >= 9) {
			XP_BUFFSIZE(mode2popen[openmode], ai.play.buffer_size);
			XP_BUFFSIZE(mode2ropen[openmode], ai.record.buffer_size);
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
			XP_EQ(mode2popen[openmode], ai.play.open);
			XP_EQ(mode2ropen[openmode], ai.record.open);
			// N7、N8 では buffer_size は常に非ゼロなので調べない
			if (netbsd >= 9) {
				XP_BUFFSIZE(mode2popen[openmode], ai.play.buffer_size);
				XP_BUFFSIZE(mode2ropen[openmode], ai.record.buffer_size);
			}
		}

		// 書き込みが出来るかどうかはオープン時の inimode によるようだ。
		// オープン後に変えた mode は適用されない。
		r = WRITE(fd, buf, 0);
		if ((inimode & AUMODE_PLAY) != 0) {
			XP_SYS_EQ(0, r);
		} else {
			XP_SYS_NG(EBADF, r);
		}

		// 読み込みが出来るかどうかはオープン時の inimode によるようだ。
		// オープン後に変えた mode は適用されない。
		r = READ(fd, buf, 0);
		if ((inimode & AUMODE_RECORD) != 0) {
			XP_SYS_EQ(0, r);
		} else {
			XP_SYS_NG(EBADF, r);
		}

		CLOSE(fd);
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
	DEF(encoding_1),
	DEF(encoding_2),
	DEF(drain_1),
	DEF(drain_2),
	DEF(playsync_1),
	DEF(readwrite_1),
	DEF(readwrite_2),
	DEF(readwrite_3),
	DEF(FIOASYNC_1),
	DEF(FIOASYNC_2),
	DEF(FIOASYNC_3),
	DEF(FIOASYNC_4),
	DEF(FIOASYNC_5),
	DEF(AUDIO_WSEEK_1),
	DEF(AUDIO_SETFD_ONLY),
	DEF(AUDIO_SETFD_RDWR),
	DEF(AUDIO_GETINFO_eof),
	DEF(AUDIO_SETINFO_mode),
	{ NULL, NULL },
};

