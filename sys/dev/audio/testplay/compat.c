/*
 * emulation
 */

#include "compat.h"
#include "userland.h"

// panic メッセージを表示するかどうか制御する。テスト用。
int panic_msgout = 1;

int
cv_wait_sig(kcondvar_t *cv, kmutex_t *lock)
{
	for (;;) {
		if (cv->v == 1) {
			cv->v = 0;
			return 0;
		}
	}
	/* NOTREACHED */
}

void
aprint_error_dev(device_t dev, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
}

void
aprint_normal_dev(device_t dev, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
}
