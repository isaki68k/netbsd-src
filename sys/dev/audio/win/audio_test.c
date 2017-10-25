#ifdef _WIN32
#pragma once
#include <windows.h>
#endif

//#define PSG_TEST
//#define WAV_API_TEST
#define ADPCM_TEST

#ifdef ADPCM_TEST

#include <stdio.h>
#include <math.h>
#include "aumix.h"
#include "audev.h"
#include "auring.h"

void ring_expand(audio_ring_t *ring, int newcapacity);

struct test_file
{
	audio_format_t fmt;
	audio_file_t *file;
	audio_ring_t mem;
	bool play;
	int wait;
};

int
main(int ac, char *av[])
{
	if (ac < 2) {
		printf("arg: adpcm file\n");
		return 0;
	}

	audio_softc_t *sc;
	audio_attach(&sc);

	struct test_file files[16];
	ac -= 1;
	av += 1;
	int fileidx = 0;
	int freq = 15625;

	for (int i = 0; i < ac; i++) {
		if (strcmp(av[i], "-f") == 0) {
			i++;
			if (i == ac) return 1;
			freq = atoi(av[i]);
			continue;
		}

		printf("%s\n", av[i]);
		FILE *fp = fopen(av[i], "rb");
		if (fp == NULL) {
			printf("ERROR: fopen\n");
			return 1;
		}

		struct test_file *f = &files[fileidx];

		f->fmt.frequency = freq;
#if 0
		f->fmt.encoding = AUDIO_ENCODING_MSM6258;
		f->fmt.channels = 1;
		f->fmt.precision = 4;
		f->fmt.stride = 4;
#else
		f->fmt.encoding = AUDIO_ENCODING_SLINEAR_LE;
		f->fmt.channels = 1;
		f->fmt.precision = 16;
		f->fmt.stride = 16;
#endif

		f->mem.fmt = &f->fmt;
		f->mem.capacity = 0;
		f->mem.top = 0;
		f->mem.count = 0;
		f->mem.sample = NULL;

		f->file = audio_file_open(sc, AUDIO_PLAY);
		/* この辺は ioctl になる */
		f->file->lane_play.volume = 256;
		f->file->lane_play.mixer->volume = 256;
		for (int j = 0; j < 2; j++) {
			f->file->lane_play.ch_volume[j] = 256;
		}

		audio_lane_set_format(&f->file->lane_play, &f->fmt);

		f->mem.sample = malloc(4096);
		int len = 0;
		for (;;) {
			uint8_t *dptr = (uint8_t*)f->mem.sample + len;
			int n = fread(dptr, 1, 4096, fp);
			if (n == 0) break;
			len += n;
			f->mem.sample = realloc(f->mem.sample, len + 4096);
		}
		fclose(fp);

		f->mem.capacity = len * 8 / f->fmt.stride;
		f->mem.count = len * 8 / f->fmt.stride;

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
				audio_file_write(f->file, RING_TOP_UINT8(&f->mem), len);
				audio_ring_tookfromtop(&f->mem, n);
			}
		}

		bool isPlay = false;
		for (int i = 0; i < ac; i++) {
			isPlay |= files[i].play;
		}
		if (isPlay == false) break;
	}

	for (int i = 0; i < ac; i++) {
		struct test_file *f = &files[i];
		audio_lane_play_drain(&f->file->lane_play);
	}

	//Sleep(1000);

	audio_detach(sc);

	printf("END\n");
	getchar();
	return 0;

}

#endif


#ifdef PSG_TEST

#include <math.h>
#include "aumix.h"
#include "audev.h"
#include <stdio.h>
#include "auring.h"

void play_mml(audio_ring_t *dst, char *mml);


double freqs[] = { 523.251, 587.330, 659.255, 698.456, 783.991, 880.000, 987.767, };

void gen_sin(int16_t *dst, int count, double freq)
{
	int vol = 32767;
	for (int i = 0; i < count; i++) {
		dst[i] = (int16_t)(sin((double)i * 3.14159 * 2 * freq / 44100) * vol);
//dst[i] = freq;
		if (i > (int)((double)count * 0.9)) {
			if (vol > 0) vol/=2;
		}
	}
}

int gen_MML(int16_t *dst, int count, char *mml)
{
	int note_len = 44100 / 2;
	int oct = 5;
	int16_t *end = dst + count;
	int output = 0;

	for (; *mml != 0; mml++) {
		char c = *mml;
		int fi = -1;
		if (c == 'L') {
			mml++;
			char *ep;
			long num = strtol(mml, &ep, 10);
			if (num <= 0) break;
			mml = ep - 1;
			note_len = 44100 / num;
		}
		if (c == 'O') {
			mml++;
			char *ep;
			long num = strtol(mml, &ep, 10);
			if (num <= 0) break;
			mml = ep - 1;
			oct = num;
		}
		if (c == 'R') {
			output += note_len;
			dst += note_len;
		}
		if ('C' <= c && c <= 'G') {
			fi = c - 'C';
		} else if ('A' <= c && c <= 'B') {
			fi = c - 'A' + 5;
		}

		if (output + note_len > count) break;
		if (fi >= 0) {
			gen_sin(dst, note_len, freqs[fi] * pow(2, oct - 5));
			output += note_len;
			dst += note_len;
		}
	}
	return output;
}

struct test_file
{
	audio_format_t fmt;
	audio_file_t *file;
	audio_ring_t mem;
	bool play;
	int wait;
};

int
main(void)
{
	audio_softc_t *sc;

	audio_attach(&sc);

#define N 2
	struct test_file files[N];

	for (int i = 0; i < N; i++) {
		struct test_file *f = &files[i];

		f->fmt.frequency = 44100;
		f->fmt.encoding = AUDIO_ENCODING_SLINEAR_HE;
		f->fmt.channels = 1;
		f->fmt.precision = 16;
		f->fmt.stride = 16;

		f->mem.fmt = &f->fmt;
		f->mem.capacity = 0;
		f->mem.top = 0;
		f->mem.count = 0;
		f->mem.sample = NULL;

		f->file = audio_file_open(sc, AUDIO_PLAY);
		/* この辺は ioctl になる */
		f->file->lane_play.volume = 256;
		f->file->lane_play.mixer->volume = 256;
		for (int j = 0; j < 2; j++) {
			f->file->lane_play.ch_volume[j] = 256;
		}

		audio_lane_set_format(&f->file->lane_play, &f->fmt);

		char* MML[3] = {
			"T30O4A1",//"V128T100R60R60T120L4RO4CDEFEDCREFGAGFERCRCRCRCRL8CCDDEEFFL4EDC",
			"R",//"V128T6000R1T120L4RO4RRRRRRRRCDEFEDCREFGAGFERCRCRCRCRL8CCDDEEFFL4EDC",
			"R",//"V128T120L4RO4RRRRRRRRRRRRRRRRCDEFEDCREFGAGFERCRCRCRCRL8CCDDEEFFL4EDC",
		};
		play_mml(&f->mem, MML[i]);
		f->play = true;
		f->wait = i;
	}

	for (int loop = 0; ; loop++) {
		for (int i = 0; i < N; i++) {
			struct test_file *f = &files[i];
			//if (loop % (f->wait + 1) != 0) continue;
			int n = min(f->mem.count, 4000);
			if (n == 0) {
				f->play = false;
				audio_lane_play_drain(&f->file->lane_play);
			} else {
				//printf("%d %d ", i, n);
				int len = n * f->fmt.channels * f->fmt.stride / 8;
				audio_file_write(f->file, RING_TOP(int16_t, &f->mem), len);
				audio_ring_tookfromtop(&f->mem, n);
			}
		}

		bool isPlay = false;
		for (int i = 0; i < N; i++) {
			isPlay |= files[i].play;
		}
		if (isPlay == false) break;
	}

	for (int i = 0; i < N; i++) {
		struct test_file *f = &files[i];
		audio_lane_play_drain(&f->file->lane_play);
	}

	//Sleep(1000);

	audio_detach(sc);

	printf("END\n");
	getchar();
	return 0;
}
#endif

#ifdef WAV_API_TEST

#include <windows.h>
#include <mmsystem.h>
#pragma comment(lib, "winmm.lib")

int main()
{
	HWAVEOUT hWaveOut = 0;
	WAVEFORMATEX wfx = { WAVE_FORMAT_PCM, 1, 8000, 8000, 1, 8, 0 };
	waveOutOpen(&hWaveOut, WAVE_MAPPER, &wfx, 0, 0, CALLBACK_NULL);
	char buffer[8000 * 60] = { 0, };

	// See http://goo.gl/hQdTi
	for (DWORD t = 0; t < sizeof(buffer); ++t)
		buffer[t] = (char)((((t * (t >> 8 | t >> 9) & 46 & t >> 8)) ^ (t & t >> 13 | t >> 6)) & 0xFF);

	WAVEHDR header = { buffer, sizeof(buffer), 0, 0, 0, 0, 0, 0 };
	waveOutPrepareHeader(hWaveOut, &header, sizeof(WAVEHDR));
	waveOutWrite(hWaveOut, &header, sizeof(WAVEHDR));
	waveOutUnprepareHeader(hWaveOut, &header, sizeof(WAVEHDR));
	waveOutClose(hWaveOut);
	Sleep(60 * 1000);
}
#endif
