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
	if (intr.code != 0) {
		// 多重割り込みはサポートしていない
		panic("intr on intr");
	}
	intr = x;
}

// CPU の割り込み受付・ディスパッチに相当。
void
emu_intr_check()
{
	if (intr.code != 0) {
		switch (intr.code) {
		case INTR_TRACKMIXER:
			audio_trackmixer_intr(intr.mixer, intr.count);
			break;
		}
		intr.code = 0;
	}
}
