#ifdef _WIN32
#pragma once
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#endif
#include <ctype.h>
#include <stdio.h>
#include <inttypes.h>
#include <math.h>
#include <signal.h>
#include <sys/time.h>
#include "aumix.c"
#ifdef USE_PTHREAD
#include <pthread.h>
#endif

struct test_file
{
	audio_file_t *file;
	audio_ring_t mem;
	bool play;
	int wait;
#ifdef USE_PTHREAD
	pthread_t tid;
#endif
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
	CMD_MML,
	CMD_TEST,
	CMD_PERF,
};

struct freqdata {
	int srcfreq;
	int dstfreq;
	const char *name;
};

int child_loop(struct test_file *, int);
#ifdef USE_PTHREAD
void *child(void *);
#endif
int cmd_print_file(const char *);
int cmd_set_file(const char *);
int cmd_set_mml(const char *);
int cmd_play();
int cmd_perf(const char *);
int cmd_perf_freq();
int cmd_perf_freq_up();
int cmd_perf_freq_down();
int cmd_perf_freq_main(struct freqdata *);
int parse_file(struct test_file *, FILE *, const char *, int);
uint16_t lebe16toh(uint16_t);
uint32_t lebe32toh(uint32_t);
const char *fmt_tostring(const audio_format2_t *);
uint64_t filesize(FILE *);
const char *audio_encoding_name(int);
const char *tagname(uint32_t);
void play_mml(audio_ring_t *dst, const char *mml);

struct audio_softc *sc;
struct test_file files[16];
int debug;
int rifx;
int fileidx;
int freq;
int opt_wait;	// 1ファイルごとの開始ディレイ
int opt_vol;
#if !defined(_WIN32)
const char *devicefile;
#endif

void
usage()
{
	printf("usage: [options] <cmd> [files...]\n");
	printf("options:\n");
#if !defined(_WIN32)
	printf(" -D <dev>  device name (default: /dev/sound)\n");
#endif
	printf(" -d        debug\n");
	printf(" -f <freq> set ADPCM frequency\n");
	printf(" -w <cnt>  delay block count for each files\n");
	printf(" -v <vol>  track volume (0..256)\n");
	printf("cmd:\n");
	printf(" file <files...>  print format (without playback)\n");
	printf(" play <files...>  play files\n");
	printf(" mml  <mml>       play mml\n");
	printf(" perf <testname>  performance test\n");
	exit(1);
}

int
main(int ac, char *av[])
{
	int i;
	int cmd;

	if (ac < 2) {
		usage();
	}

	ac -= 1;
	av += 1;

	freq = 15625;
	opt_vol = 256;
#if !defined(_WIN32)
	devicefile = "/dev/sound";
#endif

	// 先にオプション
	for (i = 0; i < ac; i++) {
		const char *mml = NULL;
#if !defined(_WIN32)
		if (strcmp(av[i], "-D") == 0) {
			i++;
			if (i == ac)
				usage();
			devicefile = av[i];
			continue;
		}
#endif
		if (strcmp(av[i], "-d") == 0) {
			debug++;
			continue;
		}
		if (strcmp(av[i], "-f") == 0) {
			i++;
			if (i == ac)
				usage();
			freq = atoi(av[i]);
			continue;
		}
		if (strcmp(av[i], "-w") == 0) {
			i++;
			if (i == ac)
				usage();
			opt_wait = atoi(av[i]);
			continue;
		}
		if (strcmp(av[i], "-v") == 0) {
			i++;
			if (i == ac)
				usage();
			opt_vol = atoi(av[i]);
			continue;
		}
		break;
	}

	// コマンド名
	if (i == ac)
		usage();
	if (strcmp(av[i], "file") == 0) {
		cmd = CMD_FILE;
	} else if (strcmp(av[i], "play") == 0) {
		cmd = CMD_PLAY;
	} else if (strcmp(av[i], "mml") == 0) {
		cmd = CMD_MML;
	} else if (strcmp(av[i], "perf") == 0) {
		cmd = CMD_PERF;
	} else {
		usage();
	}
	i++;

	// コマンドごとにその後の引数
	int r = 0;
	switch (cmd) {
	 case CMD_FILE:
		for (; i < ac; i++) {
			r = cmd_print_file(av[i]);
			if (r != 0)
				break;
		}
		break;
	 case CMD_PLAY:
		audio_attach(&sc, true);
		for (; i < ac; i++) {
			r = cmd_set_file(av[i]);
			if (r != 0)
				break;
		}
		r = cmd_play();
		audio_detach(sc);
		break;
	 case CMD_MML:
		audio_attach(&sc, true);
		r = cmd_set_mml(av[i]);
		r = cmd_play();
		audio_detach(sc);
		break;

	 case CMD_PERF:
		audio_attach(&sc, false);
		for (; i < ac; i++) {
			r = cmd_perf(av[i]);
			if (r != 0)
				break;
		}
		audio_detach(sc);
		break;
	}

#ifdef _WIN32
	printf("END\n");
	getchar();
#endif
	return r;
}

// filename のヘッダとか内容を表示します。
int
cmd_print_file(const char *filename)
{
	struct test_file *f = &files[fileidx];

	// ファイル名なら音声ファイルとして開く
	FILE *fp = fopen(filename, "rb");
	if (fp == NULL) {
		printf("ERROR: fopen: %s\n", filename);
		return 1;
	}

	// ファイル形式を調べて f に色々格納。
	int len = parse_file(f, fp, filename, freq);
	fclose(fp);

	printf("%s: %s\n", filename, fmt_tostring(&f->mem.fmt));
	return 0;
}

// MML を再生します。
int
cmd_set_mml(const char *mml)
{
	struct test_file *f = &files[fileidx];

	f->mem.top = 0;
	f->mem.fmt.sample_rate = 44100;
	f->mem.fmt.encoding = AUDIO_ENCODING_SLINEAR_NE;
	f->mem.fmt.channels = 1;
	f->mem.fmt.precision = 16;
	f->mem.fmt.stride = 16;

	f->mem.capacity = 0;
	f->mem.count = 0;
	f->mem.sample = NULL;
	play_mml(&f->mem, mml);
	return 0;
}

// filename を再生用にセットします。
int
cmd_set_file(const char *filename)
{
	struct test_file *f = &files[fileidx];
	f->mem.top = 0;

	// ファイル名なら音声ファイルとして開く
	FILE *fp = fopen(filename, "rb");
	if (fp == NULL) {
		printf("ERROR: fopen: %s\n", filename);
		return 1;
	}

	// ファイル形式を調べて f に色々格納。
	int len = parse_file(f, fp, filename, freq);
	fclose(fp);

	printf("%s: %s\n", filename, fmt_tostring(&f->mem.fmt));

	if (f->mem.fmt.encoding == AUDIO_ENCODING_ADPCM) {
		// MSM6258 -> SLINEAR16(internal format)
		uint8_t *tmp = malloc(len * 4);
		audio_format2_t dstfmt;
		dstfmt.encoding = AUDIO_ENCODING_SLINEAR_NE;
		dstfmt.sample_rate = f->mem.fmt.sample_rate;
		dstfmt.precision = AUDIO_INTERNAL_BITS;
		dstfmt.stride = AUDIO_INTERNAL_BITS;
		dstfmt.channels = f->mem.fmt.channels;
		audio_filter_arg_t arg;
		arg.context = msm6258_context_create();
		arg.src = f->mem.sample;
		arg.srcfmt = &f->mem.fmt;
		arg.dst = tmp;
		arg.dstfmt = &dstfmt;
		arg.count = len * 2;
		msm6258_to_internal(&arg);
		msm6258_context_destroy(arg.context);
		// かきかえ
		free(f->mem.sample);
		f->mem.sample = tmp;
		f->mem.fmt = dstfmt;
		f->mem.capacity = len * 2;
	} else {
		f->mem.capacity = len * 8 / f->mem.fmt.stride / f->mem.fmt.channels;
	}
	f->mem.count = f->mem.capacity;

	f->file = sys_open(sc, AUMODE_PLAY);
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
#ifdef USE_PTHREAD
	for (int i = 0; i < fileidx; i++) {
		struct test_file *f = &files[i];
		int r = pthread_create(&f->tid, NULL, child, f);
		if (r == -1) {
			printf("pthread_create\n");
			exit(1);
		}
	}

	for (int i = 0; i < fileidx; i++) {
		struct test_file *f = &files[i];
		pthread_join(f->tid, NULL);
	}
#else
	for (int loop = 0; ; loop++) {
		emu_intr_check();

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
#endif

	for (int i = 0; i < fileidx; i++) {
		struct test_file *f = &files[i];
		printf("file %d: hw=%" PRIu64 " hw_comp=%" PRIu64 "\n",
			i,
			f->file->ptrack->mixer_hw_counter,
			f->file->ptrack->hw_complete_counter);
	}
	printf("mixer: hw_out=%" PRIu64 "\n",
		files[0].file->ptrack->mixer->hw_output_counter);

	return 0;

}

// ループが終わるときは -1
int
child_loop(struct test_file *f, int loop)
{
	if (f->wait > loop) return 0;

	// 1ブロック分のフレーム数
	int frames_per_block = frame_per_block_roundup(f->file->ptrack->mixer, &f->mem.fmt);
	// 今回再生するフレーム数
	int frames = min(f->mem.count, frames_per_block);
	// フレーム数をバイト数に
	int bytes = frames * f->mem.fmt.channels * f->mem.fmt.stride / 8;

	sys_write(f->file, RING_TOP_UINT8(&f->mem), bytes);
	audio_ring_tookfromtop(&f->mem, frames);

	if (frames < frames_per_block) {
		// 最後
		f->play = false;
		return -1;
	}
	return 0;
}

#ifdef USE_PTHREAD
void *
child(void *arg)
{
	struct test_file *f = arg;

	for (int loop = 0; ; loop++) {
		if (child_loop(f, loop) == -1)
			break;
	}
	return NULL;
}
#endif

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
parse_file(struct test_file *f, FILE *fp, const char *filename, int adpcm_freq)
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

		f->mem.sample = malloc(len);
		if (f->mem.sample == NULL) {
			printf("%s: malloc failed: %d\n", filename, len);
			exit(1);
		}
		// 読み込み開始位置は s
		fseek(fp, (long)(s - hdrbuf), SEEK_SET);
		fread(f->mem.sample, 1, len, fp);
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

		f->mem.sample = malloc(len);
		if (f->mem.sample == NULL) {
			printf("%s: malloc failed: %d\n", filename, len);
			exit(1);
		}
		// 読み込み開始位置は offset
		fseek(fp, offset, SEEK_SET);
		fread(f->mem.sample, 1, len, fp);
		return len;
	}

	// ADPCM
	f->mem.fmt.encoding = AUDIO_ENCODING_ADPCM;
	f->mem.fmt.channels = 1;
	f->mem.fmt.precision = 4;
	f->mem.fmt.stride = 4;
	f->mem.fmt.sample_rate = adpcm_freq;

	len = (int)filesize(fp);
	f->mem.sample = malloc(len);
	if (f->mem.sample == NULL) {
		printf("%s: malloc failed: %d\n", filename, len);
		exit(1);
	}
	fread(f->mem.sample, 1, len, fp);
	return len;
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

// AUDIO_ENCODING_* に対応する表示名を返す
const char *
audio_encoding_name(int enc)
{
	static char buf[16];

	if (enc == AUDIO_ENCODING_ULAW)
		return "ULAW";
	if (enc == AUDIO_ENCODING_SLINEAR_LE)
		return "SLINEAR_LE";
	if (enc == AUDIO_ENCODING_SLINEAR_BE)
		return "SLINEAR_BE";
	if (enc == AUDIO_ENCODING_ULINEAR_LE)
		return "ULINEAR_LE";
	if (enc == AUDIO_ENCODING_ULINEAR_BE)
		return "ULINEAR_BE";
	if (enc == AUDIO_ENCODING_ADPCM)
		return "MSM6258";
	sprintf(buf, "enc=%d", enc);
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

// テストパターン
struct testdata {
	const char *testname;
	int (*funcname)();
} perfdata[] = {
	{ "freq",		cmd_perf_freq },
	{ "freq_up",	cmd_perf_freq_up },
	{ "freq_down",	cmd_perf_freq_down },
	{ NULL, NULL },
};

int
cmd_perf(const char *testname)
{
	for (int i = 0; perfdata[i].testname != NULL; i++) {
		if (strcmp(perfdata[i].testname, testname) == 0) {
			(perfdata[i].funcname)();
			return 0;
		}
	}

	// 一覧を表示しとくか
	for (int i = 0; perfdata[i].testname != NULL; i++) {
		printf(" %s", perfdata[i].testname);
	}
	printf("\n");
	return 1;
}

int
cmd_perf_freq()
{
	cmd_perf_freq_up();
	cmd_perf_freq_down();
	return 0;
}

int
cmd_perf_freq_up()
{
	struct freqdata pattern[] = {
		{ 44100, 48000,	"44.1->48" },
		{ 8000,  48000, "8->48" },
		{ -1, -1, NULL },
	};
	return cmd_perf_freq_main(pattern);
}

int
cmd_perf_freq_down()
{
	struct freqdata pattern[] = {
		{ 48000, 44100,	"48->44.1" },
		{ 48000, 8000,	"48->8" },
		{ -1, -1, NULL },
	};
	return cmd_perf_freq_main(pattern);
}

volatile int signaled;

void
sigalrm(int signo)
{
	signaled = 1;
}

int
cmd_perf_freq_main(struct freqdata *pattern)
{
	struct test_file *f = &files[fileidx];
	audio_track_t *track;
	struct timeval start, end, result;
	struct itimerval it;

	signal(SIGALRM, sigalrm);
	memset(&it, 0, sizeof(it));
	it.it_value.tv_sec = 3;

	f->file = sys_open(sc, AUMODE_PLAY);
	track = f->file->ptrack;
	for (int i = 0; pattern[i].name != NULL; i++) {
		track->inputfmt.encoding = AUDIO_ENCODING_SLINEAR_NE;
		track->inputfmt.precision = 16;
		track->inputfmt.stride = 16;
		track->inputfmt.channels = 2;
		track->inputfmt.sample_rate = pattern[i].srcfreq;
		track->input = &track->freq.srcbuf;
		track->outputbuf.fmt = track->inputfmt;
		track->outputbuf.fmt.sample_rate = pattern[i].dstfreq;
		track->outputbuf.top = 0;
		track->outputbuf.count = 0;
		track->outputbuf.capacity = frame_per_block_roundup(track->mixer,
		    &track->outputbuf.fmt);
		track->outputbuf.sample = audio_realloc(track->outputbuf.sample,
		    RING_BYTELEN(&track->outputbuf));
		init_freq(track, &track->outputbuf);

		uint64_t count;
		printf("%-8s: ", pattern[i].name);
		fflush(stdout);

		setitimer(ITIMER_REAL, &it, NULL);
		gettimeofday(&start, NULL);
		for (count = 0, signaled = 0; signaled == 0; count++) {
			track->freq.srcbuf.count = track->inputfmt.sample_rate * 40 /1000;
			track->freq.arg.src = track->freq.srcbuf.sample;
			track->freq.arg.dst = track->outputbuf.sample;
			track->freq.arg.count = track->outputbuf.capacity;

			track->freq.srcbuf.top = 0;
			track->outputbuf.top = 0;
			track->outputbuf.count = 0;
			track->freq.filter(&track->freq.arg);
			//printf("dst count = %d\n", track->outputbuf.count);
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
