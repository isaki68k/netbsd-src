// テストソース .cpp から include される .cpp ファイル

void
do_test(int n)
{
	testname[0] = '\0';
	descname[0] = '\0';
	testtable[n].func();
}

// err(3) ぽい自前関数
#define err(code, fmt...)	xp_err(code, __LINE__, fmt)
void xp_err(int, int, const char *, ...) __printflike(3, 4) __dead;
void
xp_err(int code, int line, const char *fmt, ...)
{
	va_list ap;
	int backup_errno;

	backup_errno = errno;
	printf(" ERR %d: ", line);
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	printf(": %s\n", strerror(backup_errno));

	exit(code);
}

#define errx(code, fmt...)	xp_errx(code, __LINE__, fmt)
void xp_errx(int, int, const char *, ...) __printflike(3, 4) __dead;
void
xp_errx(int code, int line, const char *fmt, ...)
{
	va_list ap;

	printf(" ERR %d: ", line);
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	printf("\n");

	exit(code);
}

void
init(int unit)
{
	audio_device_t dev;
	char name[256];
	size_t len;
	int r;
	int rel;
	int fd;

	snprintf(devaudio, sizeof(devaudio), "/dev/audio%d", unit);
	snprintf(devsound, sizeof(devsound), "/dev/sound%d", unit);
	snprintf(devaudioctl, sizeof(devaudioctl), "/dev/audioctl%d", unit);
	snprintf(devmixer, sizeof(devmixer), "/dev/mixer%d", unit);
	snprintf(hwconfig9, sizeof(hwconfig9), "audio%d", unit);
	if (debug)
		printf("unit = %d\n", unit);

	// バージョンを適当に判断。
	// 7 系なら 7
	// 8 系なら 8
	// 8.99 系で AUDIO2 なら 9
	// 8.99 系でそれ以外なら 8
	len = sizeof(rel);
	r = sysctlbyname("kern.osrevision", &rel, &len, NULL, 0);
	if (r == -1)
		err(1, "sysctl: osrevision");
	netbsd = rel / 100000000;
	if (netbsd == 8) {
		len = sizeof(name);
		r = sysctlbyname("kern.version", name, &len, NULL, 0);
		if (r == -1)
			err(1, "sysctl: version");
		if (strstr(name, "AUDIO2"))
			netbsd++;
	}

	if (debug)
		printf("netbsd = %d\n", netbsd);

	// デバイスのプロパティを取得
	// GETPROPS のための open/ioctl/close が信頼できるかどうかは悩ましいが
	// とりあえずね

	fd = open(devaudio, O_WRONLY);
	if (fd == -1)
		err(1, "init: open: %s", devaudio);
	r = ioctl(fd, AUDIO_GETPROPS, &props);
	if (r == -1)
		err(1, "init:AUDIO_GETPROPS");
	canplay = (props & AUDIO_PROP_PLAYBACK) ? 1 : 0;
	canrec = (props & AUDIO_PROP_CAPTURE) ? 1 : 0;
	hwfull = (props & AUDIO_PROP_FULLDUPLEX) ? 1 : 0;

	// NetBSD8 の sysctl 用に MD 名を取得。
	// GETDEV で得られる値の定義がないので、現物合わせでよしなに頑張るorz
	r = ioctl(fd, AUDIO_GETDEV, &dev);
	if (r == -1)
		err(1, "init:AUDIO_GETDEV");
	if (strcmp(dev.config, "eap") == 0 ||
	    strcmp(dev.config, "vs") == 0)
	{
		// config に MD デバイス名(unit 番号なし)が入っているパターン
		// o eap (VMware Player)
		// o vs (x68k)
		// サウンドカードが2つはないのでへーきへーき…
		snprintf(hwconfig, sizeof(hwconfig), "%s0", dev.config);
	} else {
		// そうでなければ
		// MDデバイス名が入っていないパターン
		// o hdafg (mai)
		// config に MD デバイス名(unit 番号あり)が入ってるパターン
		// o auich (VirtualBox)

		// XXX orz
		if (strcmp(dev.config, "01h") == 0) {
			strlcpy(hwconfig, "hdafg0", sizeof(hwconfig));
		} else {
			strlcpy(hwconfig, dev.config, sizeof(hwconfig));
		}
	}
	if (debug)
		printf("hwconfig = %s\n", hwconfig);
	close(fd);

	// vs0 だけ制約がいろいろ多い。
	if (strcmp(hwconfig, "vs0") == 0)
		vs0 = 1;

	// x68k という制約もある。
	struct utsname utsname;
	r = uname(&utsname);
	if (r == -1)
		err(1, "uname");
	if (strcmp(utsname.machine, "x68k") == 0)
		x68k = 1;
}

// テスト名
static inline void TEST(const char *, ...) __printflike(1, 2);
static inline void
TEST(const char *name, ...)
{
	va_list ap;

	va_start(ap, name);
	vsnprintf(testname, sizeof(testname), name, ap);
	va_end(ap);
	printf("%s\n", testname);
	fflush(stdout);

	descname[0] = '\0';
}

// テスト詳細
static inline void DESC(const char *, ...) __printflike(1, 2);
static inline void
DESC(const char *name, ...)
{
	va_list ap;

	va_start(ap, name);
	vsnprintf(descname, sizeof(descname), name, ap);
	va_end(ap);

	if (debug)
		printf("%s(%s)\n", testname, descname);
}

// 検査

// XP_FAIL は呼び出し元でテストした上で失敗した時に呼ぶ。
// xp_fail はすでに testcount を追加した後で呼ぶ。
#define XP_FAIL(fmt...)	do {	\
	testcount++;	\
	xp_fail(__LINE__, fmt);	\
} while (0)
void xp_fail(int line, const char *fmt, ...)
{
	va_list ap;

	printf(" FAIL %d: %s", line, testname);
	if (descname[0])
		printf("(%s)", descname);
	printf(": ");
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	printf("\n");
	fflush(stdout);
	failcount++;
}

// XP_SUCCESS は XP_FAIL の else 側で呼ぶ。通常は不要。
#define XP_SUCCESS() do { testcount++; } while (0)

#define XP_EXPFAIL(fmt...)	do { \
	testcount++;	\
	xp_expfail(__LINE__, fmt);	\
} while (0)
void xp_expfail(int line, const char *fmt, ...)
{
	va_list ap;

	printf(" Expected Failure %d: %s", line, testname);
	if (descname[0])
		printf("(%s)", descname);
	printf(": ");
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	printf("\n");
	fflush(stdout);
	expfcount++;
}

#define XP_SKIP(fmt...)	do { \
	testcount++;	\
	xp_skip(__LINE__, fmt);	\
} while (0)
void xp_skip(int line, const char *fmt, ...)
{
	va_list ap;

	printf(" SKIP %d: %s", line, testname);
	if (descname[0])
		printf("(%s)", descname);
	printf(": ");
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	printf("\n");
	fflush(stdout);
	skipcount++;
}

#define XP_EQ(exp, act)	xp_eq(__LINE__, exp, act, #act)
void xp_eq(int line, int exp, int act, const char *varname)
{
	testcount++;
	if (exp != act)
		xp_fail(line, "%s expects %d but %d", varname, exp, act);
}
void xp_eq(int line, const char *exp, const char *act, const char *varname)
{
	testcount++;
	if (strcmp(exp, act) != 0)
		xp_fail(line, "%s expects \"%s\" but \"%s\"", varname, exp, act);
}

#define XP_NE(exp, act)	xp_ne(__LINE__, exp, act, #act)
void xp_ne(int line, int exp, int act, const char *varname)
{
	testcount++;
	if (exp == act)
		xp_fail(line, "%s expects != %d but %d", varname, exp, act);
}

// システムコールの結果 exp になることを期待
#define XP_SYS_EQ(exp, act)	xp_sys_eq(__LINE__, exp, act, #act)
void xp_sys_eq(int line, int exp, int act, const char *varname)
{
	testcount++;
	if (act == -1) {
		xp_fail(line, "%s expects %d but -1,err#%d(%s)", varname, exp,
			errno, strerror(errno));
	} else {
		xp_eq(line, exp, act, varname);
	}
}

// システムコールの結果 exp 以外になることを期待
// エラーもテスト成功に含む
#define XP_SYS_NE(exp, act)	xp_sys_ne(__LINE__, exp, act, #act)
void xp_sys_ne(int line, int exp, int act, const char *varname)
{
	testcount++;
	if (act != -1) {
		xp_ne(line, exp, act, varname);
	}
}

// システムコールの結果成功することを期待
// open(2) のように返ってくる成功値が分からない場合用
#define XP_SYS_OK(act)	xp_sys_ok(__LINE__, act, #act)
void xp_sys_ok(int line, int act, const char *varname)
{
	testcount++;
	if (act == -1)
		xp_fail(line, "%s expects success but -1,err#%d(%s)",
			varname, errno, strerror(errno));
}
void xp_sys_ok(int line, void *act, const char *varname)
{
	testcount++;
	if (act == (void *)-1)
		xp_fail(line, "%s expects success but -1,err#%d(%s)",
			varname, errno, strerror(errno));
}

// システムコールがexperrnoで失敗することを期待
#define XP_SYS_NG(experrno, act) xp_sys_ng(__LINE__, experrno, act, #act)
void xp_sys_ng(int line, int experrno, int act, const char *varname)
{
	testcount++;
	if (act != -1) {
		xp_fail(line, "%s expects -1,err#%d but %d",
			varname, experrno, act);
	} else if (experrno != errno) {
		char acterrbuf[100];
		int acterrno = errno;
		strlcpy(acterrbuf, strerror(acterrno), sizeof(acterrbuf));
		xp_fail(line, "%s expects -1,err#%d(%s) but -1,err#%d(%s)",
			varname, experrno, strerror(experrno),
			acterrno, acterrbuf);
	}
}
void xp_sys_ng(int line, int experrno, void *act, const char *varname)
{
	testcount++;
	if (act != (void *)-1) {
		xp_fail(line, "%s expects -1,err#%d but %p",
			varname, experrno, act);
	} else if (experrno != errno) {
		char acterrbuf[100];
		int acterrno = errno;
		strlcpy(acterrbuf, strerror(acterrno), sizeof(acterrbuf));
		xp_fail(line, "%s expects -1,err#%d(%s) but -1,err#%d(%s)",
			varname, experrno, strerror(experrno),
			acterrno, acterrbuf);
	}
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

// ポインタ版 (mmap)
// -1 は -1 と表示してくれたほうが分かりやすい
#define DRESULT_PTR(r)	do {	\
	int backup_errno = errno;	\
	if ((r) == (void *)-1) {	\
		DPRINTF(" = -1, err#%d %s\n",	\
			backup_errno, strerror(backup_errno));	\
	} else {	\
		DPRINTF(" = %p\n", r);	\
	}	\
	errno = backup_errno;	\
	return r;	\
} while (0)

static const char *openmodetable[] = {
	"O_RDONLY",
	"O_WRONLY",
	"O_RDWR",
};
static const char *aumodetable[] __unused = {
	"RECORD",
	"PLAY",
	"PLAY|REC",
	"AUMODE_0",
};

// システムコールはこのマクロを経由して呼ぶ
#define OPEN(name, mode)	debug_open(__LINE__, name, mode)
int debug_open(int line, const char *name, int mode)
{
	char modestr[32];
	int n;

	if ((mode & 3) != 3)
		n = snprintf(modestr, sizeof(modestr), "%s", openmodetable[mode & 3]);
	else
		n = snprintf(modestr, sizeof(modestr), "%d", mode & 3);
	if (mode & O_NONBLOCK)
		n += snprintf(modestr + n, sizeof(modestr) - n, "|O_NONBLOCK");

	DPRINTFF(line, "open(\"%s\", %s)", name, modestr);
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
#define IOCTL(fd, name, addr, addrfmt...)	\
	debug_ioctl(__LINE__, fd, name, #name, addr, addrfmt)
int debug_ioctl(int line, int fd, u_long name, const char *namestr,
	void *addr, const char *addrfmt, ...)
{
	char addrbuf[100];
	va_list ap;

	va_start(ap, addrfmt);
	vsnprintf(addrbuf, sizeof(addrbuf), addrfmt, ap);
	va_end(ap);
	DPRINTFF(line, "ioctl(%d, %s, %s)", fd, namestr, addrbuf);
	int r = ioctl(fd, name, addr);
	DRESULT(r);
}

#define FCNTL(fd, name...)	\
	debug_fcntl(__LINE__, fd, name, #name)
int debug_fcntl(int line, int fd, int name, const char *namestr, ...)
{
	int r;

	switch (name) {
	 case F_GETFL:	// 引数なし
		DPRINTFF(line, "fcntl(%d, %s)", fd, namestr);
		r = fcntl(fd, name);
		break;
	}
	DRESULT(r);
}

#define CLOSE(fd)	debug_close(__LINE__, fd)
int debug_close(int line, int fd)
{
	DPRINTFF(line, "close(%d)", fd);
	int r = close(fd);

	// ちょっと待つ必要がある
	if (strcmp(hwconfig, "pad0") != 0) {
		usleep(100);
	}

	DRESULT(r);
}

#define MMAP(ptr, len, prot, flags, fd, offset)	\
	debug_mmap(__LINE__, ptr, len, prot, flags, fd, offset)
void *debug_mmap(int line, void *ptr, size_t len, int prot, int flags, int fd,
	off_t offset)
{
	char protbuf[256];
	char flagbuf[256];
	int n;

#define ADDFLAG(buf, var, name)	do {	\
	if (((var) & (name)))	\
		n = strlcat(buf, "|" #name, sizeof(buf));	\
		var &= ~(name);	\
} while (0)

	n = 0;
	protbuf[n] = '\0';
	if (prot == 0) {
		strlcpy(protbuf, "|PROT_NONE", sizeof(protbuf));
	} else {
		ADDFLAG(protbuf, prot, PROT_EXEC);
		ADDFLAG(protbuf, prot, PROT_WRITE);
		ADDFLAG(protbuf, prot, PROT_READ);
		if (prot != 0)
			snprintf(protbuf + n, sizeof(protbuf) - n, "|prot=0x%x", prot);
	}

	n = 0;
	flagbuf[n] = '\0';
	if (flags == 0) {
		strlcpy(flagbuf, "|MAP_FILE", sizeof(flagbuf));
	} else {
		ADDFLAG(flagbuf, flags, MAP_SHARED);
		ADDFLAG(flagbuf, flags, MAP_PRIVATE);
		ADDFLAG(flagbuf, flags, MAP_FIXED);
		ADDFLAG(flagbuf, flags, MAP_INHERIT);
		ADDFLAG(flagbuf, flags, MAP_HASSEMAPHORE);
		ADDFLAG(flagbuf, flags, MAP_TRYFIXED);
		ADDFLAG(flagbuf, flags, MAP_WIRED);
		ADDFLAG(flagbuf, flags, MAP_ANON);
		if ((flags & MAP_ALIGNMENT_MASK)) {
			n += snprintf(flagbuf + n, sizeof(flagbuf) - n, "|MAP_ALIGN(%d)",
				((flags & MAP_ALIGNMENT_MASK) >> MAP_ALIGNMENT_SHIFT));
			flags &= ~MAP_ALIGNMENT_MASK;
		}
		if (flags != 0)
			n += snprintf(flagbuf + n, sizeof(flagbuf) - n, "|flag=0x%x",
				flags);
	}

	DPRINTFF(line, "mmap(%p, %zd, %s, %s, %d, %jd)",
		ptr, len, protbuf + 1, flagbuf + 1, fd, offset);
	void *r = mmap(ptr, len, prot, flags, fd, offset);
	DRESULT_PTR(r);
}

#define MUNMAP(ptr, len)	debug_munmap(__LINE__, ptr, len)
int debug_munmap(int line, void *ptr, int len)
{
	DPRINTFF(line, "munmap(%p, %d)", ptr, len);
	int r = munmap(ptr, len);
	DRESULT(r);
}

#define POLL(pfd, nfd, timeout)	debug_poll(__LINE__, pfd, nfd, timeout)
int debug_poll(int line, struct pollfd *pfd, int nfd, int timeout)
{
	char buf[256];
	int n = 0;
	buf[n] = '\0';
	for (int i = 0; i < nfd; i++) {
		n += snprintf(buf + n, sizeof(buf) - n, "{fd=%d,events=%d}",
			pfd[i].fd, pfd[i].events);
	}
	DPRINTFF(line, "poll(%s, %d, %d)", buf, nfd, timeout);
	int r = poll(pfd, nfd, timeout);
	DRESULT(r);
}

#define KQUEUE()	debug_kqueue(__LINE__)
int debug_kqueue(int line)
{
	DPRINTFF(line, "kqueue()");
	int r = kqueue();
	DRESULT(r);
}

#define KEVENT_SET(kq, kev, nev)	debug_kevent_set(__LINE__, kq, kev, nev)
int debug_kevent_set(int line, int kq, struct kevent *kev, size_t nev)
{
	DPRINTFF(line, "kevent_set(%d, %p, %zd)", kq, kev, nev);
	int r = kevent(kq, kev, nev, NULL, 0, NULL);
	DRESULT(r);
}

#define KEVENT_POLL(kq, kev, nev, ts) \
	debug_kevent_poll(__LINE__, kq, kev, nev, ts)
int debug_kevent_poll(int line, int kq, struct kevent *kev, size_t nev,
	const struct timespec *ts)
{
	char tsbuf[32];

	if (ts == NULL) {
		snprintf(tsbuf, sizeof(tsbuf), "NULL");
	} else if (ts->tv_sec == 0 && ts->tv_nsec == 0) {
		snprintf(tsbuf, sizeof(tsbuf), "0.0");
	} else {
		snprintf(tsbuf, sizeof(tsbuf), "%d.%09ld",
			(int)ts->tv_sec, ts->tv_nsec);
	}
	DPRINTFF(line, "kevent_poll(%d, %p, %zd, %s)", kq, kev, nev, tsbuf);
	int r = kevent(kq, NULL, 0, kev, nev, ts);
	DRESULT(r);
}

#define DEBUG_KEV(name, kev)	debug_kev(__LINE__, name, kev)
void debug_kev(int line, const char *name, const struct kevent *kev)
{
	char flagbuf[256];
	const char *filterbuf;
	uint32_t v;
	int n;

	n = 0;
	flagbuf[n] = '\0';
	if (kev->flags == 0) {
		strcpy(flagbuf, "|0?");
	} else {
		v = kev->flags;
		ADDFLAG(flagbuf, v, EV_ADD);
		if (v != 0)
			snprintf(flagbuf + n, sizeof(flagbuf)-n, "|0x%x", v);
	}

	switch (kev->filter) {
	 case EVFILT_READ:	filterbuf = "EVFILT_READ";	break;
	 case EVFILT_WRITE:	filterbuf = "EVFILT_WRITE";	break;
	 default:			filterbuf = "EVFILT_?";		break;
	}

	DPRINTFF(line,
		"%s={id:%d,%s,%s,fflags:0x%x,data:0x%" PRIx64 ",udata:0x%x}\n",
		name,
		(int)kev->ident,
		flagbuf + 1,
		filterbuf,
		kev->fflags,
		kev->data,
		(int)kev->udata);
}

#define FSTAT(fd, st) debug_fstat(__LINE__, fd, st)
int debug_fstat(int line, int fd, struct stat *st)
{
	DPRINTFF(line, "fstat(%d, %p)", fd, st);
	int r = fstat(fd, st);
	DRESULT(r);
}

#define GETUID()	debug_getuid(__LINE__)
uid_t debug_getuid(int line)
{
	DPRINTFF(line, "getuid");
	uid_t r = getuid();
	DRESULT(r);
}

#define SETEUID(id)	debug_seteuid(__LINE__, id)
int debug_seteuid(int line, uid_t id)
{
	DPRINTFF(line, "seteuid(%d)", (int)id);
	int r = seteuid(id);
	DRESULT(r);
}

#define SYSCTLBYNAME(name, oldp, oldlenp, newp, newlen)	\
	debug_sysctlbyname(__LINE__, name, oldp, oldlenp, newp, newlen)
int debug_sysctlbyname(int line, const char *name, void *oldp, size_t *oldlenp,
	const void *newp, size_t newlen)
{
	DPRINTFF(line, "sysctlbyname(\"%s\")", name);
	int r = sysctlbyname(name, oldp, oldlenp, newp, newlen);
	DRESULT(r);
}

#define SYSTEM(cmd)	debug_system(__LINE__, cmd)
int debug_system(int line, const char *cmd)
{
	DPRINTFF(line, "system(\"%s\")", cmd);
	int r = system(cmd);
	DRESULT(r);
}

// popen() して1行目を buf に返す。
// 成功すれば戻り値 0、失敗すれば戻り値 -1 を返す。
#define POPEN_GETS(buf, buflen, cmd...) \
	debug_popen_gets(__LINE__, buf, buflen, cmd)
int debug_popen_gets(int line, char *buf, size_t bufsize, const char *cmd, ...)
{
	char cmdbuf[256];
	va_list ap;
	FILE *fp;
	char *p;

	va_start(ap, cmd);
	vsnprintf(cmdbuf, sizeof(cmdbuf), cmd, ap);
	va_end(ap);

	DPRINTFF(line, "popen(\"%s\", \"r\")", cmdbuf);
	fp = popen(cmdbuf, "r");
	if (fp == NULL) {
		DPRINTF(" = NULL, popen failed\n");
		return -1;
	}
	p = fgets(buf, bufsize, fp);
	pclose(fp);
	if (p == NULL) {
		DPRINTF(" = NULL, fgets failed\n");
		return -1;
	}

	p = strchr(buf, '\n');
	if (p)
		*p = '\0';

	DPRINTF(" = \"%s\"\n", buf);
	return 0;
}

// ---

// O_* を AUMODE_* に変換。HW Full 固定
int mode2aumode_full[] = {
	AUMODE_RECORD,
	AUMODE_PLAY | AUMODE_PLAY_ALL,
	AUMODE_PLAY | AUMODE_PLAY_ALL | AUMODE_RECORD,
};
// O_* を AUMODE_* に変換 (hwfull を考慮)
int mode2aumode(int mode)
{
	int aumode = mode2aumode_full[mode];
	if (hwfull == 0 && mode == O_RDWR)
		aumode &= ~AUMODE_RECORD;
	return aumode;
}

// O_* を PLAY 側がオープンされているかに変換。HW Full 固定
int mode2popen_full[] = {
	0, 1, 1,
};
// O_* を PLAY 側がオープンされてるかに変換 (hwfull を考慮、ただし同じになる)
int mode2popen(int mode)
{
	return mode2popen_full[mode];
}

// O_* を RECORD 側がオープンされてるかに変換。HW Full 固定
int mode2ropen_full[] = {
	1, 0, 1,
};
// O_* を RECORD 側がオープンされてるかに変換 (hwfull を考慮)
int mode2ropen(int mode)
{
	int rec = mode2ropen_full[mode];
	if (hwfull == 0 && mode == O_RDWR)
		rec = 0;
	return rec;
}

// sysctl で使う HW デバイス名を返します。
// N8 では MD 名(vs0 とか)、
// N9 では audioN です。
const char *
hwconfigname()
{
	if (netbsd <= 8) {
		return hwconfig;
	} else {
		return hwconfig9;
	}
}
