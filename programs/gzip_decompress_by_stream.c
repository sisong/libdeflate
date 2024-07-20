/*
 * gzip_decompress_by_stream.c
 * added decompress by stream, 2023 housisong
 */
#include "../lib/gzip_overhead.h"
#include "../lib/gzip_constants.h"
#include "gzip_decompress_by_stream.h"
#include <assert.h>
#include <string.h> //memcpy

#define             kDictSize    (1<<15)  //MATCHFINDER_WINDOW_SIZE
static const size_t kMaxDeflateBlockSize   = 301000; //default starting value, if input DeflateBlockSize greater than this, then will auto increase;  
static const size_t kMaxDeflateBlockSize_min =1024*4;
static const size_t kMaxDeflateBlockSize_max = ((~(size_t)0)-kDictSize)/2;
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

#define _free_swap_buf(oldBuf_,newBuf_) do{ free(oldBuf_); oldBuf_=newBuf_; newBuf_=0; }while(0)

int gzip_decompress_by_stream(struct libdeflate_decompressor *d,
	                        struct file_stream *in, u64 in_size,struct file_stream *out,
							u64* _actual_in_nbytes_ret,u64* _actual_out_nbytes_ret){
    int err_code=0;
    u8* data_buf=0;
    u8* code_buf=0;
    u8* _data_buf=0;
    u8* _code_buf=0;
    u64    in_cur=0;
    u64    out_cur=0;
    size_t curDeflateBlockSize=kMaxDeflateBlockSize;
    size_t curBlockSize=_limitMaxDefBSize(curDeflateBlockSize);
    size_t data_buf_size=curBlockSize+kDictSize;
    size_t code_buf_size=(curBlockSize<in_size)?curBlockSize:in_size;
    size_t data_cur=kDictSize; //empty
    size_t code_cur=code_buf_size; //empty
    size_t actual_in_nbytes_ret;
    uint32_t data_crc=0;
    int ret;

    data_buf=(u8*)malloc(data_buf_size);
    _check(data_buf!=0, LIBDEFLATE_DESTREAM_MEM_ALLOC_ERROR);
    code_buf=(u8*)malloc(code_buf_size);
    _check(code_buf!=0, LIBDEFLATE_DESTREAM_MEM_ALLOC_ERROR);

    _read_code_from_file();
    {//gzip head
        ret=libdeflate_gzip_decompress_head(code_buf,code_buf_size-code_cur,&actual_in_nbytes_ret);
        _check_d(ret);
        code_cur+=actual_in_nbytes_ret;
    }

    int is_final_block_ret=0;
    while(1){
        //     [ ( dict ) |     dataBuf                 ]              [            codeBuf              ]
        //     ^              ^         ^               ^              ^                     ^           ^
        //  data_buf       out_cur   data_cur     data_buf_size     code_buf              code_cur code_buf_size
        size_t kLimitDataSize=curBlockSize/2+kDictSize;
        size_t kLimitCodeSize=code_buf_size/2;
    __datas_prepare:
        if (is_final_block_ret||(data_cur>kLimitDataSize)){//save data to out file
            if (out)
                _check(0==full_write(out,data_buf+kDictSize,data_cur-kDictSize), LIBDEFLATE_DESTREAM_WRITE_FILE_ERROR);
            data_crc=libdeflate_crc32(data_crc,data_buf+kDictSize,data_cur-kDictSize);
            out_cur+=data_cur-kDictSize;
            if (is_final_block_ret)
                break;
            size_t dict_size=_dictSize_avail(out_cur);
            memmove(data_buf+kDictSize-dict_size,data_buf+data_cur-dict_size,dict_size);//dict data for next block
            data_cur=kDictSize;
        }
        size_t dict_size=_dictSize_avail(out_cur+(data_cur-kDictSize));
        if (code_cur>kLimitCodeSize)
            _read_code_from_file();

        size_t actual_out_nbytes_ret;
        const size_t dec_state=libdeflate_deflate_decompress_get_state(d);
        ret=libdeflate_deflate_decompress_block(d,code_buf+code_cur,code_buf_size-code_cur,
                data_buf+data_cur-dict_size,dict_size,data_buf_size-data_cur,
                &actual_in_nbytes_ret,&actual_out_nbytes_ret,
                LIBDEFLATE_STOP_BY_ANY_BLOCK,&is_final_block_ret);
        if (ret!=LIBDEFLATE_SUCCESS){
            if ((in_cur==in_size)&&(ret!=LIBDEFLATE_INSUFFICIENT_SPACE))
                _check_d(ret);
            kLimitDataSize=kDictSize;
            kLimitCodeSize=0;
            if ((data_cur>kDictSize)||((code_cur>0)&&(in_cur<in_size))) { //need more datas & retry
                //ok
            }else if (curDeflateBlockSize<kMaxDeflateBlockSize_max){//need increase buf size & retry
                curDeflateBlockSize=(curDeflateBlockSize*2<kMaxDeflateBlockSize_max)?curDeflateBlockSize*2:kMaxDeflateBlockSize_max;
                size_t _curBlockSize=_limitMaxDefBSize(curDeflateBlockSize);
                size_t _data_buf_size=_curBlockSize+kDictSize;
                const size_t loaded_in_size=(code_buf_size-code_cur);
                const u64 rem_in_size=loaded_in_size+(in_size-in_cur);
                size_t _code_buf_size=(_curBlockSize<rem_in_size)?_curBlockSize:rem_in_size;
                curBlockSize=_curBlockSize;
                {
                    _data_buf=(u8*)malloc(_data_buf_size);
                    _check(_data_buf!=0, LIBDEFLATE_DESTREAM_MEM_ALLOC_ERROR);
                    memcpy(_data_buf,data_buf,data_cur);
                    _free_swap_buf(data_buf,_data_buf);
                    data_buf_size=_data_buf_size;
                }
                if (_code_buf_size>code_buf_size){
                    _code_buf=(u8*)malloc(_code_buf_size);
                    _check(_code_buf!=0, LIBDEFLATE_DESTREAM_MEM_ALLOC_ERROR);
                    memcpy(_code_buf+_code_buf_size-loaded_in_size,code_buf+code_cur,loaded_in_size);
                    _free_swap_buf(code_buf,_code_buf);
                    code_cur+=_code_buf_size-code_buf_size;
                    code_buf_size=_code_buf_size;
                }
            }else{ //decompress fail, can't increase buf
                _check_d(ret);
            }
            libdeflate_deflate_decompress_set_state(d,dec_state);
            goto __datas_prepare; //retry by more datas
        }
        //decompress ok
        code_cur+=actual_in_nbytes_ret;
        data_cur+=actual_out_nbytes_ret;
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
    if (data_buf) free(data_buf);
    if (code_buf) free(code_buf);
    if (_data_buf) free(_data_buf);
    if (_code_buf) free(_code_buf);
    return err_code;
}

