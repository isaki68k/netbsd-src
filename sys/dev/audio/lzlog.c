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
static int lzlog_cur;	// 次の書き込み位置 (現在の末尾 '\0' の位置)

// サイズは確保するメモリサイズ。
static void
lzlog_open(int size)
{
	lzlog_size = size;
	lzlog_buf = kmem_zalloc(lzlog_size, KM_SLEEP);

	// 最後に番兵を入れる
	lzlog_size--;
	lzlog_cur = 0;
}

static void
lzlog_close(void)
{
	kmem_free(lzlog_buf, lzlog_size + 1);
	lzlog_buf = NULL;
}

static void
lzlog_printf(const char *fmt, ...)
{
	char buf[1024];
	int len;
	va_list ap;

	va_start(ap, fmt);
	len = vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);

	// メモリ確保されてなければフォールバック
	if (lzlog_buf == NULL) {
		printf("**%s", buf);
		return;
	}

	// len に '\0' の長さは入ってないけど書き込むので計算に注意。

	if (lzlog_cur + len < lzlog_size) {
		strcpy(lzlog_buf + lzlog_cur, buf);
		lzlog_cur = (lzlog_cur + len) % lzlog_size;
	} else {
		// 折り返しが発生する
		int len1 = lzlog_size - lzlog_cur;
		int len2 = len - len1;
		memcpy(lzlog_buf + lzlog_cur, buf, len1);
		strcpy(lzlog_buf, buf + len1);
		lzlog_cur = len2;
	}
}

static void
lzlog_flush(void)
{
	// cur は '\0' の位置を指しているので、'\0' である間スキップ
	while (++lzlog_cur < lzlog_size) {
		if (lzlog_buf[lzlog_cur] != '\0')
			break;
	}
	if (lzlog_cur == lzlog_size) {
		// 末尾まで '\0' だったら、まだ折り返しが発生してない
		printf("%s", lzlog_buf);
	} else {
		// 例えばlzlog_bufが10 バイトのバッファ(9バイト有効)で cur が 6なら
		//  0 1 2 3 4  5 6 7 8  9
		// |D E F G H \0 A B C \0<-番兵
		printf("%s", lzlog_buf + lzlog_cur);
		printf("%s", lzlog_buf);
	}
}

#if !defined(_KERNEL)
int
main(int ac, char *av[])
{
	int i;

	lzlog_open(8);
	for (i = 1; i < ac; i++) {
		lzlog_printf("%s\n", av[i]);
	}
	lzlog_flush();
	lzlog_close();
	return 0;
}
#endif
