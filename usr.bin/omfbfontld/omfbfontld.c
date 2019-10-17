/*
 * TODO: BSD tag, etc..
 *
 * OMRON LUNA Font loader
 */

#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dev/wscons/wsconsio.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>

static void
usage(void)
{
	printf("%s: [-dfo] luna-font-file\n", getprogname());
	printf(
" LUNA EUC font file loader\n"
"  -d: device\n"
"  -f: force if warn\n"
"  -n: not write\n"
"  -o: x offset (<=1280, default 1280, mod 8)\n"
"  -t: test\n"
	);
	exit(1);
}

int
main(int ac, char *av[])
{
	const char *file;
	const char *fbdev;
	int filefd, fbfd;
	struct stat filestat;
	struct wsdisplay_fbinfo fbinfo;
	off_t filelen;
	uint32_t fblen;
	uint8_t *filem;
	uint8_t *fb;
	uint8_t *src, *dst;
	int bpp;
	int mode;

	int c;
	int opt_force = 0;
	int opt_n = 0;

	int fontoffset = 1280 / 8;
	const int stride = 2048 / 8;
	const int w = 768 / 8;

	fbdev = "/dev/ttyE0";

	while ((c = getopt(ac, av, "d:fno:t")) != -1) {
		switch (c) {
		 case 'd':
			fbdev = optarg;
			break;
		 case 'f':
			opt_force = 1;
			break;
		 case 'n':
			opt_n = 1;
			break;
		 case 'o':
			fontoffset = strtoul(optarg, NULL, 10) / 8;
			if (fontoffset < 0 || fontoffset > 1280 / 8) {
				usage();
			}
			break;
		 case 't':
			for (int h = 0x8e; h < 0xfe; h++) {
				for (int l = 0xa1; l <= 0xfe; l++) {
					printf("%c%c", h, l);
				}
				if (h == 0x8e) h = 0xa1;
			}
			printf("\n");
			return 0;
			
		 default:
			usage();
		}
	}
	ac -= optind;
	av += optind;

	if (ac < 1)
		usage();

	printf("x offset: %d\n", fontoffset * 8);

	file = av[0];
	printf("font file: %s\n", file);

	filefd = open(file, O_RDONLY);
	if (filefd == -1) {
		err(EXIT_FAILURE, "open: %s", file);
	}

	if (fstat(filefd, &filestat)) {
		err(EXIT_FAILURE, "fstat");
	}

	filelen = filestat.st_size;

	if (filelen == w * 1024) {
		bpp = 1;
	} else if (filelen == w * 4096) {
		bpp = 4;
	} else {
		errx(1, "Invalid file length. (must be 96 or 384kiB)");
	}
	printf("font file type: %dbpp\n", bpp);

	filem = mmap(NULL, filelen, PROT_READ, MAP_SHARED, filefd, 0);
	if (filem == MAP_FAILED) {
		err(EXIT_FAILURE, "mmap");
	}

	fbfd = open(fbdev, O_RDWR);
	if (fbfd == -1) {
		err(EXIT_FAILURE, "open framebuffer");
	}

	if (ioctl(fbfd, WSDISPLAYIO_GINFO, &fbinfo)) {
		err(EXIT_FAILURE, "WSDISPLAYIO_GINFO");
	}
	printf("fbinfo.width=%d\n", fbinfo.width);
	printf("fbinfo.height=%d\n", fbinfo.height);
	printf("fbinfo.depth=%d\n", fbinfo.depth);

	if (fbinfo.depth != bpp) {
		printf("file %dbpp != fb.depth %dbpp\n", bpp, fbinfo.depth);
		if (!opt_force) {
			exit(1);
		}
	}

	printf("Now loading...\n");

	fblen = 2048 * 1024 * fbinfo.depth / 8;
	//fblen -= 8;

	mode = WSDISPLAYIO_MODE_DUMBFB;
	// ここからグラフィックモード
	// printf できないっぽい
	if (ioctl(fbfd, WSDISPLAYIO_SMODE, &mode)) {
		err(EXIT_FAILURE, "WSDISPLAYIO_SMODE DUMBFB");
	}
	fb = mmap(NULL, fblen, PROT_WRITE, MAP_SHARED, fbfd, 0);

	src = filem;
	dst = fb + fontoffset;

	if (0) {
		// デバッグ
		volatile uint8_t t;
		printf("read\n");
		t = *src;
		printf("read OK\n");
		printf("write\n");
		*(uint32_t *)dst = t;
		printf("write OK\n");
	}

	for (int y = 0; y < 1024 * bpp; y++) {
		printf("copy from %d(%p) to %p\n", y, src, dst);
		if (!opt_n)
			memcpy(dst, src, w);
		dst += stride;
		src += w;
	}

	// テキストモードに戻す
	mode = WSDISPLAYIO_MODE_EMUL;
	if (ioctl(fbfd, WSDISPLAYIO_SMODE, &mode)) {
		err(EXIT_FAILURE, "WSDISPLAYIO_SMODE EMUL");
	}
	if (fb == MAP_FAILED) {
		err(EXIT_FAILURE, "mmap fbfd");
	}


	munmap(fb, fblen);
	munmap(filem, filelen);

	close(fbfd);
	close(filefd);

	if (1) {
		char s[] = 
"LUNA\x20\xC6\xFC\xCB\xDC\xB8\xEC\xA5\xD5\xA5\xA9\xA5\xF3\xA5\xC8\xA5\xD5\xA5\xA1\xA5\xA4\xA5\xEB\xA4\xAC\xA5\xED\xA1\xBC\xA5\xC9\xA4\xB5\xA4\xEC\xA4\xDE\xA4\xB7\xA4\xBF\xA1\xA3";

		printf("%s\n", s);
	}

	return 0;
}

