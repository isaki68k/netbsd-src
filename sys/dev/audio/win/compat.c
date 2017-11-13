#include "compat.h"
#include "auintr.h"

int
cv_wait_sig(kcondvar_t *cv, void *lock)
{
	// 本当は割り込みハンドラからトラックが消費されるんだけど
	// ここで消費をエミュレート。
	emu_intr_check();
	// nop
	return 0;
}
