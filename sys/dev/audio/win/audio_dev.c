#include <Windows.h>
#include <mmsystem.h>
#pragma comment(lib, "winmm.lib")

// Windows 的型キャストを許容する
#pragma warning(disable: 4312)

#include "aumix.h"
#include "auring.h"
#include <stdio.h>
#include "auintr.h"

//#define DEBUG_ONEBUF
//#define DEBUG_DUMP

#define DEV_BITS 16
typedef int16_t DEV_SAMPLE_T;

struct audio_dev_win32
{
	HWAVEOUT handle;
	uint32_t deviceID;
	WAVEFORMATEX wfx;
#define WAVEHDR_COUNT 4
	WAVEHDR wavehdr[WAVEHDR_COUNT];

	int data_bytelen;			/* wavehdr[].lpData の確保バイト長 */
	int data_framecount;		/* wavehdr[].lpData に書き込める最大フレーム数 */

	CRITICAL_SECTION cs;
};
typedef struct audio_dev_win32 audio_dev_win32_t;

void lock(struct audio_softc *sc)
{
	audio_dev_win32_t *dev = sc->phys;
	EnterCriticalSection(&dev->cs);
}

void unlock(struct audio_softc *sc)
{
	audio_dev_win32_t *dev = sc->phys;
	LeaveCriticalSection(&dev->cs);
}

void CALLBACK audio_dev_win32_callback(
	HWAVEOUT hwo,
	UINT uMsg,
	DWORD dwInstance,
	DWORD dwParam1,
	DWORD dwParam2
)
{
	if (uMsg == WOM_DONE) {
		struct audio_softc *sc = (struct audio_softc*)dwInstance;
		audio_dev_win32_t *dev = sc->phys;
		audio_trackmixer_t *mixer = &sc->sc_pmixer;
		WAVEHDR *wh = (WAVEHDR*)dwParam1;

		lock(sc);
		if (wh->dwBufferLength == 0) {
			panic("wh->dwBufferLentgh == 0");
		}
		KASSERT(wh->dwBufferLength % 1764 == 0);
#ifdef AUDIO_INTR_EMULATED
		struct intr_t x;
		x.code = INTR_TRACKMIXER;
		x.mixer = mixer;
		x.count = wh->dwBufferLength / dev->wfx.nBlockAlign;
		emu_intr(x);
#else
		audio_trackmixer_intr(mixer, wh->dwBufferLength / dev->wfx.nBlockAlign);
#endif
		wh->dwUser = 0;
		unlock(sc);
	}
}

void
audio_attach(struct audio_softc **softc)
{
	struct audio_softc *sc;
	sc = calloc(1, sizeof(struct audio_softc));
	*softc = sc;
	sc->phys = calloc(1, sizeof(audio_dev_win32_t));

	audio_dev_win32_t *dev = sc->phys;
	/* こっちが一次情報 */
	dev->deviceID = WAVE_MAPPER;
	dev->wfx.wFormatTag = WAVE_FORMAT_PCM;
	dev->wfx.nSamplesPerSec = 44100;
	dev->wfx.wBitsPerSample = DEV_BITS;
	dev->wfx.nChannels = 2;
	dev->wfx.nBlockAlign = dev->wfx.nChannels * dev->wfx.wBitsPerSample / 8;
	dev->wfx.nAvgBytesPerSec = dev->wfx.nBlockAlign * dev->wfx.nSamplesPerSec;
	dev->wfx.cbSize = sizeof(WAVEFORMATEX);

	MMRESULT r;
	r = waveOutOpen(
		&dev->handle,
		dev->deviceID,
		&dev->wfx,
#if 0
		0, 0, CALLBACK_NULL);
#else
		(DWORD_PTR)audio_dev_win32_callback,
		(DWORD_PTR)sc,
		CALLBACK_FUNCTION);
#endif
	if (r != MMSYSERR_NOERROR) {
		panic("r != MMSYSERR_NOERROR");
	}

	InitializeCriticalSectionAndSpinCount(&dev->cs, 100);

	dev->data_bytelen = dev->wfx.nAvgBytesPerSec * AUDIO_BLK_MS / 1000;
#ifdef DEBUG_ONEBUF
	dev->data_bytelen = 16 * 1024 * 1024;
#endif
	dev->data_framecount = dev->data_bytelen / dev->wfx.nBlockAlign;

	for (int i = 0; i < WAVEHDR_COUNT; i++) {
		dev->wavehdr[i].lpData = malloc(dev->data_bytelen);
		dev->wavehdr[i].dwBufferLength = dev->data_bytelen;
		dev->wavehdr[i].dwFlags = 0;

		r = waveOutPrepareHeader(dev->handle, &dev->wavehdr[i], sizeof(WAVEHDR));
		if (r != MMSYSERR_NOERROR) {
			panic("");
		}

		/* 一旦 0 においておく */
		dev->wavehdr[i].dwBufferLength = 0;
	}

	audio_mixer_init(&sc->sc_pmixer, sc, AUMODE_PLAY);
	audio_mixer_init(&sc->sc_rmixer, sc, AUMODE_RECORD);
}

void
audio_detach(struct audio_softc *sc)
{
	audio_dev_win32_t *dev = sc->phys;

#ifdef AUDIO_INTR_EMULATED
	audio_file_t *f;
	SLIST_FOREACH(f, &sc->sc_files, entry) {
		audio_track_t *ptrack = &f->ptrack;

		audio_track_play_drain_core(ptrack, true);
	}
	printf("output=%d complete=%d\n", (int)sc->sc_pmixer.hw_output_counter, (int)sc->sc_pmixer.hw_complete_counter);
#endif

	MMRESULT r;
#ifdef DEBUG_ONEBUF
	r = waveOutWrite(dev->handle, &dev->wavehdr[0], sizeof(WAVEHDR));
	if (r != MMSYSERR_NOERROR) {
		panic();
	}
	Sleep((int)dev->wavehdr[0].dwBufferLength / dev->wfx.nBlockAlign * 1000 / dev->wfx.nSamplesPerSec);
#endif

	for (int i = 0; i < WAVEHDR_COUNT; i++) {
		/* もとの値に戻す */
//		dev->wavehdr[i].dwBufferLength = dev->data_bytelen;
		for (;;) {
			r = waveOutUnprepareHeader(dev->handle, &dev->wavehdr[i], sizeof(WAVEHDR));
			if (r == MMSYSERR_NOERROR) break;
			if (r == WAVERR_STILLPLAYING) {
				Sleep(100);
			} else {
				panic("");
			}
		}
	}

	for(;;) {
		r = waveOutClose(dev->handle);
		if (r == MMSYSERR_NOERROR) break;
		if (r == WAVERR_STILLPLAYING) {
			Sleep(100);
		} else {
			panic("");
		}
	}
	DeleteCriticalSection(&dev->cs);
	for (int i = 0; i < WAVEHDR_COUNT; i++) {
		free(dev->wavehdr[i].lpData);
	}
}


/*
* ***** audio_sc *****
*/
static void audio_softc_play_start_core(struct audio_softc *sc);

void
audio_softc_play_start(struct audio_softc *sc)
{
	audio_dev_win32_t *dev = sc->phys;
	audio_trackmixer_t *mixer = &sc->sc_pmixer;

	lock(sc);
	audio_softc_play_start_core(sc);
	unlock(sc);
}

static void audio_softc_play_start_core(struct audio_softc *sc)
{
	audio_dev_win32_t *dev = sc->phys;
	audio_trackmixer_t *mixer = &sc->sc_pmixer;

	WAVEHDR* wh = NULL;
	for (int i = 0; i < WAVEHDR_COUNT; i++) {
		if ((dev->wavehdr[i].dwFlags & WHDR_INQUEUE) == 0 && !dev->wavehdr[i].dwUser) {
			wh = &dev->wavehdr[i];
			break;
		}
	}
	if (wh == NULL) return;

	uint8_t *dst = (uint8_t*)wh->lpData;
#ifdef DEBUG_ONEBUF
	dst += wh->dwBufferLength;
	int wh_free_count = dev->data_framecount - (int)wh->dwBufferLength / dev->wfx.nBlockAlign;
#else
	wh->dwBufferLength = 0;
	int wh_free_count = dev->data_framecount;
#endif

	if (mixer->hwbuf.count <= 0) {
		return;
	}

	int count;

	for (int loop = 0; loop < 2; loop++) {
		count = audio_ring_unround_count(&mixer->hwbuf);
		count = min(count, wh_free_count);

		DEV_SAMPLE_T *src = RING_TOP(DEV_SAMPLE_T, &mixer->hwbuf);
		
		int bytelen = count * dev->wfx.nBlockAlign;
		memcpy(dst, src, bytelen);

		dst += bytelen;
		wh->dwBufferLength += bytelen;
		wh_free_count -= count;

		audio_ring_tookfromtop(&mixer->hwbuf, count);
	}

	KASSERT(wh->dwBufferLength % 1764 == 0);
#ifdef DEBUG_ONEBUF
#else
	wh->dwUser = 1;

	MMRESULT r;
	// わざと \n いれてない
	printf("WOUT");
	r = waveOutWrite(dev->handle, wh, sizeof(WAVEHDR));
	if (r != MMSYSERR_NOERROR) {
		panic("");
	}
#endif

#ifdef DEBUG_DUMP
	FILE *fp = fopen("x:\\1.csv", "a+");
	DEV_SAMPLE_T *src = wh->lpData;
	count = wh->dwBufferLength / (dev->wfx.wBitsPerSample * dev->wfx.nChannels / 8);
	for (int i = 0; i < count; i++) {
		for (int ch = 0; ch < mixer->hw_fmt.channels; ch++) {
			fprintf(fp, "%d,", *src++);
		}
		fprintf(fp, "\n");
	}
	fclose(fp);
#endif
}

bool
audio_softc_play_busy(struct audio_softc *sc)
{
	audio_dev_win32_t *dev = sc->phys;
	int busy_buf_count = 0;
	for (int i = 0; i < WAVEHDR_COUNT; i++) {
		busy_buf_count += (dev->wavehdr[i].dwFlags & WHDR_INQUEUE) ? 1 : 0;
	}
	return busy_buf_count >= WAVEHDR_COUNT;
}

int audio_softc_get_hw_capacity(struct audio_softc *sc)
{
	audio_dev_win32_t *dev = sc->phys;
	return dev->data_framecount * WAVEHDR_COUNT;
}

void* audio_softc_allocm(struct audio_softc *sc, int n)
{
	return malloc(n);
}

audio_format2_t audio_softc_get_hw_format(struct audio_softc *sc, int mode)
{
	audio_dev_win32_t *dev = sc->phys;
	audio_format2_t rv;
	rv.encoding = AUDIO_ENCODING_SLINEAR_LE;
	rv.channels = (uint8_t)dev->wfx.nChannels;
	rv.sample_rate = dev->wfx.nSamplesPerSec;
	rv.precision = (uint8_t)dev->wfx.wBitsPerSample;
	rv.stride = (uint8_t)dev->wfx.wBitsPerSample;
	return rv;
}
