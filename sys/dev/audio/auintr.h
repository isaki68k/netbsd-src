#include "audiovar.h"
#include "aumix.h"

#define INTR_TRACKMIXER		1

struct intr_t
{
	volatile int code;
	struct audio_softc *sc;

	audio_trackmixer_t *mixer;
	int count;
};

void emu_intr(struct intr_t x);
void emu_intr_check();

