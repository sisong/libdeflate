/*
 * do_compress_by_stream_mt.h
 * added compression by stream & muti-thread parallel, 2023 housisong
 */
#ifndef PROGRAMS_PROG_COMPRESS_STREAM_MT_H
#define PROGRAMS_PROG_COMPRESS_STREAM_MT_H
#ifdef __cplusplus
extern "C" {
#endif
#include "prog_util.h"

static const size_t kLibDefBlockSize = 1024*1024*1;
static const size_t kLibDefBlockSize_max = 1024*1024*128; //for decompressor


int do_compress_by_stream_mt(int compression_level,struct file_stream *in,u64 in_size,
                            struct file_stream *out,int thread_num);

#ifdef __cplusplus
}
#endif
#endif /* PROGRAMS_PROG_COMPRESS_STREAM_MT_H */
