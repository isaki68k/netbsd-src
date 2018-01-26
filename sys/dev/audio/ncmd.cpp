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
	exit(1);
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

#define DPRINTF(fmt...)	do {	\
	if (debug)	\
		printf(fmt);	\
} while (0)

#define DPRINTFF(line, fmt...)	do {	\
	if (debug) {	\
		printf("  > %d: ", line);	\
		DPRINTF(fmt);	\
		fflush(stdout);	\
	}	\
} while (0)

#define DRESULT(r)	do {	\
	int backup_errno = errno;	\
	if ((r) == -1) {	\
		DPRINTF(" = %d, err#%d %s\n",	\
			r, backup_errno, strerror(backup_errno));	\
	} else {	\
		DPRINTF(" = %d\n", r);	\
	}	\
	errno = backup_errno;	\
	return r;	\
} while (0)

static const char *openmodetable[] = {
	"O_RDONLY",
	"O_WRONLY",
	"O_RDWR",
};

// システムコールはこのマクロを経由して呼ぶ
#define OPEN(name, mode)	debug_open(__LINE__, name, mode)
int debug_open(int line, const char *name, int mode)
{
	if (0 <= mode && mode <= 2)
		DPRINTFF(line, "open(\"%s\", %s)", name, openmodetable[mode]);
	else
		DPRINTFF(line, "open(\"%s\", %d)", name, mode);
	int r = open(name, mode);
	DRESULT(r);
}

#define WRITE(fd, addr, len)	debug_write(__LINE__, fd, addr, len)
int debug_write(int line, int fd, const void *addr, size_t len)
{
	DPRINTFF(line, "write(%d, %p, %zd)", fd, addr, len);
	int r = write(fd, addr, len);
	DRESULT(r);
}

#define READ(fd, addr, len)	debug_read(__LINE__, fd, addr, len)
int debug_read(int line, int fd, void *addr, size_t len)
{
	DPRINTFF(line, "read(%d, %p, %zd)", fd, addr, len);
	int r = read(fd, addr, len);
	DRESULT(r);
}

// addrstr は値についてのコメント。ex.
//	int onoff = 0;
//	ioctl(fd, SWITCH, onoff); -> IOCTL(fd, SWITCH, onoff, "off")
#define IOCTL(fd, name, addr, addrstr)	\
	debug_ioctl(__LINE__, fd, name, #name, addr, addrstr)
int debug_ioctl(int line, int fd, u_long name, const char *namestr,
	void *addr, const char *addrstr)
{
	DPRINTFF(line, "ioctl(%d, %s, %s)", fd, namestr, addrstr);
	int r = ioctl(fd, name, addr);
	DRESULT(r);
}

#define CLOSE(fd)	debug_close(__LINE__, fd)
int debug_close(int line, int fd)
{
	DPRINTFF(line, "close(%d)", fd);
	int r = close(fd);
	DRESULT(r);
}


// N7 では SETFD で full-duplex にしたことが close に反映されてないので、
// O_RDONLY でオープンして、SETFD して再生している状態で
// close すると drain 他が実行されないような気がする。
int
cmd_SETFD(int ac, char *av[])
{
	int fdfile;
	int fd;
	int r;
	int on;

	if (ac < 2) {
		printf("usage: <filename.wav>\n");
		exit(1);
	}

	// ファイルオープン
	fdfile = OPEN(av[1], O_RDONLY);
	if (fdfile == -1)
		err(1, "open: %s", av[1]);

	// Half-Duplex でオープン
	fd = OPEN("/dev/audio", O_RDONLY);
	if (fd == -1)
		err(1, "open");

	// Full-Duplex
	on = 1;
	r = IOCTL(fd, AUDIO_SETFD, &on, "on");
	if (r == -1)
		err(1, "ioctl");

	// 再生
	char buf[32768];
	r = READ(fdfile, buf, sizeof(buf));
	r = WRITE(fd, buf, r);
	if (r == -1)
		err(1, "write");

	// クローズ
	r = CLOSE(fd);
	if (r == -1)
		err(1, "close");

	CLOSE(fdfile);
	return 0;
}


// コマンド一覧
#define DEF(x)	{ #x, cmd_ ## x }
struct cmdtable cmdtable[] = {
	DEF(SETFD),
	{ NULL, NULL },
};
#undef DEF
