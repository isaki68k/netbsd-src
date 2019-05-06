// vi:set ts=8:
/*	$NetBSD$	*/

#include <sys/cdefs.h>
__RCSID("$NetBSD$");

#include <atf-c.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/audioio.h>
#include <sys/ioctl.h>

#include <rump/rump.h>
#include <rump/rump_syscalls.h>

#include "h_macros.h"

/* XXX rump cannot test half duplex */
int hwfull = 1;

/* Convert O_* into AUMODE_* for HW Full */
int mode2aumode_full[] = {
	AUMODE_RECORD,
	AUMODE_PLAY | AUMODE_PLAY_ALL,
	AUMODE_PLAY | AUMODE_PLAY_ALL | AUMODE_RECORD,
};
/* Convert O_* into AUMODE_* (considering hwfull) */
int mode2aumode(int mode)
{
	int aumode = mode2aumode_full[mode];
	if (hwfull == 0 && mode == O_RDWR)
		aumode &= ~AUMODE_RECORD;
	return aumode;
}


/* Convert O_* into 'is playback open' for HW Full */
int mode2popen_full[] = {
	0, 1, 1,
};
/* Convert O_* into 'is_playback open' (considering hwfull) */
int mode2popen(int mode)
{
	return mode2popen_full[mode];
}

/* Convert O_* into 'is recording open' for HW Full */
int mode2ropen_full[] = {
	1, 0, 1,
};
/* Convert O_* into 'is recording open' (considering hwfull) */
int mode2ropen(int mode)
{
	int rec = mode2ropen_full[mode];
	if (hwfull == 0 && mode == O_RDWR)
		rec = 0;
	return rec;
}

static const char *openmodetable[] = {
	"O_RDONLY",
	"O_WRONLY",
	"O_RDWR",
};

/*
 * Check exp == val (for each member not -1)
 */
#define CHECK_AI(expected, val, name)	do { \
	if ((expected)->name != -1) { \
		ATF_CHECK_EQ_MSG((expected)->name, (val)->name, \
		    " expected = %d, actual = %d", \
		    (expected)->name, (val)->name); \
	} \
} while (0)

#define check_ai(expected, val) do { \
	/* play */ \
	CHECK_AI(expected, val, play.sample_rate); \
	CHECK_AI(expected, val, play.channels); \
	CHECK_AI(expected, val, play.precision); \
	CHECK_AI(expected, val, play.encoding); \
			     /* play.gain */ \
			     /* play.port */ \
	CHECK_AI(expected, val, play.seek); \
			     /* play.avail_ports */ \
			     /* play.buffer_size */ \
	CHECK_AI(expected, val, play.samples); \
	CHECK_AI(expected, val, play.eof); \
	CHECK_AI(expected, val, play.pause); \
	CHECK_AI(expected, val, play.error); \
	CHECK_AI(expected, val, play.waiting); \
			     /* play.balance */ \
	CHECK_AI(expected, val, play.open); \
	CHECK_AI(expected, val, play.active); \
	/* record */ \
	CHECK_AI(expected, val, record.sample_rate); \
	CHECK_AI(expected, val, record.channels); \
	CHECK_AI(expected, val, record.precision); \
	CHECK_AI(expected, val, record.encoding); \
			     /* record.gain */ \
			     /* record.port */ \
	CHECK_AI(expected, val, record.seek); \
			     /* record.avail_ports */ \
			     /* record.buffer_size */ \
	CHECK_AI(expected, val, record.samples); \
	CHECK_AI(expected, val, record.eof); \
	CHECK_AI(expected, val, record.pause); \
	CHECK_AI(expected, val, record.error); \
	CHECK_AI(expected, val, record.waiting); \
			     /* record.balance */ \
	CHECK_AI(expected, val, record.open); \
	CHECK_AI(expected, val, record.active); \
	/* ai */ \
			     /* monitor_gain */ \
	CHECK_AI(expected, val, blocksize); \
	CHECK_AI(expected, val, hiwat); \
	CHECK_AI(expected, val, lowat); \
	CHECK_AI(expected, val, mode); \
} while (0)

/*
 * Check /dev/audio's initial parameters.
 * The parameters are the same in every open.
 * XXX cannot test half duplex
 */
static void
test_open_audio(int mode)
{
	struct audio_info ai;
	struct audio_info ai0;
	struct audio_info exp;
	int padmode;
	int padfd;
	int fd;
	int r;

	switch (mode) {
	case O_RDONLY:
		padmode = O_WRONLY;
		atf_tc_skip("pad does not support this mode");
		break;
	case O_WRONLY:
		padmode = O_RDONLY;
		break;
	case O_RDWR:
		padmode = O_RDWR;
		atf_tc_skip("pad does not support this mode");
		break;
	}

	rump_init();
	padfd = rump_sys_open("/dev/pad0", padmode);
	ATF_REQUIRE(padfd >= 0);

	fd = rump_sys_open("/dev/audio0", mode);
	ATF_REQUIRE(fd >= 0);

	/* Check (1st time) */
	memset(&ai, 0, sizeof(ai));
	r = rump_sys_ioctl(fd, AUDIO_GETBUFINFO, &ai);
	ATF_REQUIRE(0 == r);

	AUDIO_INITINFO(&exp);
	exp.play.sample_rate = 8000;
	exp.play.channels = 1;
	exp.play.precision = 8;
	exp.play.encoding = AUDIO_ENCODING_ULAW;
	exp.play.seek = 0;
	exp.play.samples = 0;
	exp.play.eof = 0;
	exp.play.pause = 0;
	exp.play.error = 0;
	exp.play.waiting = 0;
	exp.play.open = mode2popen(mode);
	exp.play.active = 0;
	exp.record.sample_rate = 8000;
	exp.record.channels = 1;
	exp.record.precision = 8;
	exp.record.encoding = AUDIO_ENCODING_ULAW;
	exp.record.seek = 0;
	exp.record.samples = 0;
	exp.record.eof = 0;
	exp.record.pause = 0;
	exp.record.error = 0;
	exp.record.waiting = 0;
	exp.record.open = mode2ropen(mode);
	exp.record.active = 0;
	exp.mode = mode2aumode(mode);
	check_ai(&exp, &ai);

	/* Save it */
	ai0 = ai;

	/* Change as possible */
	AUDIO_INITINFO(&ai);
	ai.blocksize = ai0.blocksize * 2;
	if (ai0.hiwat > 0)
		ai.hiwat = ai0.hiwat - 1;
	if (ai0.lowat < ai0.hiwat)
		ai.lowat = ai0.lowat + 1;
	ai.mode = ai.mode ^ AUMODE_PLAY_ALL;
	ai.play.sample_rate = 11025;
	ai.play.channels = 1;
	ai.play.precision = 16;
	ai.play.encoding = AUDIO_ENCODING_SLINEAR_LE;
	ai.play.pause = 1;
	ai.record.sample_rate = 11025;
	ai.record.channels = 1;
	ai.record.precision = 16;
	ai.record.encoding = AUDIO_ENCODING_SLINEAR_LE;
	ai.record.pause = 1;
	RL(rump_sys_ioctl(fd, AUDIO_SETINFO, &ai));
	/* Then close. */
	RL(rump_sys_close(fd));

	/*
	 * Open and check again.
	 */
	fd = rump_sys_open("/dev/audio0", mode);
	ATF_REQUIRE(fd >= 0);

	memset(&ai, 0, sizeof(ai));
	RL(rump_sys_ioctl(fd, AUDIO_GETBUFINFO, &ai));

	check_ai(&exp, &ai);

	RL(rump_sys_close(fd));
	RL(rump_sys_close(padfd));
}

ATF_TC(open_audio_RDONLY);
ATF_TC_HEAD(open_audio_RDONLY, tc)
{
	atf_tc_set_md_var(tc, "descr", "initial parameters on /dev/audio");
}
ATF_TC_BODY(open_audio_RDONLY, tc)
{
	test_open_audio(O_RDONLY);
}

ATF_TC(open_audio_WRONLY);
ATF_TC_HEAD(open_audio_WRONLY, tc)
{
	atf_tc_set_md_var(tc, "descr", "initial parameters on /dev/audio");
}
ATF_TC_BODY(open_audio_WRONLY, tc)
{
	test_open_audio(O_WRONLY);
}

ATF_TC(open_audio_RDWR);
ATF_TC_HEAD(open_audio_RDWR, tc)
{
	atf_tc_set_md_var(tc, "descr", "initial parameters on /dev/audio");
}
ATF_TC_BODY(open_audio_RDWR, tc)
{
	test_open_audio(O_RDWR);
}


/*
 * Check /dev/sound's initial parameters.
 * Some parameters are sticky.
 * XXX cannot test half duplex.
 */
static void
test_open_sound(int mode)
{
	struct audio_info ai;
	struct audio_info ai0;
	struct audio_info exp;
	int padmode;
	int padfd;
	int fd;
	int r;

	switch (mode) {
	case O_RDONLY:
		padmode = O_WRONLY;
		atf_tc_skip("pad does not support this mode");
		break;
	case O_WRONLY:
		padmode = O_RDONLY;
		break;
	case O_RDWR:
		padmode = O_RDWR;
		atf_tc_skip("pad does not support this mode");
		break;
	}

	rump_init();
	padfd = rump_sys_open("/dev/pad0", padmode);
	ATF_REQUIRE(padfd >= 0);

	fd = rump_sys_open("/dev/sound0", mode);
	ATF_REQUIRE(fd >= 0);

	memset(&ai, 0, sizeof(ai));
	r = rump_sys_ioctl(fd, AUDIO_GETBUFINFO, &ai);
	ATF_REQUIRE(0 == r);

	AUDIO_INITINFO(&exp);
	exp.play.sample_rate = 8000;
	exp.play.channels = 1;
	exp.play.precision = 8;
	exp.play.encoding = AUDIO_ENCODING_ULAW;
	exp.play.seek = 0;
	exp.play.samples = 0;
	exp.play.eof = 0;
	exp.play.pause = 0;
	exp.play.error = 0;
	exp.play.waiting = 0;
	exp.play.open = mode2popen(mode);
	exp.play.active = 0;
	exp.record.sample_rate = 8000;
	exp.record.channels = 1;
	exp.record.precision = 8;
	exp.record.encoding = AUDIO_ENCODING_ULAW;
	exp.record.seek = 0;
	exp.record.samples = 0;
	exp.record.eof = 0;
	exp.record.pause = 0;
	exp.record.error = 0;
	exp.record.waiting = 0;
	exp.record.open = mode2ropen(mode);
	exp.record.active = 0;
	exp.mode = mode2aumode(mode);
	check_ai(&exp, &ai);

	/* Save it */
	ai0 = ai;

	/* Change as possible */
	AUDIO_INITINFO(&ai);
	ai.blocksize = ai0.blocksize * 2;
	if (ai0.hiwat > 0)
		ai.hiwat = ai0.hiwat - 1;
	if (ai0.lowat < ai0.hiwat)
		ai.lowat = ai0.lowat + 1;
	ai.mode = ai0.mode ^ AUMODE_PLAY_ALL;
	ai.play.sample_rate = 11025;
	ai.play.channels = 1;
	ai.play.precision = 16;
	ai.play.encoding = AUDIO_ENCODING_SLINEAR_LE;
	ai.play.pause = 1;
	ai.record.sample_rate = 11025;
	ai.record.channels = 1;
	ai.record.precision = 16;
	ai.record.encoding = AUDIO_ENCODING_SLINEAR_LE;
	ai.record.pause = 1;
	RL(rump_sys_ioctl(fd, AUDIO_SETINFO, &ai));
	RL(rump_sys_ioctl(fd, AUDIO_GETBUFINFO, &ai0));
	/* Then close. */
	RL(rump_sys_close(fd));

	/*
	 * Open and check again.
	 */
	fd = rump_sys_open("/dev/sound0", mode);
	ATF_REQUIRE(fd >= 0);

	memset(&ai, 0, sizeof(ai));
	RL(rump_sys_ioctl(fd, AUDIO_GETBUFINFO, &ai));

	AUDIO_INITINFO(&exp);
	exp.play.sample_rate = 11025;
	exp.play.channels = 1;
	exp.play.precision = 16;
	exp.play.encoding = AUDIO_ENCODING_SLINEAR_LE;
	exp.play.seek = 0;
	exp.play.samples = 0;
	exp.play.eof = 0;
	exp.play.pause = 1;	/* sticky */
	exp.play.error = 0;
	exp.play.waiting = 0;
	exp.play.open = mode2popen(mode);
	exp.play.active = 0;
	exp.record.sample_rate = 11025;
	exp.record.channels = 1;
	exp.record.precision = 16;
	exp.record.encoding = AUDIO_ENCODING_SLINEAR_LE;
	exp.record.seek = 0;
	exp.record.samples = 0;
	exp.record.eof = 0;
	exp.record.pause = 1;	/* sticky */
	exp.record.error = 0;
	exp.record.waiting = 0;
	exp.record.open = mode2ropen(mode);
	exp.record.active = 0;
	exp.mode = mode2aumode(mode);
	check_ai(&exp, &ai);

	RL(rump_sys_close(fd));
	RL(rump_sys_close(padfd));
}

ATF_TC(open_sound_RDONLY);
ATF_TC_HEAD(open_sound_RDONLY, tc)
{
	atf_tc_set_md_var(tc, "descr", "initial parameters on /dev/sound");
}
ATF_TC_BODY(open_sound_RDONLY, tc)
{
	test_open_sound(O_RDONLY);
}

ATF_TC(open_sound_WRONLY);
ATF_TC_HEAD(open_sound_WRONLY, tc)
{
	atf_tc_set_md_var(tc, "descr", "initial parameters on /dev/sound");
}
ATF_TC_BODY(open_sound_WRONLY, tc)
{
	test_open_sound(O_WRONLY);
}

ATF_TC(open_sound_RDWR);
ATF_TC_HEAD(open_sound_RDWR, tc)
{
	atf_tc_set_md_var(tc, "descr", "initial parameters on /dev/sound");
}
ATF_TC_BODY(open_sound_RDWR, tc)
{
	test_open_sound(O_RDWR);
}

/*
 * Open /dev/sound -> /dev/audio -> /dev/sound.
 * 2nd /dev/sound affected by /dev/audio.
 * /dev/sound and /dev/audio are not independent in the internal, and
 * there is only one sticky parameter per device.
 */
ATF_TC(open_audio_sound);
ATF_TC_HEAD(open_audio_sound, tc)
{
	atf_tc_set_md_var(tc, "descr", "sticky parameter and /dev/audio");
}
ATF_TC_BODY(open_audio_sound, tc)
{
	struct audio_info ai;
	struct audio_info ai0;
	struct audio_info exp;
	int padfd;
	int fd;
	int r;

	rump_init();
	padfd = rump_sys_open("/dev/pad0", O_RDONLY);
	ATF_REQUIRE(padfd >= 0);

	/*
	 * To simplify, check play.encoding only here.
	 */

	/* At first, open /dev/sound and set */
	fd = rump_sys_open("/dev/sound0", O_WRONLY);
	ATF_REQUIRE(fd >= 0);
	AUDIO_INITINFO(&ai);
	ai.play.encoding = AUDIO_ENCODING_SLINEAR_LE;
	RL(rump_sys_ioctl(fd, AUDIO_SETINFO, &ai));
	RL(rump_sys_close(fd));

	/* At second, open /dev/audio.  It sets mulaw automatically */
	fd = rump_sys_open("/dev/audio0", O_WRONLY);
	ATF_REQUIRE(fd >= 0);
	RL(rump_sys_close(fd));

	/* And then, open /dev/sound again */
	fd = rump_sys_open("/dev/sound0", O_WRONLY);
	ATF_REQUIRE(fd >= 0);
	RL(rump_sys_ioctl(fd, AUDIO_GETBUFINFO, &ai));
	ATF_CHECK_EQ(AUDIO_ENCODING_ULAW, ai.play.encoding);

	RL(rump_sys_close(fd));
	RL(rump_sys_close(padfd));
}

/*
 * Open multiple descriptors and check its mode.
 */
ATF_TC(open_multifd);
ATF_TC_HEAD(open_multifd, tc)
{
	atf_tc_set_md_var(tc, "descr", "multiple open");
}
ATF_TC_BODY(open_multifd, tc)
{
	struct audio_info ai;
	int padfd;
	int fd1, fd2;
	int i;
	int r;
	int actmode;
#define AUMODE_BOTH (AUMODE_PLAY | AUMODE_RECORD)
	struct {
		int mode1;
		int mode2;
	} expfulltable[] = {
		/* fd0's expected	fd1's expected or errno */
		{ AUMODE_RECORD,	AUMODE_RECORD },	// REC, REC
		{ AUMODE_RECORD,	AUMODE_PLAY },		// REC, PLAY
		{ AUMODE_RECORD,	AUMODE_BOTH },		// REC, BOTH
		{ AUMODE_PLAY,		AUMODE_RECORD },	// PLAY, REC
		{ AUMODE_PLAY,		AUMODE_PLAY },		// PLAY, PLAY
		{ AUMODE_PLAY,		AUMODE_BOTH },		// PLAY, BOTH
		{ AUMODE_BOTH,		AUMODE_RECORD },	// BOTH, REC
		{ AUMODE_BOTH,		AUMODE_PLAY },		// BOTH, PLAY
		{ AUMODE_BOTH,		AUMODE_BOTH },		// BOTH, BOTH
	},
	exphalftable[] = {
		/* fd0's expected	fd1's expected or errno */
		{ AUMODE_RECORD,	AUMODE_RECORD },	// REC, REC
		{ AUMODE_RECORD,	-ENODEV },		// REC, PLAY
		{ AUMODE_RECORD,	-ENODEV },		// REC, BOTH
		{ AUMODE_PLAY,		-ENODEV },		// PLAY, REC
		{ AUMODE_PLAY,		AUMODE_PLAY },		// PLAY, PLAY
		{ AUMODE_PLAY,		AUMODE_PLAY },		// PLAY, BOTH
		{ AUMODE_PLAY,		-ENODEV },		// BOTH, REC
		{ AUMODE_PLAY,		AUMODE_PLAY },		// BOTH, PLAY
		{ AUMODE_PLAY,		AUMODE_PLAY },		// BOTH, BOTH
	}, *exptable;

	/* XXX cannot test half duplex */
	if (hwfull) {
		exptable = expfulltable;
	} else {
		exptable = exphalftable;
	}

	rump_init();
	padfd = rump_sys_open("/dev/pad0", O_RDONLY);
	ATF_REQUIRE(padfd >= 0);

	for (i = 0; i < 9; i++) {
		int mode1 = i / 3;
		int mode2 = i % 3;
		char desc[64];

		/* XXX pad only supports O_WRONLY... */
		if (mode1 != O_WRONLY || mode2 != O_WRONLY)
			continue;

		snprintf(desc, sizeof(desc), "%s,%s",
		    openmodetable[mode1], openmodetable[mode2]);

		/* 1st fd */
		fd1 = rump_sys_open("/dev/audio0", mode1);
		ATF_REQUIRE(fd1 >= 0);
		RL(rump_sys_ioctl(fd1, AUDIO_GETBUFINFO, &ai));
		actmode = ai.mode & (AUMODE_PLAY | AUMODE_RECORD);
		ATF_CHECK_EQ_MSG(exptable[i].mode1, actmode, "%s", desc);

		/* 2nd fd */
		fd2 = rump_sys_open("/dev/audio0", mode2);
		if (exptable[i].mode2 >= 0) {
			/* expected to be able to open */
			ATF_CHECK_MSG(fd2 >= 0, "fd >= 0 not met: %s", desc);
			r = rump_sys_ioctl(fd2, AUDIO_GETBUFINFO, &ai);
			ATF_CHECK_EQ_MSG(0, r, "%s", desc);
			if (r == 0) {
				actmode = ai.mode & (AUMODE_PLAY|AUMODE_RECORD);
				ATF_CHECK_EQ_MSG(exptable[i].mode2, actmode,
				    "%s", desc);
			}
		} else {
			/* expected not to be able to open */
			ATF_CHECK_EQ(-1, fd2);
			if (fd2 == -1) {
				ATF_CHECK_EQ_MSG(-exptable[i].mode2, errno,
				    "%s", desc);
			}
		}
		if (fd2 >= 0)
			RL(rump_sys_close(fd2));
		RL(rump_sys_close(fd1));
	}
}


ATF_TP_ADD_TCS(tp)
{
	ATF_TP_ADD_TC(tp, open_audio_RDONLY);
	ATF_TP_ADD_TC(tp, open_audio_WRONLY);
	ATF_TP_ADD_TC(tp, open_audio_RDWR);

	ATF_TP_ADD_TC(tp, open_sound_RDONLY);
	ATF_TP_ADD_TC(tp, open_sound_WRONLY);
	ATF_TP_ADD_TC(tp, open_sound_RDWR);

	ATF_TP_ADD_TC(tp, open_audio_sound);
	ATF_TP_ADD_TC(tp, open_multifd);

	return atf_no_error();
}
