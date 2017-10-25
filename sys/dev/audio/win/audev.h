#pragma once

#include "aumix.h"

void
audio_attach(audio_softc_t **softc);
void
audio_detach(audio_softc_t *sc);

void
audio_softc_play_start(audio_softc_t *sc);

bool
audio_softc_play_busy(audio_softc_t *sc);

void lock(audio_softc_t *sc);
void unlock(audio_softc_t *sc);


void WAIT();