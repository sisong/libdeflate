/*
 * do_compress_by_stream_mt.cpp
 * added compression by stream & muti-thread parallel, 2023 housisong
 */
#ifdef __cplusplus
extern "C" {
#endif
#include "../lib/deflate_compress.h" //libdeflate_get_compression_level
#ifdef __cplusplus
}
#endif
#include "do_compress_by_stream_mt.h"
static const size_t kDictSize  = (1<<15); //MATCHFINDER_WINDOW_SIZE
static const size_t kBlockSize = 1024*1024*1;

#define _check(v,_ret_errCode) do { if (!(v)) {resultCode=_ret_errCode; goto _out; } } while (0)

static inline size_t _dictSize_avail(u64 in_read_pos) { return (in_read_pos<kDictSize)?in_read_pos:kDictSize; }

int do_compress_by_stream_mt(struct libdeflate_compressor *c,
                             struct file_stream *in,u64 in_size,struct file_stream *out,int thread_num){
    if (in_size==0) return 0; //ok
    int resultCode=0;
    u8* pmem=0;
    thread_num=(thread_num<=1)?1:thread_num;
    const int    is_byte_align = 0;
    uint32_t     in_crc=0;
    const size_t block_bound=libdeflate_deflate_compress_bound_block(kBlockSize);
    const size_t one_buf_size=(kDictSize+kBlockSize+block_bound+4096-1)/4096*4096;
    pmem=(u8*)malloc(one_buf_size*thread_num);
    _check(pmem, 21);

    {//gizp head
        size_t codeSize=libdeflate_gzip_compress_head(libdeflate_get_compression_level(c),in_size,pmem,block_bound);
        _check(codeSize>0, 22);
        int w_ret=full_write(out,pmem,codeSize);
        _check(w_ret==0, 23);
    }

    if (1){ //(thread_num<=1){ // single thread
        u8* pdata=pmem;
        u8* pcode=pdata+kDictSize+kBlockSize;

        for (u64 i=0;i<in_size;i+=kBlockSize){//compress by stream
            bool isEnd=(i+kBlockSize>=in_size);
            size_t in_len=isEnd?in_size-i:kBlockSize;
            size_t dictSize=_dictSize_avail(i);

            //read block data
            ssize_t r_len=xread(in,pdata+dictSize,in_len);
            _check(r_len==in_len, 11);
            in_crc=libdeflate_crc32(in_crc,pdata+dictSize,in_len);

            //compress the block
            size_t codeSize=libdeflate_deflate_compress_block(c,pdata,dictSize,in_len,isEnd,
                                                              pcode,block_bound,is_byte_align);
            _check(codeSize>0, 12);

            //write the block's code
            int w_ret=full_write(out,pcode,codeSize);
            _check(w_ret==0, 13);

            //dict data for next block
            size_t nextDictSize=_dictSize_avail(i+kBlockSize);
            memmove(pdata,pdata+dictSize+in_len-nextDictSize,nextDictSize);
        }
    }else{

    }
    
    {//gizp foot
        size_t codeSize=libdeflate_gzip_compress_foot(in_crc,in_size,pmem,block_bound);
        _check(codeSize>0, 24);
        int w_ret=full_write(out,pmem,codeSize);
        _check(w_ret==0, 25);
    }
_out:
    if (pmem) free(pmem);
    return resultCode;
}
