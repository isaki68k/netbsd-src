#ifdef _WIN32
#pragma once
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#endif
#include <ctype.h>
#include <stdio.h>
#include <math.h>
#include "aumix.h"
#include "audev.h"
#include "auring.h"

struct test_file
{
	audio_format_t fmt;
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

int parse_file(struct test_file *, FILE *, const char *, int);
uint16_t lebe16toh(uint16_t);
uint32_t lebe32toh(uint32_t);
uint64_t filesize(FILE *);
const char *audio_encoding_name(int);
const char *tagname(uint32_t);
void play_mml(audio_ring_t *dst, const char *mml);

int debug;
int rifx;

void
usage()
{
	printf("usage: [options or files...]\n");
	printf(" -d        debug\n");
	printf(" -f <freq> set ADPCM frequency\n");
	printf(" -g        (guess) display format only, no playback\n");
	printf(" -m <MML>  play MML\n");
	exit(1);
}

int
main(int ac, char *av[])
{
	audio_softc_t *sc;
	struct test_file files[16];
	int fileidx = 0;
	int freq = 15625;
	int opt_g = 0;		// フォーマットを表示するだけオプション

	if (ac < 2) {
		usage();
	}

	audio_attach(&sc);

	ac -= 1;
	av += 1;

	for (int i = 0; i < ac; i++) {
		const char *mml = NULL;
		if (strcmp(av[i], "-d") == 0) {
			debug++;
			continue;
		}
		if (strcmp(av[i], "-g") == 0) {
			opt_g = 1;
			continue;
		}
		if (strcmp(av[i], "-f") == 0) {
			i++;
			if (i == ac)
				usage();
			freq = atoi(av[i]);
			continue;
		}

		struct test_file *f = &files[fileidx];
		f->mem.fmt = &f->fmt;
		f->mem.top = 0;

		if (strcmp(av[i], "-m") == 0) {
			// -m <MML>
			i++;
			if (i == ac)
				usage();
			mml = av[i];

			f->fmt.frequency = 44100;
			f->fmt.encoding = AUDIO_ENCODING_SLINEAR_HE;
			f->fmt.channels = 1;
			f->fmt.precision = 16;
			f->fmt.stride = 16;

			f->mem.capacity = 0;
			f->mem.count = 0;
			f->mem.sample = NULL;
			play_mml(&f->mem, mml);
		} else {
			// ファイル名なら音声ファイルとして開く
			FILE *fp = fopen(av[i], "rb");
			if (fp == NULL) {
				printf("ERROR: fopen\n");
				return 1;
			}

			// ファイル形式を調べて f に色々格納。
			int len = parse_file(f, fp, av[i], freq);
			fclose(fp);

			printf("%s: %s\n", av[i], fmt_tostring(&f->fmt));
			if (opt_g)
				continue;

			f->mem.capacity = len * 8 / f->fmt.stride;
			f->mem.count = len * 8 / f->fmt.stride;
		}

		f->file = sys_open(sc, AUDIO_PLAY);
		/* この辺は ioctl になる */
		f->file->lane_play.volume = 256;
		f->file->lane_play.mixer->volume = 256;
		for (int j = 0; j < 2; j++) {
			f->file->lane_play.ch_volume[j] = 256;
		}

		audio_lane_set_format(&f->file->lane_play, &f->fmt);

		f->play = true;
		f->wait = i * 5;
		fileidx++;
	}

	for (int loop = 0; ; loop++) {
		for (int i = 0; i < fileidx; i++) {
			struct test_file *f = &files[i];
			if (f->wait > loop) continue;
			int n = min(f->mem.count, 625*8);
			if (n == 0) {
				f->play = false;
				audio_lane_play_drain(&f->file->lane_play);
			} else {
				//printf("%d %d ", i, n);
				int len = n * f->fmt.channels * f->fmt.stride / 8;
				sys_write(f->file, RING_TOP_UINT8(&f->mem), len);
				audio_ring_tookfromtop(&f->mem, n);
			}
		}

		bool isPlay = false;
		for (int i = 0; i < fileidx; i++) {
			isPlay |= files[i].play;
		}
		if (isPlay == false) break;
	}

	for (int i = 0; i < fileidx; i++) {
		struct test_file *f = &files[i];
		audio_lane_play_drain(&f->file->lane_play);
	}

	//Sleep(1000);

	audio_detach(sc);

	printf("END\n");
#ifdef _WIN32
	getchar();
#endif
	return 0;

}

#define GETID(ptr) (be32toh(*(uint32_t *)(ptr)))
#define ID(str)	((str[0]<<24) | (str[1]<<16) | (str[2]<<8) | str[3])
static inline int IDC(uint32_t value) {
	value &= 0xff;
	return isprint(value) ? value : '?';
}

// ファイル形式を調べて f->fmt にパラメータをセット、
// ファイル(データ)本体を f->mem.sample にセットする。
// 戻り値はデータ本体の長さ。
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
				printf("tag=%s len=%d", tagname(tag), len);
			if (tag == ID("fmt ")) {
				WAVEFORMATEX *wf = (WAVEFORMATEX *)s;
				s += len;
				if (rifx)
					f->fmt.encoding = AUDIO_ENCODING_SLINEAR_BE;
				else
					f->fmt.encoding = AUDIO_ENCODING_SLINEAR_LE;
				f->fmt.channels = (uint8_t)lebe16toh(wf->nChannels);
				f->fmt.frequency = lebe32toh(wf->nSamplesPerSec);
				f->fmt.precision = (uint8_t)lebe16toh(wf->wBitsPerSample);
				f->fmt.stride = f->fmt.precision;
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
			f->fmt.encoding = AUDIO_ENCODING_MULAW;
			f->fmt.precision = 8;
			break;
		 case SND_FORMAT_LINEAR_8:
			f->fmt.encoding = AUDIO_ENCODING_SLINEAR_BE;
			f->fmt.precision = 8;
			break;
		 case SND_FORMAT_LINEAR_16:
			f->fmt.encoding = AUDIO_ENCODING_SLINEAR_BE;
			f->fmt.precision = 16;
			break;
		 case SND_FORMAT_LINEAR_24:
			f->fmt.encoding = AUDIO_ENCODING_SLINEAR_BE;
			f->fmt.precision = 24;
			break;
		 case SND_FORMAT_LINEAR_32:
			f->fmt.encoding = AUDIO_ENCODING_SLINEAR_BE;
			f->fmt.precision = 32;
			break;
		 default:
			printf("%s: Unknown format: format=%d\n", filename, format);
			exit(1);
		}
		f->fmt.channels = be32toh(au->channels);
		f->fmt.frequency = be32toh(au->sample_rate);
		f->fmt.stride = f->fmt.precision;
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
	f->fmt.encoding = AUDIO_ENCODING_MSM6258;
	f->fmt.channels = 1;
	f->fmt.precision = 4;
	f->fmt.stride = 4;
	f->fmt.frequency = adpcm_freq;

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
fmt_tostring(audio_format_t *fmt)
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
		fmt->frequency);
	return buf;
}

// AUDIO_ENCODING_* に対応する表示名を返す
const char *
audio_encoding_name(int enc)
{
	static char buf[16];

	if (enc == AUDIO_ENCODING_MULAW)
		return "MULAW";
	if (enc == AUDIO_ENCODING_SLINEAR_LE)
		return "SLINEAR_LE";
	if (enc == AUDIO_ENCODING_SLINEAR_BE)
		return "SLINEAR_BE";
	if (enc == AUDIO_ENCODING_ULINEAR_LE)
		return "ULINEAR_LE";
	if (enc == AUDIO_ENCODING_ULINEAR_BE)
		return "ULINEAR_BE";
	if (enc == AUDIO_ENCODING_MSM6258)
		return "MSM6258";
	sprintf(buf, "enc=%d", enc);
	return buf;
}

const char *
tagname(uint32_t tag)
{
	static char buf[32];

	snprintf(buf, sizeof(buf), "%x '%c%c%c%c' ",
		tag,
		IDC(tag>>24),
		IDC(tag>>16),
		IDC(tag>>8),
		IDC(tag));
	return buf;
}

