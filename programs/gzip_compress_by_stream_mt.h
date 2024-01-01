/*
 * gzip_compress_by_stream_mt.h
 * added compress by stream & muti-thread parallel, 2023 housisong
 */
#ifndef PROGRAMS_PROG_GZIP_COMPRESS_STREAM_H
#define PROGRAMS_PROG_GZIP_COMPRESS_STREAM_H
#ifdef __cplusplus
extern "C" {
#endif
#include "prog_util.h"

static const size_t kCompressSteamStepSize = (size_t)1024*1024*2;

//compress gzip by stream & muti-thread;
int gzip_compress_by_stream_mt(int compression_level,struct file_stream *in,u64 in_size,
                            struct file_stream *out,int thread_num);

#ifdef __cplusplus
}
#endif
#endif /* PROGRAMS_PROG_GZIP_COMPRESS_STREAM_H */
