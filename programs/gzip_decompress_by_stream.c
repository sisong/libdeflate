/*
 * gzip_decompress_by_stream.c
 * added decompress by stream, 2023 housisong
 */
#include "../lib/gzip_overhead.h"
#include "../lib/gzip_constants.h"
#include "gzip_decompress_by_stream.h"
#include <assert.h>

#define             kDictSize    (1<<15)  //MATCHFINDER_WINDOW_SIZE
static const size_t kMaxDeflateBlockSize_min =1024*4;
static const size_t kMaxDeflateBlockSize_max = ((~(size_t)0)-kDictSize)/4;
#define _check(v,_ret_errCode) do { if (!(v)) {err_code=_ret_errCode; goto _out; } } while (0)
#define _check_d(_d_ret) _check(_d_ret==LIBDEFLATE_SUCCESS, _d_ret)
static inline size_t _dictSize_avail(u64 uncompressed_pos) { 
                        return (uncompressed_pos<kDictSize)?(size_t)uncompressed_pos:kDictSize; }

#define _read_code_from_file() do{  \
    size_t read_len=code_cur;       \
    memmove(code_buf,code_buf+code_cur,code_buf_size-code_cur); \
    code_cur=0; \
    if (in_cur+read_len>in_size){   \
        code_buf_size-=read_len-(size_t)(in_size-in_cur);       \
        read_len=in_size-in_cur;    \
    }           \
    _check(read_len==xread(in,code_buf+code_buf_size-read_len,read_len),    \
           LIBDEFLATE_DESTREAM_READ_FILE_ERROR);    \
    in_cur+=read_len;               \
} while(0)

static inline size_t _limitMaxDefBSize(size_t maxDeflateBlockSize){
    if (unlikely(maxDeflateBlockSize<kMaxDeflateBlockSize_min)) return kMaxDeflateBlockSize_min;
    if (unlikely(maxDeflateBlockSize>kMaxDeflateBlockSize_max)) return kMaxDeflateBlockSize_max;
    return maxDeflateBlockSize;
}

int gzip_decompress_by_stream(struct libdeflate_decompressor *d,size_t maxDeflateBlockSize,
	                        struct file_stream *in, u64 in_size,struct file_stream *out,
							u64* _actual_in_nbytes_ret,u64* _actual_out_nbytes_ret){
    int err_code=0;
    u8* pmem=0;
    u8* code_buf;
    u8* data_buf;
    u64    in_cur=0;
    u64    out_cur=0;
    const size_t curBlockSize=_limitMaxDefBSize(maxDeflateBlockSize);
    const size_t data_buf_size=curBlockSize*2+kDictSize;
    size_t data_cur=kDictSize; //empty
    size_t code_buf_size=(curBlockSize*2<in_size)?curBlockSize*2:in_size;
    size_t code_cur=code_buf_size; //empty
    size_t actual_in_nbytes_ret;
    uint32_t data_crc=0;
    int ret;

    pmem=(u8*)malloc(data_buf_size+code_buf_size);
    _check(pmem!=0, LIBDEFLATE_DESTREAM_MEM_ALLOC_ERROR);
    data_buf=pmem;
    code_buf=data_buf+data_buf_size;
    _read_code_from_file();

    {//gzip head
        ret=libdeflate_gzip_decompress_head(code_buf,code_buf_size-code_cur,&actual_in_nbytes_ret);
        _check_d(ret);
        code_cur+=actual_in_nbytes_ret;
    }

    while(1){
        int    is_final_block_ret;
        size_t actual_out_nbytes_ret;
        size_t dict_size=_dictSize_avail(out_cur+(data_cur-kDictSize));
        if (code_cur>=curBlockSize)
            _read_code_from_file();
        ret=libdeflate_deflate_decompress_block(d,code_buf+code_cur,code_buf_size-code_cur,
                data_buf+data_cur-dict_size,dict_size,data_buf_size-data_cur,
                &actual_in_nbytes_ret,&actual_out_nbytes_ret,
                LIBDEFLATE_STOP_BY_ANY_BLOCK,&is_final_block_ret);
        _check_d(ret);
        code_cur+=actual_in_nbytes_ret;
        data_cur+=actual_out_nbytes_ret;

        if (is_final_block_ret||(data_cur>curBlockSize+kDictSize)){//save data to out file
            if (out)
                _check(0==full_write(out,data_buf+kDictSize,data_cur-kDictSize), LIBDEFLATE_DESTREAM_WRITE_FILE_ERROR);
            data_crc=libdeflate_crc32(data_crc,data_buf+kDictSize,data_cur-kDictSize);
            out_cur+=data_cur-kDictSize;
            dict_size=_dictSize_avail(out_cur);
            memmove(data_buf+kDictSize-dict_size,data_buf+data_cur-dict_size,dict_size);//dict data for next block
            data_cur=kDictSize;
        }
        if (is_final_block_ret)
            break;
    }
    
    {//gzip foot
        uint32_t saved_crc;
        uint32_t saved_uncompress_nbytes;
        if (code_cur+GZIP_FOOTER_SIZE>code_buf_size)
            _read_code_from_file();
        ret=libdeflate_gzip_decompress_foot(code_buf+code_cur,code_buf_size-code_cur,
                &saved_crc,&saved_uncompress_nbytes,&actual_in_nbytes_ret);
        _check_d(ret);
        code_cur+=actual_in_nbytes_ret;

        _check(saved_crc==data_crc, LIBDEFLATE_DESTREAM_DATA_CRC_ERROR);
        _check(saved_uncompress_nbytes==(u32)out_cur, LIBDEFLATE_DESTREAM_DATA_SIZE_ERROR);
    }

    if (_actual_in_nbytes_ret)
        *_actual_in_nbytes_ret=(in_cur-(size_t)(code_buf_size-code_cur));
    if (_actual_out_nbytes_ret)
        *_actual_out_nbytes_ret=out_cur;

_out:
    if (pmem) free(pmem);
    return err_code;
}

