/*
 * 実環境での自動テストではしにくいテスト用
 */

#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/audioio.h>
#include <sys/cdefs.h>
#include <sys/ioctl.h>
#include <sys/param.h>
#include <sys/sysctl.h>

struct cmdtable {
	const char *name;
	int (*func)(int, char *[]);
};

int debug;
extern struct cmdtable cmdtable[];

void __attribute__((__noreturn__))
usage()
{
	// cmd は一度に1つ。任意の引数あり。
	printf(" %s <cmd> [<arg...>]\n", getprogname());
	for (int i = 0; cmdtable[i].name != NULL; i++) {
		printf("\t%s\n", cmdtable[i].name);
	}
}

int
main(int ac, char *av[])
{
	int i;
	int c;

	// global option
	while ((c = getopt(ac, av, "d")) != -1) {
		switch (c) {
		 case 'd':
			debug++;
			break;
		 default:
			usage();
		}
	}
	ac -= optind;
	av += optind;

	if (ac == 0)
		usage();

	// 先頭が cmd なら一つだけ受け取って処理
	for (int j = 0; cmdtable[j].name != NULL; j++) {
		if (strcmp(av[0], cmdtable[j].name) == 0) {
			cmdtable[j].func(ac, av);
			return 0;
		}
	}

	usage();
	return 0;
}

// コマンド一覧
#define DEF(x)	{ #x, cmd_ ## x }
struct cmdtable cmdtable[] = {
	{ NULL, NULL },
};
#undef DEF
