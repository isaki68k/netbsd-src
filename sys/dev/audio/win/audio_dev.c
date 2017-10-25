#include <Windows.h>
#include <mmsystem.h>
#pragma comment(lib, "winmm.lib")

#include "aumix.h"
#include "auring.h"
#include <stdio.h>

//#define DEBUG_ONEBUF

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

void lock(audio_softc_t *sc)
{
	audio_dev_win32_t *dev = sc->phys;
	EnterCriticalSection(&dev->cs);
}

void unlock(audio_softc_t *sc)
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
		audio_softc_t *sc = (audio_softc_t*)dwInstance;
		audio_dev_win32_t *dev = sc->phys;
		audio_lanemixer_t *mixer = &sc->mixer_play;
		WAVEHDR *wh = (WAVEHDR*)dwParam1;

		lock(sc);
		if (wh->dwBufferLength == 0) {
			panic("wh->dwBufferLentgh == 0");
		}
		audio_lanemixer_intr(mixer, wh->dwBufferLength / dev->wfx.nBlockAlign);
		wh->dwUser = 0;
		unlock(sc);
	}
}

void
audio_attach(audio_softc_t **softc)
{
	audio_softc_t *sc;
	sc = calloc(1, sizeof(audio_softc_t));
	*softc = sc;
	sc->phys = calloc(1, sizeof(audio_dev_win32_t));

	audio_dev_win32_t *dev = sc->phys;
	/* こっちが一次情報 */
	dev->deviceID = WAVE_MAPPER;
	dev->wfx.wFormatTag = WAVE_FORMAT_PCM;
	dev->wfx.nSamplesPerSec = 44100;
	dev->wfx.wBitsPerSample = 16;
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

	dev->data_bytelen = dev->wfx.nAvgBytesPerSec * AUDIO_BLOCK_msec / 1000;
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

	audio_mixer_init(&sc->mixer_play, sc, AUDIO_PLAY);
	audio_mixer_init(&sc->mixer_rec, sc, AUDIO_REC);
}

void
audio_detach(audio_softc_t *sc)
{
	audio_dev_win32_t *dev = sc->phys;

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
void
audio_softc_play_start(audio_softc_t *sc)
{
	audio_dev_win32_t *dev = sc->phys;
	audio_lanemixer_t *mixer = &sc->mixer_play;

	lock(sc);

	WAVEHDR* wh = NULL;
	for (int i = 0; i < WAVEHDR_COUNT; i++) {
		if ((dev->wavehdr[i].dwFlags & WHDR_INQUEUE) == 0 && !dev->wavehdr[i].dwUser) {
			wh = &dev->wavehdr[i];
			break;
		}
	}
	unlock(sc);
	if (wh == NULL) return;

	uint8_t *dst = (uint8_t*)wh->lpData;
#ifdef DEBUG_ONEBUF
	dst += wh->dwBufferLength;
	int wh_free_count = dev->data_framecount - (int)wh->dwBufferLength / dev->wfx.nBlockAlign;
#else
	wh->dwBufferLength = 0;
	int wh_free_count = dev->data_framecount;
#endif

	if (mixer->hw_buf.count <= 0) {
		return;
	}

	int count;

	for (int loop = 0; loop < 2; loop++) {
		count = audio_ring_unround_count(&mixer->hw_buf);
		count = min(count, wh_free_count);

		int16_t *src = RING_TOP(int16_t, &mixer->hw_buf);
		
		int bytelen = count * dev->wfx.nBlockAlign;
		memcpy(dst, src, bytelen);

		dst += bytelen;
		wh->dwBufferLength += bytelen;
		wh_free_count -= count;

		audio_ring_tookfromtop(&mixer->hw_buf, count);
	}

#ifdef DEBUG_ONEBUF
#else
	wh->dwUser = 1;

	MMRESULT r;
	r = waveOutWrite(dev->handle, wh, sizeof(WAVEHDR));
	if (r != MMSYSERR_NOERROR) {
		panic("");
	}
#endif

#ifdef DEBUG_DUMP
	for (int i = 0; i < count; i++) {
		for (int ch = 0; ch < mixer->hw_fmt.channels; ch++) {
			printf("%d ", *src++);
		}
		printf("\n");
	}
#endif
}

bool
audio_softc_play_busy(audio_softc_t *sc)
{
	audio_dev_win32_t *dev = sc->phys;
	int busy_buf_count = 0;
	for (int i = 0; i < WAVEHDR_COUNT; i++) {
		busy_buf_count += (dev->wavehdr[i].dwFlags & WHDR_INQUEUE) ? 1 : 0;
	}
	return busy_buf_count > WAVEHDR_COUNT / 2;
}

int audio_softc_get_hw_capacity(audio_softc_t *sc)
{
	audio_dev_win32_t *dev = sc->phys;
	return dev->data_framecount * WAVEHDR_COUNT;
}

void* audio_softc_allocm(audio_softc_t *sc, int n)
{
	return malloc(n);
}

audio_format_t audio_softc_get_hw_format(audio_softc_t *sc, int mode)
{
	audio_dev_win32_t *dev = sc->phys;
	audio_format_t rv;
	rv.encoding = AUDIO_ENCODING_SLINEAR_LE;
	rv.channels = (uint8_t)dev->wfx.nChannels;
	rv.frequency = dev->wfx.nSamplesPerSec;
	rv.precision = (uint8_t)dev->wfx.wBitsPerSample;
	rv.stride = (uint8_t)dev->wfx.wBitsPerSample;
	return rv;
}

/* STUB */
void WAIT() {
	Sleep(1);
}
