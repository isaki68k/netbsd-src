// linear-to-mulaw の動作確認を兼ねて
// 実際に .wav を .au に変換してみるテストプログラム

#define AUDIO2

#include <err.h>
#include <stdio.h>
#include <stdlib.h>
#define MULAW_HQ_ENC
#include "mulaw.c"

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

struct test_file {
	int channels;
	int sample_rate;
	int precision;
	int stride;
	int encoding;
	uint8_t *mem;
	int len;
};

int parse_file(struct test_file *, FILE *, const char *);

int audio_mulaw_hq_enc = 1;
int panic_msgout;

int
main(int ac, char *av[])
{
	const char *infile;
	const char *outfile;
	FILE *fpin;
	FILE *fpout;
	struct test_file f;

	if (ac < 3) {
		errx(1, "%s input.wav output.au", getprogname());
	}
	infile = av[1];
	outfile = av[2];

	fpin = fopen(infile, "r");
	if (fpin == NULL) {
		err(1, "fopen: %s", infile);
	}

	fpout = fopen(outfile, "w");
	if (fpout == NULL) {
		err(1, "fopen: %s", outfile);
	}

	if (parse_file(&f, fpin, infile)) {
		errx(1, "parse_file");
	}
	fclose(fpin);

	if (f.channels > 1) {
		errx(1, "channels > 1 not supported");
	}

	fpout = fopen(outfile, "w");
	if (fpout == NULL) {
		err(1, "fopen: %s", outfile);
	}
	AUFileHeader au;
	au.magic = htobe32(0x2e736e64); // ".snd"
	au.offset = htobe32(sizeof(au));
	au.length = htobe32(f.len / (f.stride / NBBY));
	au.format = htobe32(SND_FORMAT_MULAW_8);
	au.sample_rate = htobe32(f.sample_rate);
	au.channels = htobe32(f.channels);
	fwrite(&au, sizeof(au), 1, fpout);

	audio_format2_t srcfmt;
	audio_format2_t dstfmt;
	srcfmt.encoding = f.encoding;
	srcfmt.precision = f.precision;
	srcfmt.stride = f.stride;
	srcfmt.channels = f.channels;
	srcfmt.sample_rate = f.sample_rate;
	dstfmt.encoding = AUDIO_ENCODING_ULAW;
	dstfmt.precision = 8;
	dstfmt.stride = 8;
	dstfmt.channels = f.channels;
	dstfmt.sample_rate = f.sample_rate;
	int count = 400;
	char dst[count];
	audio_filter_arg_t arg;
	arg.srcfmt = &srcfmt;
	arg.dstfmt = &dstfmt;
	arg.dst = dst;
	for (; f.len > 0;) {
		int srcbytes = count * arg.srcfmt->stride / NBBY;
		if (f.len < srcbytes) {
			srcbytes = f.len;
			count = srcbytes / (arg.srcfmt->stride / NBBY);
		}
		arg.src = f.mem;
		arg.count = count;
		audio_internal_to_mulaw(&arg);
		f.mem += srcbytes;
		f.len -= srcbytes;

		fwrite(dst, count, 1, fpout);
	}

	fclose(fpout);

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

int rifx;

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
			if (tag == ID("fmt ")) {
				WAVEFORMATEX *wf = (WAVEFORMATEX *)s;
				s += len;
				f->channels = (uint8_t)lebe16toh(wf->nChannels);
				f->sample_rate = lebe32toh(wf->nSamplesPerSec);
				f->precision = (uint8_t)lebe16toh(wf->wBitsPerSample);
				f->stride = f->precision;
				if (rifx) {
					f->encoding = AUDIO_ENCODING_SLINEAR_BE;
				} else {
					if (f->precision == 8)
						f->encoding = AUDIO_ENCODING_ULINEAR_LE;
					else
						f->encoding = AUDIO_ENCODING_SLINEAR_LE;
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

		f->len = len;
		f->mem = malloc(len);
		if (f->mem == NULL) {
			printf("%s: malloc failed: %d\n", filename, len);
			exit(1);
		}
		// 読み込み開始位置は s
		fseek(fp, (long)(s - hdrbuf), SEEK_SET);
		fread(f->mem, 1, len, fp);
		return 0;
	}

	return -1;
}
