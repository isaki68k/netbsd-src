#include "audiovar.h"
#include "aumix.h"

#define INTR_TRACKMIXER		1

struct intr_t
{
	volatile int code;
	struct audio_softc *sc;

#if defined(_WIN32)
	audio_trackmixer_t *mixer;
#else
	// XXX Windows 側もこれに揃えたほうがいい
	void (*func)(void *);
	void *arg;
#endif
	int count;
};

void emu_intr(struct intr_t x);
void emu_intr_check();

