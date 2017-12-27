#pragma once

#include "aumix.h"

void audio_attach(struct audio_softc **softc, bool hw);
void audio_detach(struct audio_softc *sc);

void audio_softc_play_start(struct audio_softc *sc);
bool audio_softc_play_busy(struct audio_softc *sc);

void lock(struct audio_softc *sc);
void unlock(struct audio_softc *sc);

// OBSOLETE
void WAIT();

