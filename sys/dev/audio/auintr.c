#include "audiovar.h"
#include "aumix.h"
#include "auintr.h"

// 割り込みエミュレーション。

static struct intr_t intr;

// デバイスから割り込みを発生したことを通知する。
// INT 信号線のドライブに相当。
void
emu_intr(struct intr_t x)
{
#if !true
	while (intr.code != 0);
#else
	if (intr.code != 0) {
		panic("intr on intr");
	}
#endif
	intr = x;
}

// CPU の割り込み受付・ディスパッチに相当。
void
emu_intr_check()
{
	if (intr.code != 0) {
		struct intr_t y = intr;
		intr.code = 0;

		switch (y.code) {
		case INTR_TRACKMIXER:
			mutex_enter(y.sc->sc_intr_lock);
#if defined(_WIN32)
			audio_pintr(y.sc);
#else
			// 指定された func、arg を呼ぶ
			y.func(y.arg);
#endif
			mutex_exit(y.sc->sc_intr_lock);
			break;
		}
	}
}
