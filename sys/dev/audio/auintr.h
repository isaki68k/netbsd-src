#include "audiovar.h"
#include "aumix.h"

// 割り込みエミュレーション。
#define AUDIO_INTR_EMULATED

#define INTR_TRACKMIXER		1

struct intr_t
{
	int code;
	struct audio_softc *sc;

	audio_trackmixer_t *mixer;
	int count;
};

void emu_intr(struct intr_t x);
void emu_intr_check();

