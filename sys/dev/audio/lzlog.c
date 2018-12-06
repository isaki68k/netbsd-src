// ログをメモリに書き溜めておいて、close 時にカーネルログに出力する。
// m68k だとログをコンソールに出しながらの実時間動作はできないので。

#if !defined(_KERNEL)
#include <err.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define kmem_zalloc(size, flag)	calloc(size, 1)
#define kmem_free(ptr, size)	free(ptr)
#define KM_SLEEP	0
#endif

static void lzlog_open(int);
static void lzlog_close(void);
static void lzlog_printf(const char *, ...) __printflike(1, 2);
static void lzlog_flush(void);

static char *lzlog_buf;
static int lzlog_size;
static int lzlog_wptr;	// 次の書き込み位置 (現在の末尾 '\0' の位置)

// サイズは確保するメモリサイズ。
static void
lzlog_open(int size)
{
	lzlog_size = size;
	lzlog_buf = kmem_zalloc(lzlog_size, KM_SLEEP);

	lzlog_wptr = 0;
}

static void
lzlog_close(void)
{
	kmem_free(lzlog_buf, lzlog_size);
	lzlog_buf = NULL;
}

static void
lzlog_printf(const char *fmt, ...)
{
	char buf[1024];
	va_list ap;
	int i;

	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);

	// メモリ確保されてなければフォールバック
	if (lzlog_buf == NULL) {
		printf("* %s", buf);
		return;
	}

	for (i = 0; buf[i] != '\0'; i++) {
		lzlog_buf[lzlog_wptr] = buf[i];
		lzlog_wptr = (lzlog_wptr + 1) % lzlog_size;
	}
	lzlog_buf[lzlog_wptr] = '\0';
}

static void
lzlog_flush(void)
{
	char buf[1024];
	int rptr;
	int i;

	rptr = lzlog_wptr - 1;
	if (rptr < 0)
		rptr = lzlog_size - 1;

	// 先頭を探す
	for (; lzlog_buf[rptr] != '\0'; ) {
		rptr--;
		if (rptr < 0)
			rptr = lzlog_size - 1;
	}
	rptr = (rptr + 1) % lzlog_size;

	i = 0;
	for (; lzlog_buf[rptr] != '\0'; rptr = (rptr + 1) % lzlog_size) {
		int ch = lzlog_buf[rptr];
		buf[i++] = ch;

		if (ch == '\n') {
			buf[i] = '\0';
			printf("%s", buf);
			i = 0;
		}
	}
}

#if !defined(_KERNEL)
int
main(int ac, char *av[])
{
	int i;

	lzlog_open(10);
	for (i = 1; i < ac; i++) {
		lzlog_printf("%s\n", av[i]);
	}
	lzlog_flush();
	lzlog_close();
	return 0;
}
#endif
