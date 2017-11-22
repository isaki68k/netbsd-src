#include "compat.h"
#include "auintr.h"

int
cv_wait_sig(kcondvar_t *cv, kmutex_t *lock)
{
	// 本当は割り込みハンドラからトラックが消費されるんだけど
	// ここで消費をエミュレート。

	for (;;) {
		emu_intr_check();
		if (cv->v == 1) {
			cv->v = 0;
			return 0;
		}
	}
	/* NOTREACHED */
}

void
audio_softc_init(struct audio_softc *sc)
{
	sc->sc_pmixer = malloc(sizeof(audio_trackmixer_t));
	sc->sc_rmixer = malloc(sizeof(audio_trackmixer_t));
	sc->sc_lock = &sc->sc_lock0;
	sc->sc_intr_lock = &sc->sc_intr_lock0;
	sc->hw_if = &sc->hw_if0;
}

void
aprint_error_dev(void *dev, const char *fmt, ...)
{
}

void
aprint_normal_dev(void *dev, const char *fmt, ...)
{
}
