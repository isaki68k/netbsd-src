/*
 * Test program to execute main routine in userland.
 */

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/time.h>

#include "userland.h"		/* required by audioio.h */
#include <sys/audioio.h>	/* required by audiovar.h */
#include "audiovar.h"
#include "audiodef.h"
#include "aucodec.c"
#include "mulaw.c"
#include "alaw.c"

#define malloc(len, a, b)	malloc(len)
#define AUDIO_DEBUG 0
#include "audio.c"
#undef malloc

struct test_file
{
	audio_file_t *file;
	audio_ring_t mem;
	bool play;
	int wait;
};

typedef struct {
	uint32_t magic;
	uint32_t offset;
	uint32_t length;
	uint32_t format;
#define SND_FORMAT_MULAW_8		(1)
#define SND_FORMAT_LINEAR_8		(2)
#define SND_FORMAT_LINEAR_16	(3)
#define SND_FORMAT_LINEAR_24	(4)
#define SND_FORMAT_LINEAR_32	(5)
#define SND_FORMAT_FLOAT		(6)
#define SND_FORMAT_DOUBLE		(7)
#define SND_FORMAT_ALAW_8		(27)
	uint32_t sample_rate;
	uint32_t channels;
	char text[0];
} AUFileHeader;

enum {
	CMD_NONE = 0,
	CMD_FILE,
	CMD_PLAY,
	CMD_TEST,
	CMD_PERF,
};

struct freqdata {
	int srcfreq;
	int dstfreq;
	const char *name;
};

struct testdata {
	const char *testname;
	int (*funcname)();
};

int child_loop(struct test_file *, int);
int cmd_print_file(int, char **);
int print_file(const char *);
int cmd_set_file(const char *);
int cmd_play();
int cmd_test(int, char **, struct testdata*);
int test_mixer_calc_blktime();
int perf_codec_slinear_to_mulaw();
int perf_codec_linear16_to_internal();
int perf_freq_up();
int perf_freq_down();
int perf_freq_main(struct freqdata *);
int perf_chmix_mixLR();
int parse_file(struct test_file *, FILE *, const char *);
uint16_t lebe16toh(uint16_t);
uint32_t lebe32toh(uint32_t);
const char *fmt_tostring(const audio_format2_t *);
uint64_t filesize(FILE *);
const char *audio_encoding_name(int);
const char *tagname(uint32_t);

audio_file_t *sys_open(int);
int/*ssize_t*/ sys_write(audio_file_t *, void *, size_t);
int sys_ioctl_drain(audio_track_t *);

extern struct testdata testdata[];
extern struct testdata perfdata[];

struct audio_softc *sc;
struct test_file files[16];
int debug;
int rifx;
int fileidx;
int opt_wait;	// 1ファイルごとの開始ディレイ
int opt_vol;
int audio_blk_ms;
const char *devicefile;
int hw_chan;
int hw_freq;

void
usage()
{
	printf("usage: [options] <cmd> [files...]\n");
	printf("options:\n");
	printf(" -D <dev>  device name (default: /dev/sound)\n");
	printf(" -d        debug\n");
	printf(" -C <ch>   hw channels (default: 2)\n");
	printf(" -F <freq> hw frequency (default: 48000)\n");
	printf(" -m <msec> AUDIO_BLK_MS (default: 40)\n");
	printf(" -w <cnt>  delay block count for each files\n");
	printf(" -v <vol>  track volume (0..256)\n");
	printf("cmd:\n");
	printf(" file <files...>  print format (without playback)\n");
	printf(" play <files...>  play files\n");
	printf(" perf <testname>  performance test\n");
	printf(" test <testname>  test\n");
	exit(1);
}

int
main(int ac, char *av[])
{
	int c;
	int cmd;

	opt_vol = 256;
	audio_blk_ms = AUDIO_BLK_MS;
	devicefile = "/dev/sound";
	hw_chan = 2;
	hw_freq = 48000;

	while ((c = getopt(ac, av, "C:D:F:dm:w:v:")) != -1) {
		switch (c) {
		 case 'C':
			hw_chan = atoi(optarg);
			break;
		 case 'D':
			devicefile = optarg;
			break;
		 case 'd':
			debug++;
			break;
		 case 'F':
			hw_freq = atoi(optarg);
			break;
		 case 'm':
			audio_blk_ms = atoi(optarg);
			break;
		 case 'w':
			opt_wait = atoi(optarg);
			break;
		 case 'v':
			opt_vol = atoi(optarg);
			break;
		 default:
			usage();
			break;
		}
	}
	ac -= optind;
	av += optind;

	// コマンド名
	if (ac == 0)
		usage();
	if (strcmp(av[0], "file") == 0) {
		cmd = CMD_FILE;
	} else if (strcmp(av[0], "play") == 0) {
		cmd = CMD_PLAY;
	} else if (strcmp(av[0], "perf") == 0) {
		cmd = CMD_PERF;
	} else if (strcmp(av[0], "test") == 0) {
		cmd = CMD_TEST;
	} else {
		usage();
	}
	ac--;
	av++;

	// コマンドごとにその後の引数
	int r = 0;
	switch (cmd) {
	 case CMD_FILE:
		if (ac == 0)
			usage();
		r = cmd_print_file(ac, av);
		break;
	 case CMD_PLAY:
		if (ac == 0)
			usage();
		audio_attach(&sc, true);
		for (int i = 0; i < ac; i++) {
			r = cmd_set_file(av[i]);
			if (r != 0)
				break;
		}
		r = cmd_play();
		audio_detach(sc);
		break;

	 case CMD_PERF:
#if defined(AUDIO_ASSERT)
		printf("warn: perf but AUDIO_ASSERT defined\n");
#endif
		/* FALLTHROUGH */

	 case CMD_TEST:
		audio_attach(&sc, false);
		if (cmd == CMD_PERF)
			r = cmd_test(ac, av, perfdata);
		else
			r = cmd_test(ac, av, testdata);
		audio_detach(sc);
		break;
	}

	return r;
}

int
cmd_print_file(int ac, char *av[])
{
	for (int i = 0; i < ac; i++) {
		print_file(av[i]);
	}
	return 0;
}

// filename のヘッダとか内容を表示します。
int
print_file(const char *filename)
{
	struct test_file *f = &files[fileidx];

	// ファイル名なら音声ファイルとして開く
	FILE *fp = fopen(filename, "rb");
	if (fp == NULL) {
		printf("ERROR: fopen: %s\n", filename);
		return 1;
	}

	// ファイル形式を調べて f に色々格納。
	int len = parse_file(f, fp, filename);
	fclose(fp);

	printf("%s: %s\n", filename, fmt_tostring(&f->mem.fmt));
	return 0;
}

// filename を再生用にセットします。
int
cmd_set_file(const char *filename)
{
	struct test_file *f = &files[fileidx];
	f->mem.head = 0;

	// ファイル名なら音声ファイルとして開く
	FILE *fp = fopen(filename, "rb");
	if (fp == NULL) {
		printf("ERROR: fopen: %s\n", filename);
		return 1;
	}

	// ファイル形式を調べて f に色々格納。
	int len = parse_file(f, fp, filename);
	fclose(fp);

	printf("%s: %s\n", filename, fmt_tostring(&f->mem.fmt));

	f->mem.capacity = len * 8 / f->mem.fmt.stride / f->mem.fmt.channels;
	f->mem.used = f->mem.capacity;

	f->file = sys_open(AUMODE_PLAY);
	/* この辺は ioctl になる */
	f->file->ptrack->volume = opt_vol;
	f->file->ptrack->mixer->volume = 256;
	for (int j = 0; j < 2; j++) {
		f->file->ptrack->ch_volume[j] = 256;
	}

	audio_track_set_format(f->file->ptrack, &f->mem.fmt);

	f->play = true;
	f->wait = opt_wait;
	fileidx++;
	return 0;
}

int
cmd_play()
{
	for (int loop = 0; ; loop++) {
		bool isPlay = false;
		for (int i = 0; i < fileidx; i++) {
			struct test_file *f = &files[i];
			if (f->play)
				child_loop(f, loop);
			if (f->play)
				isPlay = true;
		}
		if (isPlay == false)
			break;
	}

	return 0;

}

// ループが終わるときは -1
int
child_loop(struct test_file *f, int loop)
{
	if (f->wait > loop) return 0;

	// 1ブロック分のフレーム数
	int frames_per_block = frame_per_block(f->file->ptrack->mixer, &f->mem.fmt);
	// 今回再生するフレーム数
	int frames = min(f->mem.used, frames_per_block);
	// フレーム数をバイト数に
	int bytes = frames * f->mem.fmt.channels * f->mem.fmt.stride / 8;

	sys_write(f->file, auring_headptr(&f->mem), bytes);
	auring_take(&f->mem, frames);

	if (frames < frames_per_block) {
		// 最後
		f->play = false;
		return -1;
	}
	return 0;
}

#define GETID(ptr) (be32toh(*(uint32_t *)(ptr)))
static inline uint32_t ID(const char *str) {
	const unsigned char *ustr = (const unsigned char *)str;
	return (ustr[0] << 24)
	     | (ustr[1] << 16)
	     | (ustr[2] << 8)
	     | (ustr[3]);
}
static inline int IDC(uint32_t value) {
	value &= 0xff;
	return isprint(value) ? value : '?';
}

// ファイル形式を調べて f->fmt にパラメータをセット、
// ファイル(データ)本体を f->mem.sample にセットする。
// 戻り値はデータ本体のバイト数。
int
parse_file(struct test_file *f, FILE *fp, const char *filename)
{
	char hdrbuf[256];
	uint32_t tag;
	int len;

	// ヘッダ抽出用に先頭だけ適当に取り出す
	fread(hdrbuf, 1, sizeof(hdrbuf), fp);

	// WAV
	// 先頭が RIFF で +8 からが WAVE ならリトルエンディアン
	// 先頭が RIFX で +8 からが WAVE ならビッグエンディアン
	if ((GETID(hdrbuf) == ID("RIFF") || GETID(hdrbuf) == ID("RIFX"))
	 && GETID(hdrbuf + 8) == ID("WAVE")) {
		char *s;

		// 先頭が RIFF なら LE、RIFX なら BE
		rifx = (GETID(hdrbuf) == ID("RIFX")) ? 1 : 0;

		// "WAVE" の続きから、チャンクをたぐる
		s = hdrbuf + 12;
		for (; s < hdrbuf + sizeof(hdrbuf);) {
			// このチャンクのタグ、長さ (以降のチャンク本体のみの長さ)
			tag = GETID(s); s += 4;
			len = lebe32toh(*(uint32_t *)s); s += 4;
			if (debug)
				printf("tag=%s len=%d\n", tagname(tag), len);
			if (tag == ID("fmt ")) {
				WAVEFORMATEX *wf = (WAVEFORMATEX *)s;
				s += len;
				f->mem.fmt.channels = (uint8_t)lebe16toh(wf->nChannels);
				f->mem.fmt.sample_rate = lebe32toh(wf->nSamplesPerSec);
				f->mem.fmt.precision = (uint8_t)lebe16toh(wf->wBitsPerSample);
				f->mem.fmt.stride = f->mem.fmt.precision;
				if (rifx) {
					f->mem.fmt.encoding = AUDIO_ENCODING_SLINEAR_BE;
				} else {
					if (f->mem.fmt.precision == 8)
						f->mem.fmt.encoding = AUDIO_ENCODING_ULINEAR_LE;
					else
						f->mem.fmt.encoding = AUDIO_ENCODING_SLINEAR_LE;
				}
				uint16_t fmtid = lebe16toh(wf->wFormatTag);
				if (fmtid == 0xfffe) {
					// 拡張形式 (今は使ってない)
					//WAVEFORMATEXTENSIBLE *we = (WAVEFORMATEXTENSIBLE *)wf;
				} else {
					if (fmtid != 1) {
						printf("%s: Unsupported WAV format: fmtid=%x\n",
							filename, fmtid);
						exit(1);
					}
				}
			} else if (tag == ID("data")) {
				break;
			} else {
				// それ以外の知らないタグは飛ばさないといけない
				s += len;
			}
		}

		f->mem.mem = malloc(len);
		if (f->mem.mem == NULL) {
			printf("%s: malloc failed: %d\n", filename, len);
			exit(1);
		}
		// 読み込み開始位置は s
		fseek(fp, (long)(s - hdrbuf), SEEK_SET);
		fread(f->mem.mem, 1, len, fp);
		return len;
	}

	// AU
	// すべてビッグエンディアン
	if (GETID(hdrbuf) == ID(".snd")) {
		AUFileHeader *au = (AUFileHeader *)hdrbuf;
		uint32_t format = be32toh(au->format);
		switch (format) {
		 case SND_FORMAT_MULAW_8:
			f->mem.fmt.encoding = AUDIO_ENCODING_ULAW;
			f->mem.fmt.precision = 8;
			break;
		 case SND_FORMAT_LINEAR_8:
			f->mem.fmt.encoding = AUDIO_ENCODING_SLINEAR_BE;
			f->mem.fmt.precision = 8;
			break;
		 case SND_FORMAT_LINEAR_16:
			f->mem.fmt.encoding = AUDIO_ENCODING_SLINEAR_BE;
			f->mem.fmt.precision = 16;
			break;
		 case SND_FORMAT_LINEAR_24:
			f->mem.fmt.encoding = AUDIO_ENCODING_SLINEAR_BE;
			f->mem.fmt.precision = 24;
			break;
		 case SND_FORMAT_LINEAR_32:
			f->mem.fmt.encoding = AUDIO_ENCODING_SLINEAR_BE;
			f->mem.fmt.precision = 32;
			break;
		 default:
			printf("%s: Unknown format: format=%d\n", filename, format);
			exit(1);
		}
		f->mem.fmt.channels = be32toh(au->channels);
		f->mem.fmt.sample_rate = be32toh(au->sample_rate);
		f->mem.fmt.stride = f->mem.fmt.precision;
		len = be32toh(au->length);
		uint32_t offset = be32toh(au->offset);

		f->mem.mem = malloc(len);
		if (f->mem.mem == NULL) {
			printf("%s: malloc failed: %d\n", filename, len);
			exit(1);
		}
		// 読み込み開始位置は offset
		fseek(fp, offset, SEEK_SET);
		fread(f->mem.mem, 1, len, fp);
		return len;
	}

	printf("unknown file format\n");
	exit(1);
}

// ファイルサイズを取得する。ついでにポジションを 0 に戻す。
uint64_t
filesize(FILE *fp)
{
	uint64_t len;
	fseek(fp, 0, SEEK_END);
	len = ftell(fp);
	fseek(fp, 0, SEEK_SET);
	return len;
}

// 大域変数 rifx によって le16toh()/be16toh() を切り替える
uint16_t
lebe16toh(uint16_t x)
{
	if (rifx)
		return be16toh(x);
	else
		return le16toh(x);
}

// 大域変数 rifx によって le32toh()/be32toh() を切り替える
uint32_t
lebe32toh(uint32_t x)
{
	if (rifx)
		return be32toh(x);
	else
		return le32toh(x);
}

// fmt の表示用文字列を返す
const char *
fmt_tostring(const audio_format2_t *fmt)
{
	static char buf[64];
	char stridebuf[16];

	// stride は precision と違う時か、24bit なら常に表示
	stridebuf[0] = '\0';
	if (fmt->stride != fmt->precision || fmt->precision == 24)
		sprintf(stridebuf, "/%d", fmt->stride);

	snprintf(buf, sizeof(buf), "%s, %d%sbit, %dch, %dHz",
		audio_encoding_name(fmt->encoding),
		fmt->precision,
		stridebuf,
		fmt->channels,
		fmt->sample_rate);
	return buf;
}

const char *
tagname(uint32_t tag)
{
	static char buf[32];

	snprintf(buf, sizeof(buf), "%x '%c%c%c%c'",
		tag,
		IDC(tag>>24),
		IDC(tag>>16),
		IDC(tag>>8),
		IDC(tag));
	return buf;
}

/*
 * ***** audio_file *****
 */

void
audio_softc_init(const audio_format2_t *phwfmt, const audio_format2_t *rhwfmt)
{
	audio_filter_reg_t pfil, rfil;

	sc->sc_sound_pparams = params_to_format2(&audio_default);
	sc->sc_sound_rparams = params_to_format2(&audio_default);
	sc->sc_blk_ms = audio_blk_ms;
	printf("blk_ms = %d\n", sc->sc_blk_ms);

	audio_mixers_init(sc, AUMODE_PLAY | AUMODE_RECORD,
		phwfmt, rhwfmt, &pfil, &rfil);
}

audio_file_t *
sys_open(int mode)
{
	audio_file_t *file;
	int error;

	file = calloc(1, sizeof(audio_file_t));
	file->sc = sc;
	file->mode = mode;

	if (mode == AUMODE_PLAY) {
		error = audio_track_init(sc, &file->ptrack, AUMODE_PLAY);
		if (error) {
			printf("audio_track_init(PLAY) failed: %s\n", strerror(error));
			exit(1);
		}
	} else {
		error = audio_track_init(sc, &file->rtrack, AUMODE_RECORD);
		if (error) {
			printf("audio_track_init(RECORD) failed: %s\n", strerror(error));
			exit(1);
		}
	}

	SLIST_INSERT_HEAD(&sc->sc_files, file, entry);

	return file;
}

int//ssize_t
sys_write(audio_file_t *file, void* buf, size_t len)
{
	KASSERT(buf);

	if (len > INT_MAX) {
		errno = EINVAL;
		return -1;
	}

	struct uio uio = buf_to_uio(buf, len, UIO_READ);

	mutex_enter(file->sc->sc_lock);
	int error = audio_write(file->sc, &uio, 0, file);
	mutex_exit(file->sc->sc_lock);
	if (error) {
		errno = error;
		return -1;
	}
	return (int)len;
}

// ioctl(AUDIO_DRAIN) 相当
int
sys_ioctl_drain(audio_track_t *track)
{
	mutex_enter(sc->sc_lock);
	audio_track_drain(sc, track);
	mutex_exit(sc->sc_lock);

	return 0;
}

// テストパターン
struct testdata testdata[] = {
	{ "mixer_calc_blktime",		test_mixer_calc_blktime },
	{ NULL, NULL },
};

// パフォーマンステスト
struct testdata perfdata[] = {
	{ "codec_slinear_to_mulaw",	perf_codec_slinear_to_mulaw },
	{ "codec_linear16_to_internal", perf_codec_linear16_to_internal },
	{ "freq_up",	perf_freq_up },
	{ "freq_down",	perf_freq_down },
	{ "chmix_mixLR",perf_chmix_mixLR },
	{ NULL, NULL },
};

// test と perf の共通部
int
cmd_test(int ac, char *av[], struct testdata *testlist)
{
	int i;
	bool found;

	/* -a なら all */
	if (ac == 1 && strcmp(av[0], "-a") == 0) {
		for (i = 0; testlist[i].testname != NULL; i++) {
			(testlist[i].funcname)();
		}
		return 0 ;
	}
	/* そうでなければ順にマッチするものをテスト */
	found = false;
	for (i = 0; i < ac; i++) {
		for (int j = 0; testlist[j].testname != NULL; j++) {
			if (strncmp(av[i], testlist[j].testname, strlen(av[i])) == 0) {
				(testlist[j].funcname)();
				found = true;
			}
		}
	}

	if (!found) {
		// 一つもなければ一覧を表示しとくか
		printf("valid testnames are:\n");
		for (i = 0; testlist[i].testname != NULL; i++) {
			printf(" %s\n", testlist[i].testname);
		}
		return 1;
	}
	return 0;
}

int
test_mixer_calc_blktime()
{
	struct {
		int stride;
		int sample_rate;
		int expected;
	} table[] = {
		{ 8,	11025,	40 },
		{ 8,	48000,	40 },
		{ 4,	15625,	80 },
	};
	int failcount;

	// XXX AUDIO_BLK_MS を変えるテストは難しい。
	printf("mixer_calc_blktime:\n");
	failcount = 0;
	for (int i = 0; i < __arraycount(table); i++) {
		audio_trackmixer_t mixer;
		memset(&mixer, 0, sizeof(mixer));
		mixer.hwbuf.fmt.stride = table[i].stride;
		mixer.hwbuf.fmt.sample_rate = table[i].sample_rate;

		u_int actual = audio_mixer_calc_blktime(sc, &mixer);
		if (actual != table[i].expected) {
			printf("test %d expects %d but %d\n",
				i, table[i].expected, actual);
			failcount++;
		}
	}
	if (failcount == 0) {
		printf("ok\n");
	} else {
		printf("%d failed\n", failcount);
	}
	return 0;
}

int
perf_freq_up()
{
	struct freqdata pattern[] = {
		{ 44100, 48000,	"44.1->48" },
		{ 8000,  48000, "8->48" },
		{ -1, -1, NULL },
	};
	return perf_freq_main(pattern);
}

int
perf_freq_down()
{
	struct freqdata pattern[] = {
		{ 48000, 44100,	"48->44.1" },
		{ 48000, 8000,	"48->8" },
		{ -1, -1, NULL },
	};
	return perf_freq_main(pattern);
}

volatile int signaled;

void
sigalrm(int signo)
{
	signaled = 1;
}

int
perf_codec_slinear_to_mulaw()
{
	struct test_file *f = &files[fileidx];
	audio_track_t *track;
	audio_format2_t srcfmt;
	struct timeval start, end, result;
	struct itimerval it;

	signal(SIGALRM, sigalrm);
	memset(&it, 0, sizeof(it));
	it.it_value.tv_sec = 3;

	f->file = sys_open(AUMODE_RECORD);
	track = f->file->rtrack;
	// src fmt
	srcfmt = track->mixer->track_fmt;
	srcfmt.encoding = AUDIO_ENCODING_ULAW;
	srcfmt.precision = 8;
	srcfmt.stride = 8;
	// set
	int r = audio_track_set_format(track, &srcfmt);
	if (r != 0) {
		printf("%s: audio_track_set_format failed: %s\n",
			__func__, strerror(errno));
		exit(1);
	}

	{
		int16_t v;
		int16_t *p = (int16_t *)track->codec.srcbuf.mem;
		printf("buffer capacity: %d\n", track->codec.srcbuf.capacity);
		for (int i = 0; i < track->codec.srcbuf.capacity; i++) {
			for (int ch = 0; ch < track->codec.srcbuf.fmt.channels; ch++) {
				v = (int16_t)((uint16_t)(random() >> 3));
				*p++ = v;
			}
		}
	}

	uint64_t count;
	printf("slinear_to_mulaw: ");
	fflush(stdout);


	setitimer(ITIMER_REAL, &it, NULL);
	gettimeofday(&start, NULL);
	for (count = 0, signaled = 0; signaled == 0; count++) {
		track->codec.srcbuf.head = 0;
		track->codec.srcbuf.used = frame_per_block(track->mixer,
			&srcfmt);
		track->outbuf.head = 0;
		track->outbuf.used = 0;
		audio_apply_stage(track, &track->codec, false);
	}
	gettimeofday(&end, NULL);
	timersub(&end, &start, &result);

#if defined(__m68k__)
	printf("%d %5.1f times/sec\n", (int)count,
		(double)count/(result.tv_sec + (double)result.tv_usec/1000000));
#else
	printf("%d %5.1f times/msec\n", (int)count,
		(double)count/((uint64_t)result.tv_sec*1000 + result.tv_usec/1000));
#endif

	return 0;
}

const char *
encname(int enc)
{
	static char buf[16];
	switch (enc) {
	 case AUDIO_ENCODING_SLINEAR_LE:	return "SLINEAR_LE";
	 case AUDIO_ENCODING_SLINEAR_BE:	return "SLINEAR_BE";
	 case AUDIO_ENCODING_ULINEAR_LE:	return "ULINEAR_LE";
	 case AUDIO_ENCODING_ULINEAR_BE:	return "ULINEAR_BE";
	 default:
		snprintf(buf, sizeof(buf), "enc%d", enc);
		return buf;
	}
}

int
perf_codec_linear16_to_internal()
{
	struct test_file *f = &files[fileidx];
	audio_track_t *track;
	audio_format2_t srcfmt;
	struct timeval start, end, result;
	struct itimerval it;
	int enc[3] = {
		AUDIO_ENCODING_SLINEAR_OE,
		AUDIO_ENCODING_ULINEAR_NE,
		AUDIO_ENCODING_ULINEAR_OE,
	};

	signal(SIGALRM, sigalrm);
	memset(&it, 0, sizeof(it));
	it.it_value.tv_sec = 3;

	f->file = sys_open(AUMODE_PLAY);
	track = f->file->ptrack;

	for (int i = 0; i < __arraycount(enc); i++) {
		printf("linear16_to_internal(%s): ", encname(enc[i]));
		fflush(stdout);

		// src fmt
		srcfmt = track->mixer->track_fmt;
		srcfmt.encoding = enc[i];
		srcfmt.precision = 16;
		srcfmt.stride = 16;
		// set
		int r = audio_track_set_format(track, &srcfmt);
		if (r != 0) {
			printf("%s: audio_track_set_format failed: %s\n",
				__func__, strerror(errno));
			exit(1);
		}

		setitimer(ITIMER_REAL, &it, NULL);
		gettimeofday(&start, NULL);
		uint64_t count;
		for (count = 0, signaled = 0; signaled == 0; count++) {
			track->codec.srcbuf.head = 0;
			track->codec.srcbuf.used = frame_per_block(track->mixer,
				&srcfmt);
			track->outbuf.head = 0;
			track->outbuf.used = 0;
			audio_apply_stage(track, &track->codec, false);
		}
		gettimeofday(&end, NULL);
		timersub(&end, &start, &result);

#if defined(__m68k__)
		printf("%d %5.1f times/sec\n", (int)count,
			(double)count/(result.tv_sec + (double)result.tv_usec/1000000));
#else
		printf("%d %5.1f times/msec\n", (int)count,
			(double)count/((uint64_t)result.tv_sec*1000 + result.tv_usec/1000));
#endif
	}

	return 0;
}

int
perf_freq_main(struct freqdata *pattern)
{
	struct test_file *f = &files[fileidx];
	audio_track_t *track;
	audio_format2_t srcfmt;
	struct timeval start, end, result;
	struct itimerval it;

	signal(SIGALRM, sigalrm);
	memset(&it, 0, sizeof(it));
	it.it_value.tv_sec = 3;

	f->file = sys_open(AUMODE_PLAY);
	track = f->file->ptrack;
	for (int i = 0; pattern[i].name != NULL; i++) {
		// dst fmt
		track->mixer->track_fmt.sample_rate = pattern[i].dstfreq;
		// src fmt
		srcfmt = track->mixer->track_fmt;
		srcfmt.sample_rate = pattern[i].srcfreq;
		// set
		int r = audio_track_set_format(track, &srcfmt);
		if (r != 0) {
			printf("%s: audio_track_set_format failed: %s\n",
				__func__, strerror(errno));
			exit(1);
		}

		uint64_t count;
		printf("%-8s: ", pattern[i].name);
		fflush(stdout);

		setitimer(ITIMER_REAL, &it, NULL);
		gettimeofday(&start, NULL);
		for (count = 0, signaled = 0; signaled == 0; count++) {
			track->freq.srcbuf.head = 0;
			track->freq.srcbuf.used = frame_per_block(track->mixer,
				&srcfmt);
			track->outbuf.head = 0;
			track->outbuf.used = 0;
			audio_apply_stage(track, &track->freq, true);
		}
		gettimeofday(&end, NULL);
		timersub(&end, &start, &result);

#if defined(__m68k__)
		printf("%d %5.1f times/sec\n", (int)count,
			(double)count/(result.tv_sec + (double)result.tv_usec/1000000));
#else
		printf("%d %5.1f times/msec\n", (int)count,
			(double)count/((uint64_t)result.tv_sec*1000 + result.tv_usec/1000));
#endif
	}

	return 0;
}

int
perf_chmix_mixLR()
{
	struct test_file *f = &files[fileidx];
	audio_track_t *track;
	audio_format2_t srcfmt;
	struct timeval start, end, result;
	struct itimerval it;

	signal(SIGALRM, sigalrm);
	memset(&it, 0, sizeof(it));
	it.it_value.tv_sec = 3;

	f->file = sys_open(AUMODE_PLAY);
	track = f->file->ptrack;
	// dst fmt
	track->mixer->track_fmt.channels = 1;
	// src fmt
	srcfmt = track->mixer->track_fmt;
	srcfmt.channels = 2;
	// set
	int r = audio_track_set_format(track, &srcfmt);
	if (r != 0) {
		printf("%s: audio_track_set_format failed: %s\n",
			__func__, strerror(errno));
		exit(1);
	}

	uint64_t count;
	printf("mixLR: ");
	fflush(stdout);

	setitimer(ITIMER_REAL, &it, NULL);
	gettimeofday(&start, NULL);
	for (count = 0, signaled = 0; signaled == 0; count++) {
		track->chmix.srcbuf.head = 0;
		track->chmix.srcbuf.used = frame_per_block(track->mixer,
			&srcfmt);
		track->outbuf.head = 0;
		track->outbuf.used = 0;
		audio_apply_stage(track, &track->chmix, false);
	}
	gettimeofday(&end, NULL);
	timersub(&end, &start, &result);

#if defined(__m68k__)
	printf("%d %5.1f times/sec\n", (int)count,
		(double)count/(result.tv_sec + (double)result.tv_usec/1000000));
#else
	printf("%d %5.1f times/msec\n", (int)count,
		(double)count/((uint64_t)result.tv_sec*1000 + result.tv_usec/1000));
#endif

	return 0;
}
