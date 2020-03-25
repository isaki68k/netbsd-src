/*	$NetBSD: parse.c,v 1.8 2016/11/25 17:37:04 tsutsui Exp $	*/

/*
 * Copyright (c) 1992 OMRON Corporation.
 *
 * This code is derived from software contributed to Berkeley by
 * OMRON Corporation.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. All advertising materials mentioning features or use of this software
 *    must display the following acknowledgement:
 *	This product includes software developed by the University of
 *	California, Berkeley and its contributors.
 * 4. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 *	@(#)parse.c	8.1 (Berkeley) 6/10/93
 */
/*
 * Copyright (c) 1992, 1993
 *	The Regents of the University of California.  All rights reserved.
 *
 * This code is derived from software contributed to Berkeley by
 * OMRON Corporation.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 *	@(#)parse.c	8.1 (Berkeley) 6/10/93
 */

/*
 * parse.c -- command parser
 * by A.Fujita, JAN-30-1992
 */

#include <lib/libkern/libkern.h>
#include <luna68k/stand/boot/samachdep.h>
#include <luna68k/stand/boot/status.h>

static int cmd_help(int, char *[]);

int
check_args(int argc, char *argv[])
{
	int i;

	for (i = 0; i < argc; i++)
		printf("argv[%d] = \"%s\"\n", i, argv[i]);

	return ST_NORMAL;
}

int
exit_program(int argc, char *argv[])
{

	return ST_EXIT;
}

static const char helpmsg[] =
	"commands are:\n"
	"boot [device(unit,part)filename] [-ads]\n"
	" (ex. \"boot sd(6,0)netbsd\", \"boot le()netbsd.old\" etc.)\n"
	"  Note unit number for SCSI device is (ctlr) * 10 + (id).\n"
	"ls [device(unit, part)[path]]\n"
	" (ex. \"ls sd(0,0)/bin\")\n"
	"help\n"
	"exit\n"
#if 0 /* debug commands */
	"checkargs\n"
	"disklabel\n"
	"howto\n"
	"screen\n"
	"scsi\n"
#endif
;

static int
cmd_help(int argc, char *argv[])
{

	printf(helpmsg);
	return ST_NORMAL;
}


#if 1
/*
 * read/write test
 */
extern uint32_t read_1(uint32_t);
extern uint32_t read_2(uint32_t);
extern uint32_t read_4(uint32_t);
extern uint32_t write_1(uint32_t, uint32_t);
extern uint32_t write_2(uint32_t, uint32_t);
extern uint32_t write_4(uint32_t, uint32_t);
uint32_t hex2bin(const char *, char **);
int cmd_r(int, char*[]);
int cmd_w(int, char*[]);

uint32_t
hex2bin(const char *s, char **end)
{
	uint32_t rv;

	rv = 0;
	for (; *s; s++) {
		char ch = *s;
		if ('0' <= ch && ch <= '9') {
			rv = rv * 16 + (ch - '0');
			continue;
		}
		ch |= 0x20;
		if ('a' <= ch && ch <= 'f') {
			rv = rv * 16 + (ch - 'a' + 10);
			continue;
		}
		/* If not hex character, so break */
		break;
	}
	if (end) {
		*end = __UNCONST(s);
	}
	return rv;
}

uint32_t isbuserr;

int
cmd_r(int argc, char *argv[])
{
	char *end;
	uint32_t addr;
	uint32_t data;
	int size;
	int lines;
	int i, j;

	if (argc != 2 && argc != 3) {
		printf("usage: r <hex-address>[.<size>] [<lines>]\n");
		printf("       <size> := l,w,b (default:l)\n");
		return ST_NORMAL;
	}

	addr = hex2bin(argv[1], &end);
	size = 4;
	if (*end == '.') {
		char s = *++end;
		s |= 0x20;
		if (s == 'b') {
			size = 1;
		} else if (s == 'w') {
			size = 2;
		} else if (s == 'l') {
			size = 4;
		}
		/* no error check */
	}

	lines = 1;
	if (argc == 3) {
		lines = atoi(argv[2]);
	}

	for (j = 0; j < lines; j++) {
		printf("%08x:", addr);
		switch (size) {
		 case 1:
			for (i = 0; i < 16; i++) {
				isbuserr = 0;
				data = read_1(addr + i);
				if (isbuserr)
					printf(" --");
				else
					printf(" %02x", data);

				if (i == 7)
					printf(" ");
			}
			break;
		 case 2:
			for (i = 0; i < 16; i += 2) {
				isbuserr = 0;
				data = read_2(addr + i);
				if (isbuserr)
					printf(" ----");
				else
					printf(" %04x", data);
			}
			break;
		 case 4:
			for (i = 0; i < 16; i += 4) {
				isbuserr = 0;
				data = read_4(addr + i);
				if (isbuserr)
					printf(" --------");
				else
					printf(" %08x", data);
			}
			break;
		}
		printf("\n");
		addr += 16;
	}

	return ST_NORMAL;
}

int
cmd_w(int argc, char *argv[])
{
	char *end;
	uint32_t addr;
	uint32_t data;
	int size;

	if (argc != 3) {
		printf("usage: w <hex-address>[.<size>] <hex-data>\n");
		printf("       <size> := l,w,b (default:l)\n");
		return ST_NORMAL;
	}

	addr = hex2bin(argv[1], &end);
	size = 4;
	if (*end == '.') {
		char s = *++end;
		s |= 0x20;
		if (s == 'b') {
			size = 1;
		} else if (s == 'w') {
			size = 2;
		} else if (s == 'l') {
			size = 4;
		}
		/* no error check */
	}

	data = hex2bin(argv[2], NULL);

	isbuserr = 0;
	switch (size) {
	 case 1:
		write_1(addr, data);
	 case 2:
		write_2(addr, data);
	 case 4:
		write_4(addr, data);
	}
	if (isbuserr)
		printf("Bus Error\n");

	return ST_NORMAL;
}

#endif /* readwrite test */

struct command_entry {
	char *name;
	int (*func)(int, char **);
};

static const struct command_entry entries[] = {
	{ "b",		boot         },
	{ "boot",	boot         },
	{ "chkargs",	check_args   },
	{ "disklabel",	disklabel    },
	{ "exit",	exit_program },
#ifdef notyet
	{ "fsdump",	fsdump       },
	{ "fsrestore",	fsrestore    },
#endif
	{ "help",	cmd_help     },
	{ "ls",		cmd_ls       },
	{ "screen",	screen	     },
#ifdef notyet
	{ "tape",	tape	     },
	{ "tp",		tape	     },
#endif
	{ "scsi",	scsi         },
	{ "quit",	exit_program },

	{ "r",		cmd_r        },
	{ "w",		cmd_w        },
	{ NULL, NULL }
};


int
parse(int argc, char *argv[])
{
	int i, status = ST_NOTFOUND;

	for (i = 0; entries[i].name != NULL; i++) {
		if (!strcmp(argv[0], entries[i].name)) {
			status = (*entries[i].func)(argc, argv);
			break;
		}
	}

	return status;
}



/*
 * getargs -- make argument arrays
 */

int
getargs(char buffer[], char *argv[], int maxargs)
{
	int n = 0;
	char *p = buffer;

	argv[n++] = p;
	while (*p != '\0') {
		if (*p == ' ') {
			*p = '\0';
		} else if (p != buffer && *(p - 1) == '\0') {
			if (n < maxargs)
				argv[n++] = p;
		}
		p++;
	}

	return n;
}
