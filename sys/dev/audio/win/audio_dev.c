#include <Windows.h>
#include <mmsystem.h>
#pragma comment(lib, "winmm.lib")

// Windows 的型キャストを許容する
#pragma warning(disable: 4312)

#include "aumix.h"
#include "auring.h"
#include <stdio.h>
#include "auintr.h"
#include "compat.h"

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

void *win_allocm(void *hdl, int direction, size_t size)
{
	return malloc(size);
}

void win_freem(void *hdl, void *addr, size_t size)
{
	free(addr);
}

int
win_start_output(void *hdl, void *blk, int blksize, void(*intr)(void *), void *arg)
{
	struct audio_softc *sc = hdl;
	audio_softc_play_start(sc);
	return 0;
}

int
win_halt_output(void *hdl)
{
	return 0;
}

void
win_kick_intr(struct audio_softc *sc, WAVEHDR *wh)
{
	audio_dev_win32_t *dev = sc->phys;
	audio_trackmixer_t *mixer = sc->sc_pmixer;

	// processed
	if (wh->dwUser == 0) return;

	wh->dwUser = 0;
#ifdef AUDIO_INTR_EMULATED
	struct intr_t x;
	x.code = INTR_TRACKMIXER;
	x.sc = sc;
	x.mixer = mixer;
	x.count = wh->dwBufferLength / dev->wfx.nBlockAlign;
	emu_intr(x);
#else
	audio_trackmixer_intr(mixer, wh->dwBufferLength / dev->wfx.nBlockAlign);
#endif
}

void CALLBACK audio_dev_win32_callback(
	HWAVEOUT hwo,
	UINT uMsg,
	DWORD_PTR dwInstance,
	DWORD_PTR dwParam1,
	DWORD_PTR dwParam2
)
{
	if (uMsg == WOM_DONE) {
		struct audio_softc *sc = (struct audio_softc*)dwInstance;
		WAVEHDR *wh = (WAVEHDR*)dwParam1;

		lock(sc);
		if (wh->dwBufferLength == 0) {
			panic("wh->dwBufferLentgh == 0");
		}
		win_kick_intr(sc, wh);
		unlock(sc);
	}
}

void
audio_attach(struct audio_softc **softc)
{
	struct audio_softc *sc;
	sc = calloc(1, sizeof(struct audio_softc));
	*softc = sc;
	audio_softc_init(sc);
	sc->hw_if->allocm = win_allocm;
	sc->hw_if->freem = win_freem;
	sc->hw_if->start_output = win_start_output;
	sc->hw_if->halt_output = win_halt_output;
	sc->hw_hdl = sc;

	sc->phys = calloc(1, sizeof(audio_dev_win32_t));

	audio_dev_win32_t *dev = sc->phys;
	/* こっちが一次情報 */
	dev->deviceID = WAVE_MAPPER;
	dev->wfx.wFormatTag = WAVE_FORMAT_PCM;
	dev->wfx.nSamplesPerSec = 48000;
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

	audio_mixer_init(sc, sc->sc_pmixer, AUMODE_PLAY);
	audio_mixer_init(sc, sc->sc_rmixer, AUMODE_RECORD);
}

void
audio_detach(struct audio_softc *sc)
{
	audio_dev_win32_t *dev = sc->phys;

#ifdef AUDIO_INTR_EMULATED
	audio_file_t *f;
	SLIST_FOREACH(f, &sc->sc_files, entry) {
		audio_track_t *ptrack = &f->ptrack;

		sys_ioctl_drain(ptrack, true);
	}
	printf("output=%d complete=%d\n", (int)sc->sc_pmixer->hw_output_counter, (int)sc->sc_pmixer->hw_complete_counter);
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
	audio_trackmixer_t *mixer = sc->sc_pmixer;

	lock(sc);
	audio_softc_play_start_core(sc);
	unlock(sc);
}

static WAVEHDR*
win_trygetwh(audio_dev_win32_t *dev)
{
	WAVEHDR* wh = NULL;
	for (int i = 0; i < WAVEHDR_COUNT; i++) {
		if ((dev->wavehdr[i].dwFlags & WHDR_INQUEUE) == 0 && !dev->wavehdr[i].dwUser) {
			wh = &dev->wavehdr[i];
			break;
		}
	}
	return wh;
}

static void audio_softc_play_start_core(struct audio_softc *sc)
{
	audio_dev_win32_t *dev = sc->phys;
	audio_trackmixer_t *mixer = sc->sc_pmixer;

	WAVEHDR* wh = win_trygetwh(dev);
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

	for (int loop = 0; loop < 1; loop++) {
		count = audio_ring_unround_count(&mixer->hwbuf);
		count = min(count, wh_free_count);

		DEV_SAMPLE_T *src = RING_TOP(DEV_SAMPLE_T, &mixer->hwbuf);
		
		int bytelen = count * dev->wfx.nBlockAlign;
		memcpy(dst, src, bytelen);

		dst += bytelen;
		wh->dwBufferLength += bytelen;
		wh_free_count -= count;
	}

//	KASSERT(wh->dwBufferLength % 1764 == 0);
#ifdef DEBUG_ONEBUF
#else
	wh->dwUser = 1;

	MMRESULT r;
	r = waveOutWrite(dev->handle, wh, sizeof(WAVEHDR));
	if (r != MMSYSERR_NOERROR) {
		panic("");
	}

	if (win_trygetwh(dev) != NULL) {
		win_kick_intr(sc, wh);
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
