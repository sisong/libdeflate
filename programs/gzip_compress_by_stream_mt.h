/*
 * gzip_compress_by_stream_mt.h
 * added compress by stream & multi-thread parallel, 2023--2024 housisong
 */
#ifndef PROGRAMS_PROG_GZIP_COMPRESS_STREAM_H
#define PROGRAMS_PROG_GZIP_COMPRESS_STREAM_H
#ifdef __cplusplus
extern "C" {
#endif
#include "prog_util.h"

enum libdeflate_enstream_result{
	LIBDEFLATE_ENSTREAM_SUCCESS = LIBDEFLATE_SUCCESS,
	LIBDEFLATE_ENSTREAM_MEM_ALLOC_ERROR =30,
	LIBDEFLATE_ENSTREAM_READ_FILE_ERROR,
	LIBDEFLATE_ENSTREAM_WRITE_FILE_ERROR,
    LIBDEFLATE_ENSTREAM_ALLOC_COMPRESSOR_ERROR,
    LIBDEFLATE_ENSTREAM_GZIP_HEAD_ERROR,
    LIBDEFLATE_ENSTREAM_GZIP_FOOT_ERROR,   //35
    LIBDEFLATE_ENSTREAM_COMPRESS_BLOCK_ERROR,
    LIBDEFLATE_ENSTREAM_MT_READ_FILE_ERROR,
    LIBDEFLATE_ENSTREAM_MT_WRITE_FILE_ERROR,
    LIBDEFLATE_ENSTREAM_MT_COMPRESS_BLOCK_ERROR,
    LIBDEFLATE_ENSTREAM_MT_OUT_LACK_ERROR, //40
    LIBDEFLATE_ENSTREAM_MT_EXCEPTION_ERROR,
    LIBDEFLATE_ENSTREAM_MT_THREAD_EXCEPTION_ERROR,
};

//compress gzip by stream
//  actual_out_nbytes_ret can NULL;
//	return value is libdeflate_enstream_result.
int gzip_compress_by_stream(int compression_level,struct file_stream *in,u64 in_size,
                            struct file_stream *out,u64* actual_out_nbytes_ret);

//compress gzip by stream & multi-thread
int gzip_compress_by_stream_mt(int compression_level,struct file_stream *in,u64 in_size,
                               struct file_stream *out,int thread_num,u64* actual_out_nbytes_ret);


#ifdef __cplusplus
}
#endif
#endif /* PROGRAMS_PROG_GZIP_COMPRESS_STREAM_H */
