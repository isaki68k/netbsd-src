/*
 * 実環境での自動テストではしにくいテスト用
 */

#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/audioio.h>
#include <sys/cdefs.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/param.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include "../../pad/padio.h"	// PAD_GET_AUDIOUNIT

struct cmdtable {
	const char *name;
	int (*func)(int, char *[]);
};

void init(int);

int debug;
int x68k;
char devaudio[16];
char devsound[16];
char devmixer[16];
extern struct cmdtable cmdtable[];

void __attribute__((__noreturn__))
usage()
{
	// cmd は一度に1つ。任意の引数あり。
	printf("usage: %s <cmd> [<arg...>]\n", getprogname());
	printf("  -d: debug\n");
	printf("  -u <unit>: audio/sound device unit number (defualt:0)\n");
	for (int i = 0; cmdtable[i].name != NULL; i++) {
		printf("\t%s\n", cmdtable[i].name);
	}
	exit(1);
}

int
main(int ac, char *av[])
{
	int c;
	int unit;

	x68k = 0;
	unit = 0;

	// global option
	while ((c = getopt(ac, av, "du:")) != -1) {
		switch (c) {
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

	if (ac == 0)
		usage();

	// 先頭が cmd なら一つだけ受け取って処理
	for (int j = 0; cmdtable[j].name != NULL; j++) {
		if (strcmp(av[0], cmdtable[j].name) == 0) {
			cmdtable[j].func(ac, av);
			return 0;
		}
	}

	usage();
	return 0;
}

void
init(int unit)
{
	audio_device_t dev;
	int fd;
	int r;

	snprintf(devaudio, sizeof(devaudio), "/dev/audio%d", unit);
	snprintf(devsound, sizeof(devsound), "/dev/sound%d", unit);
	snprintf(devmixer, sizeof(devmixer), "/dev/mixer%d", unit);
	if (debug)
		printf("unit = %d\n", unit);

	fd = open(devaudio, O_WRONLY);
	if (fd == -1)
		err(1, "init: open: %s", devaudio);
	r = ioctl(fd, AUDIO_GETDEV, &dev);
	if (r == -1)
		err(1, "init:AUDIO_GETDEV");

	// ショートカット
	if (strcmp(dev.config, "vs") == 0)
		x68k = 1;
	close(fd);
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

#define POLL(pfd, nfd, timeout)	debug_poll(__LINE__, pfd, nfd, timeout)
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
	int r = poll(pfd, nfd, timeout);
	DRESULT(r);
}

// N7 では SETFD で full-duplex にしたことが close に反映されてないので、
// O_RDONLY でオープンして、SETFD して再生している状態で
// close すると drain 他が実行されないような気がする。
int
cmd_SETFD(int ac, char *av[])
{
	int fdfile;
	int fd;
	int r;
	int on;

	if (ac < 2) {
		printf("usage: <filename.wav>\n");
		exit(1);
	}

	// ファイルオープン
	fdfile = OPEN(av[1], O_RDONLY);
	if (fdfile == -1)
		err(1, "open: %s", av[1]);

	// Half-Duplex でオープン
	fd = OPEN(devaudio, O_RDONLY);
	if (fd == -1)
		err(1, "open");

	// Full-Duplex
	on = 1;
	r = IOCTL(fd, AUDIO_SETFD, &on, "on");
	if (r == -1)
		err(1, "ioctl");

	// 再生
	char buf[32768];
	r = READ(fdfile, buf, sizeof(buf));
	r = WRITE(fd, buf, r);
	if (r == -1)
		err(1, "write");

	// クローズ
	r = CLOSE(fd);
	if (r == -1)
		err(1, "close");

	CLOSE(fdfile);
	return 0;
}

// PLAY_SYNC でブロックサイズずつ書き込んでみる。
// 引数(double) はブロックサイズ長に対する書き込みの割合
// N7 だと ratio=1 でも <1 でも途切れた音が出る?
// error が点灯する条件はよく分からん。VirtualBox だと <1 なら点灯する。
// VMwarePlayer だと値によって変わるようだ。
int
cmd_playsync(int ac, char *av[])
{
	struct audio_info ai;
	char *wav;
	char *buf;
	int wavsize;
	int fd;
	int r;
	double ratio;
	int sinewave[] = {
		0, 0xb0, 0x80, 0xb0, 0, 0x30, 0x00, 0x30
	};

	if (ac < 2)
		ratio = 1;
	else
		ratio = atof(av[1]);
	printf("ratio = %5.1f\n", ratio);

	fd = OPEN(devaudio, O_WRONLY);
	if (fd == -1)
		err(1, "open");

	AUDIO_INITINFO(&ai);
	ai.mode = AUMODE_PLAY;
	r = IOCTL(fd, AUDIO_SETINFO, &ai, "mode");
	if (r == -1)
		err(1, "AUDIO_SETINFO");

	r = IOCTL(fd, AUDIO_GETBUFINFO, &ai, "");
	if (r == -1)
		err(1, "AUDIO_GETINFO");
	printf("ai.blocksize=%d\n", ai.blocksize);

	wavsize = ai.blocksize;
	wav = (char *)malloc(wavsize);
	if (wav == NULL)
		err(1, "malloc");
	buf = (char *)malloc(wavsize);
	if (buf == NULL)
		err(1, "malloc");

	// 三角波
	// mulaw 8kHz で 1kHz を想定しているけど、適当
	for (int i = 0; i < wavsize; i++) {
		wav[i] = sinewave[i % 8];
	}
	int len = wavsize * ratio;
	printf("write len = %d\n", len);
	for (int i = 0; i < 5; i++) {
		r = WRITE(fd, wav, len);
		if (r == -1)
			err(1, "write");
		if (r != len)
			errx(1, "write: too short: %d", r);
	}

	r = IOCTL(fd, AUDIO_GETBUFINFO, &ai, "");
	if (r == -1)
		err(1, "AUDIO_GETINFO");
	printf("ai.play.error = %d\n", ai.play.error);

	CLOSE(fd);
	free(wav);

	return 0;
}

// write が戻ってくるまでの時間を調べてみる。
// 引数は 1つ目がバッファサイズに対する1回の書き込み長の割合、省略なら1.0
int
cmd_writetime(int ac, char *av[])
{
	struct audio_info ai;
	struct timeval start, end, result;
	struct timeval total;
	char *buf;
	int bufsize;
	int fd;
	int r;
	double ratio;

	ratio = 1;
	switch (ac) {
	 case 2:
		ratio = atof(av[1]);
		/* FALLTHROUGH */
	 case 1:
		break;
	 default:
		errx(1, "invalid argument");
	}

	fd = OPEN(devaudio, O_WRONLY);
	if (fd == -1)
		err(1, "open");

	AUDIO_INITINFO(&ai);
	ai.play.encoding = AUDIO_ENCODING_SLINEAR_LE;
	ai.play.precision = 16;
	ai.play.channels = x68k ? 1 : 2;
	ai.play.sample_rate = x68k ? 16000 : 48000;
	r = IOCTL(fd, AUDIO_SETINFO, &ai, "");
	if (r == -1)
		err(1, "AUDIO_SETINFO");

	// bufsize を取得
	r = IOCTL(fd, AUDIO_GETBUFINFO, &ai, "");
	if (r == -1)
		err(1, "AUDIO_GETINFO");
	bufsize = (double)ai.play.buffer_size * ratio;
	printf("bufsize = %d (ratio=%4.2f)\n", bufsize, ratio);

	buf = (char *)malloc(bufsize);
	if (buf == NULL)
		err(1, "malloc");
	memset(buf, 0, bufsize);

	memset(&total, 0, sizeof(total));
	for (int i = 0; i < 5; i++) {
		gettimeofday(&start, NULL);
		r = WRITE(fd, buf, bufsize);
		if (r == -1)
			err(1, "write");
		if (r != bufsize)
			errx(1, "write: too short");
		gettimeofday(&end, NULL);
		timersub(&end, &start, &result);
		timeradd(&total, &result, &total);
		printf("write %d.%06d\n", (int)result.tv_sec, (int)result.tv_usec);
	}

	gettimeofday(&start, NULL);
	CLOSE(fd);
	gettimeofday(&end, NULL);
	timersub(&end, &start, &result);
	timeradd(&total, &result, &total);
	printf("drain %d.%06d\n", (int)result.tv_sec, (int)result.tv_usec);
	printf("total %d.%06d\n", (int)total.tv_sec, (int)total.tv_usec);

	free(buf);
	return 0;
}

// pyon_s16le.wav を途中で DRAIN 発行して、続けてみる。
int
cmd_drain(int ac, char *av[])
{
	struct audio_info ai;
	char *buf;
	int fd;
	int ff;
	int r;

	fd = OPEN(devaudio, O_WRONLY);
	if (fd == -1)
		err(1, "open");

	AUDIO_INITINFO(&ai);
	ai.play.encoding = AUDIO_ENCODING_SLINEAR_LE;
	ai.play.precision = 16;
	ai.play.channels = 1;
	ai.play.sample_rate = 44100;
	r = IOCTL(fd, AUDIO_SETINFO, &ai, "");
	if (r == -1)
		err(1, "AUDIO_SETINFO");

	ff = OPEN("pyon_s16le.wav", O_RDONLY);
	if (ff == -1)
		err(1, "open: pyon_s16le.wav");

	buf = (char *)mmap(NULL, 305228, PROT_READ, MAP_FILE, ff, 0);
	if (buf == MAP_FAILED)
		err(1, "mmap");

	r = WRITE(fd, buf + 44, 152592);
	if (r == -1)
		err(1, "write1");
	if (r < 152592)
		errx(1, "write1: too short: %d", r);

	r = IOCTL(fd, AUDIO_DRAIN, 0, "");
	if (r == -1)
		err(1, "drain");

	printf("sleep 1\n");
	sleep(1);

	r = WRITE(fd, buf + 44 + 152592, 152592);
	if (r == -1)
		err(1, "write2");
	if (r < 152592)
		errx(1, "write2: too short: %d", r);

	CLOSE(fd);
	munmap(buf, 305228);
	CLOSE(ff);
	return 0;
}

// N8 で panic: eap_trigger_input: already running を起こす
int
cmd_eap_input(int ac, char *av[])
{
	struct audio_info ai;
	int fd;
	int r;

	fd = OPEN(devsound, O_RDONLY);
	if (fd == -1)
		err(1, "open");

	AUDIO_INITINFO(&ai);
	ai.record.pause = 1;
	r = IOCTL(fd, AUDIO_SETINFO, &ai, "");
	if (r == -1)
		err(1, "ioctl");
	CLOSE(fd);

	fd = OPEN(devsound, O_RDONLY);
	if (fd == -1)
		err(1, "open");
	CLOSE(fd);

	fd = OPEN(devsound, O_RDONLY);
	if (fd == -1)
		err(1, "open");
	CLOSE(fd);

	return 0;
}

// STDOUT を POLLIN してみるテスト
int
cmd_poll_1(int ac, char *av[])
{
	struct pollfd pfd;
	int r;

	memset(&pfd, 0, sizeof(pfd));
	pfd.fd = STDOUT_FILENO;
	pfd.events = POLLIN;

	r = POLL(&pfd, 1, 2000);
	if (r == -1)
		err(1, "poll");

	printf("r = %d\n", r);
	printf("pfd.revents = 0x%x\n", pfd.revents);
	return 0;
}

// mmap で再生してみるテスト。
// XXX まだ作りかけで動かない。
int
cmd_playmmap(int ac, char *av[])
{
	struct audio_info ai;
	const char *filename;
	int filefd;
	int audiofd;
	int r;
	char *ptr;
	int blksize;

	if (ac != 2)
		errx(1, "%s playmmap filename\n", getprogname());
	filename = av[1];

	filefd = OPEN(filename, O_RDONLY);
	if (filefd == -1)
		err(1, "open: %s", filename);

	audiofd = OPEN(devaudio, O_WRONLY);
	if (audiofd == -1)
		err(1, "open: %s", devaudio);

	AUDIO_INITINFO(&ai);
	ai.play.encoding = AUDIO_ENCODING_SLINEAR_LE;
	ai.play.precision = 16;
	ai.play.sample_rate = 44100;
	ai.play.channels = 2;
	r = IOCTL(audiofd, AUDIO_SETINFO, &ai, "set");
	if (r == -1)
		err(1, "AUDIO_SETINFO");

	memset(&ai, 0, sizeof(ai));
	r = IOCTL(audiofd, AUDIO_GETBUFINFO, &ai, "get");
	if (r == -1)
		err(1, "AUDIO_GETBUFINFO");

	blksize = ai.blocksize;
	printf("bufsize=%u blksize=%d\n", ai.play.buffer_size, blksize);

	ptr = (char *)mmap(NULL, ai.play.buffer_size, PROT_WRITE, 0,
		audiofd, 0);
	if (ptr == MAP_FAILED)
		err(1, "mmap");

	for (;; usleep(100)) {
		struct audio_offset ao;
		r = IOCTL(audiofd, AUDIO_GETOOFFS, &ao, "GETOOFFS");
		if (r == -1)
			err(1, "AUDIO_GETOFFS");

		printf("GETOOFFS: s=%u d=%u o=%u\n",
			ao.samples, ao.deltablks, ao.offset);

		if (ao.deltablks == 0)
			continue;

		r = READ(filefd, ptr + ao.offset, blksize);
		if (r == -1)
			err(1, "read");
		if (r != blksize)
			break;
	}

	munmap(ptr, ai.play.buffer_size);
	close(audiofd);
	close(filefd);
	return 0;
}

#define PAD_AUDIO_UNIT 1

// pad を先に close する
int
cmd_pad_close(int ac, char *av[])
{
	char devname[16];
	int fdpad;
	int fdaudio;
	int unit;
	int r;

	fdpad = OPEN("/dev/pad", O_RDONLY);
	if (fdpad == -1) {
		if (errno == ENXIO) {
			// pad が組み込まれていない
			errx(1, "pad seems not be configured.");
		}
		err(1, "open: dev/pad");
	}

	// 可能なら紐づいてる audio を自動取得
	r = IOCTL(fdpad, PAD_GET_AUDIOUNIT, &unit, "");
	if (r == -1) {
		if (errno == ENODEV) {
			// この ioctl がない環境なので仕方ないのでコンパイル時指定
			unit = PAD_AUDIO_UNIT;
			printf("  > No GET_AUDIOUNIT ioctl so use audio%d\n", unit);
		} else {
			// それ以外はエラー
			err(1, "PAD_GET_AUDIOUNIT");
		}
	}
	snprintf(devname, sizeof(devname), "/dev/audio%d", unit);

	fdaudio = OPEN(devname, O_WRONLY);
	if (fdaudio == -1)
		err(1, "open: %s", devname);

	r = CLOSE(fdpad);

	r = CLOSE(fdaudio);

	return 0;
}

// mixer async が realloc を起こすあたりを確認するため、
// 手動で FIOASYNC オンにしたまま入力を待つだけ。
// hw.audio0.debug=2 にして手動で頑張って起動してね。
int
cmd_mixer_async(int ac, char *av[])
{
	char buf[10];
	int fd;
	int val;
	int r;

	fd = OPEN(devmixer, O_RDWR);
	if (fd == -1) {
		err(1, "open");
	}

	val = 1;
	r = IOCTL(fd, FIOASYNC, &val, "on");
	if (r == -1)
		err(1, "FIOASYNC");

	fgets(buf, sizeof(buf), stdin);

	CLOSE(fd);
	return 0;
}

// コマンド一覧
#define DEF(x)	{ #x, cmd_ ## x }
struct cmdtable cmdtable[] = {
	DEF(SETFD),
	DEF(playsync),
	DEF(writetime),
	DEF(drain),
	DEF(eap_input),
	DEF(poll_1),
	DEF(playmmap),
	DEF(pad_close),
	DEF(mixer_async),
	{ NULL, NULL },
};
#undef DEF
