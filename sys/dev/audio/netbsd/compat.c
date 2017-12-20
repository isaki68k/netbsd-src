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
aprint_error_dev(void *dev, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
}

void
aprint_normal_dev(void *dev, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
}
