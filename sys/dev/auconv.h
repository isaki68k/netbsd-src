/*	$NetBSD: auconv.h,v 1.21 2017/12/16 16:09:36 nat Exp $	*/

/*-
 * Copyright (c) 1997 The NetBSD Foundation, Inc.
 * All rights reserved.
 *
 * This code is derived from software contributed to The NetBSD Foundation
 * by Lennart Augustsson.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE NETBSD FOUNDATION, INC. AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE FOUNDATION OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef _SYS_DEV_AUCONV_H_
#define _SYS_DEV_AUCONV_H_
#include <dev/audio_if.h>

/* common routines for stream_filter_t */
extern void stream_filter_set_fetcher(stream_filter_t *, stream_fetcher_t *);
extern void stream_filter_set_inputbuffer(stream_filter_t *, audio_stream_t *);
extern stream_filter_t *auconv_nocontext_filter_factory
	(int (*)(struct audio_softc *, stream_fetcher_t *, audio_stream_t *, int));
extern void auconv_nocontext_filter_dtor(struct stream_filter *);
#define FILTER_LOOP_PROLOGUE(SRC, SRCFRAME, DST, DSTFRAME, MAXUSED) \
do { \
	const uint8_t *s; \
	uint8_t *d; \
	s = (SRC)->outp; \
	d = (DST)->inp; \
	for (; audio_stream_get_used(DST) < MAXUSED \
		&& audio_stream_get_used(SRC) >= SRCFRAME; \
		s = audio_stream_add_outp(SRC, s, SRCFRAME), \
		d = audio_stream_add_inp(DST, d, DSTFRAME))
#define FILTER_LOOP_EPILOGUE(SRC, DST)	\
	(SRC)->outp = s; \
	(DST)->inp = d; \
} while (/*CONSTCOND*/0)


/* Convert between signed and unsigned. */
extern stream_filter_factory_t change_sign8;
extern stream_filter_factory_t change_sign16;
/* Convert between little and big endian. */
extern stream_filter_factory_t swap_bytes;
extern stream_filter_factory_t swap_bytes_change_sign16;
/* Byte expansion/contraction */
extern stream_filter_factory_t linear32_32_to_linear32;
extern stream_filter_factory_t linear32_32_to_linear24;
extern stream_filter_factory_t linear32_32_to_linear16;
extern stream_filter_factory_t linear24_24_to_linear32;
extern stream_filter_factory_t linear24_24_to_linear24;
extern stream_filter_factory_t linear24_24_to_linear16;
extern stream_filter_factory_t linear16_16_to_linear32;
extern stream_filter_factory_t linear16_16_to_linear24;
extern stream_filter_factory_t linear16_16_to_linear16;
extern stream_filter_factory_t linear8_8_to_linear32;
extern stream_filter_factory_t linear8_8_to_linear24;
extern stream_filter_factory_t linear8_8_to_linear16;
extern stream_filter_factory_t linearN_to_linear8;
extern stream_filter_factory_t null_filter;

#define linear32_32_to_linear8 linearN_to_linear8
#define linear24_24_to_linear8 linearN_to_linear8
#define linear16_16_to_linear8 linearN_to_linear8
#define linear8_8_to_linear8 linearN_to_linear8
#define linear16_to_linear8 linearN_to_linear8
#define linear8_to_linear16 linear8_8_to_linear16

/* sampling rate conversion (aurateconv.c) */
extern stream_filter_factory_t aurateconv;

struct audio_encoding_set;
extern int auconv_set_converter(const struct audio_format *, int,
				int, const audio_params_t *, int,
				stream_filter_list_t *);
extern int auconv_create_encodings(const struct audio_format *, int,
				   struct audio_encoding_set **);
extern int auconv_delete_encodings(struct audio_encoding_set *);
extern int auconv_query_encoding(const struct audio_encoding_set *,
				 audio_encoding_t *);

#endif /* !_SYS_DEV_AUCONV_H_ */
