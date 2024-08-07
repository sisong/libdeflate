/*
 * gzip_compress.c - compress with a gzip wrapper
 *
 * Copyright 2016 Eric Biggers
 *
 * Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software and associated documentation
 * files (the "Software"), to deal in the Software without
 * restriction, including without limitation the rights to use,
 * copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following
 * conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
 * OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
 * HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 * WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

#include "deflate_compress.h"
#include "gzip_constants.h"
#include "gzip_overhead.h"

size_t libdeflate_gzip_compress_head(unsigned compression_level,size_t in_nbytes,
			 void *out, size_t out_nbytes_avail)
{
	u8 *out_next = out;
	u8 xfl;

	if (out_nbytes_avail <= GZIP_MIN_OVERHEAD)
		return 0;

	/* ID1 */
	*out_next++ = GZIP_ID1;
	/* ID2 */
	*out_next++ = GZIP_ID2;
	/* CM */
	*out_next++ = GZIP_CM_DEFLATE;
	/* FLG */
	*out_next++ = 0;
	/* MTIME */
	put_unaligned_le32(GZIP_MTIME_UNAVAILABLE, out_next);
	out_next += 4;
	/* XFL */
	xfl = 0;
	if (compression_level < 2)
		xfl |= GZIP_XFL_FASTEST_COMPRESSION;
	else if (compression_level >= 8)
		xfl |= GZIP_XFL_SLOWEST_COMPRESSION;
	*out_next++ = xfl;
	/* OS */
	*out_next++ = GZIP_OS_UNKNOWN;	/* OS  */

	return out_next - (u8 *)out;
}

#define _do_compress_step(_call_compress) {	\
	deflate_size=_call_compress;	\
	if (deflate_size == 0)		\
		return 0;				\
	out_next += deflate_size;	\
	out_nbytes_avail -= deflate_size;	\
}

size_t libdeflate_gzip_compress(struct libdeflate_compressor *c,
			 const void *in, size_t in_nbytes,
			 void *out, size_t out_nbytes_avail)
{
	u8 *out_next = out;
	size_t deflate_size;

	_do_compress_step(libdeflate_gzip_compress_head(libdeflate_get_compression_level(c),
									in_nbytes,out_next,out_nbytes_avail));
	_do_compress_step(libdeflate_deflate_compress(c, in, in_nbytes, out_next,
									out_nbytes_avail));
	_do_compress_step(libdeflate_gzip_compress_foot(libdeflate_crc32(0, in, in_nbytes),
									in_nbytes,out_next,out_nbytes_avail));
	return out_next - (u8 *)out;
}

size_t libdeflate_gzip_compress_foot(uint32_t in_crc, size_t in_nbytes, void *out, size_t out_nbytes_avail)
{
	u8 *out_next = out;
	if (out_nbytes_avail <= GZIP_FOOTER_SIZE)
		return 0;

	/* CRC32 */
	put_unaligned_le32(in_crc, out_next);
	out_next += 4;

	/* ISIZE */
	put_unaligned_le32((u32)in_nbytes, out_next);
	out_next += 4;

	return out_next - (u8 *)out;
}

LIBDEFLATEAPI size_t
libdeflate_gzip_compress_bound(struct libdeflate_compressor *c,
			       size_t in_nbytes)
{
	return GZIP_MIN_OVERHEAD +
	       libdeflate_deflate_compress_bound(c, in_nbytes);
}
