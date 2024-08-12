/*
 * gzip_decompress_by_stream_mt.h
 * added decompress by stream & multi-thread, 2023--2024 housisong
 */
#ifndef PROGRAMS_PROG_GZIP_DECOMPRESS_STREAM_H
#define PROGRAMS_PROG_GZIP_DECOMPRESS_STREAM_H
#ifdef __cplusplus
extern "C" {
#endif
#include "prog_util.h"

enum libdeflate_destream_result{
	LIBDEFLATE_DESTREAM_MEM_ALLOC_ERROR =20,
	LIBDEFLATE_DESTREAM_READ_FILE_ERROR,
	LIBDEFLATE_DESTREAM_WRITE_FILE_ERROR,
	LIBDEFLATE_DESTREAM_DATA_CRC_ERROR,
	LIBDEFLATE_DESTREAM_DATA_SIZE_ERROR,
	
	LIBDEFLATE_DESTREAM_MT_READ_FILE_ERROR, //25
	LIBDEFLATE_DESTREAM_MT_WRITE_FILE_ERROR,
	LIBDEFLATE_DESTREAM_MT_OUT_LACK_ERROR,
	LIBDEFLATE_DESTREAM_MT_EXCEPTION_ERROR, 
	LIBDEFLATE_DESTREAM_MT_READ_THREAD_EXCEPTION_ERROR,
	LIBDEFLATE_DESTREAM_MT_WRITE_THREAD_EXCEPTION_ERROR,//30
};

//decompress one gzip by stream
//  'out','actual_in_nbytes_ret','actual_out_nbytes_ret' can NULL;
//	return value is libdeflate_result or libdeflate_destream_result.
int gzip_decompress_by_stream(struct libdeflate_decompressor *decompressor,
							  struct file_stream *in, u64 in_size, struct file_stream *out,
							  u64* actual_in_nbytes_ret,u64* actual_out_nbytes_ret);

//decompress one gzip by stream & multi-thread
int gzip_decompress_by_stream_mt(struct libdeflate_decompressor *decompressor,
							     struct file_stream *in, u64 in_size, struct file_stream *out,
							     int thread_num,u64* actual_in_nbytes_ret,u64* actual_out_nbytes_ret);


//decompress multi concatenated gzip by stream
int gzips_decompress_by_stream(struct libdeflate_decompressor *decompressor,
							   struct file_stream *in, u64 in_size, struct file_stream *out,
							   u64* actual_out_nbytes_ret);

//decompress multi concatenated gzip by stream & multi-thread
int gzips_decompress_by_stream_mt(struct libdeflate_decompressor *decompressor,
							      struct file_stream *in, u64 in_size, struct file_stream *out,
							      int thread_num,u64* actual_out_nbytes_ret);

#ifdef __cplusplus
}
#endif
#endif /* PROGRAMS_PROG_GZIP_DECOMPRESS_STREAM_H */
