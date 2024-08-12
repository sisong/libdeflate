/*
 * gzip_decompress_by_stream_mt.cpp
 * added decompress by stream & multi-thread, 2023--2024 housisong
 */
#include <vector>
#include <thread>
#include <mutex>
#include <atomic>
#include <assert.h>
#include <string.h> //memcpy
#include "../lib/gzip_overhead.h"
#include "../lib/gzip_constants.h"
#include "gzip_decompress_by_stream_mt.h"

#define             kDictSize    (1<<15)  //MATCHFINDER_WINDOW_SIZE
static const size_t kMaxDeflateBlockSize   = 301000; //default starting value, if input DeflateBlockSize greater than this, then will auto increase;  
static const size_t kMaxDeflateBlockSize_min =1024*4;
static const size_t kMaxDeflateBlockSize_max = ((~(size_t)0)-kDictSize)/2;
static const size_t kInputSufficientSize = (1<<16);
#define _check(v,_ret_errCode) do { if (!(v)) {err_code=_ret_errCode; goto _out; } } while (0)
#define _check_d(_d_ret) _check(_d_ret==LIBDEFLATE_SUCCESS, _d_ret)

typedef ssize_t (*xread_t)(struct file_stream *strm, void *buf, size_t count);
typedef int (*full_write_t)(struct file_stream *strm, const void *buf, size_t count);

#define _read_code_from_file() do{  \
    size_t read_len=code_cur;       \
    memmove(code_buf,code_buf+code_cur,code_buf_size-code_cur); \
    code_cur=0; \
    if (in_cur+read_len>in_size){   \
        code_buf_size-=read_len-(size_t)(in_size-in_cur);       \
        read_len=in_size-in_cur;    \
    }           \
    _check(read_len==xread_proc(in,code_buf+code_buf_size-read_len,read_len),    \
           LIBDEFLATE_DESTREAM_READ_FILE_ERROR);    \
    in_cur+=read_len;               \
} while(0)

static inline size_t _limitMaxDefBSize(size_t maxDeflateBlockSize){
    if (unlikely(maxDeflateBlockSize<kMaxDeflateBlockSize_min)) return kMaxDeflateBlockSize_min;
    if (unlikely(maxDeflateBlockSize>kMaxDeflateBlockSize_max)) return kMaxDeflateBlockSize_max;
    return maxDeflateBlockSize;
}

static int _gzip_decompress_by_stream(struct libdeflate_decompressor *d,
				struct file_stream *in, u64 in_size,bool isMultiGz,struct file_stream *out,
                xread_t xread_proc,full_write_t full_write_proc,
				u64* _actual_in_nbytes_ret,u64* _actual_out_nbytes_ret){
    if (isMultiGz) assert(_actual_in_nbytes_ret==0);
    int err_code=0;
    u8* data_buf=0;
    u8* code_buf=0;
    u8* _dict_buf_back=0;
    u64    in_cur=0;
    u64    out_cur=0;
    u64    last_out_cur=0;
    size_t curDeflateBlockSize=kMaxDeflateBlockSize;
    size_t curBlockSize=_limitMaxDefBSize(curDeflateBlockSize);
    size_t data_buf_size=curBlockSize+kDictSize;
    size_t code_buf_size=(curBlockSize<in_size)?curBlockSize:in_size;
    size_t data_cur=kDictSize; //empty
    size_t code_cur=code_buf_size; //empty
    size_t actual_in_nbytes_ret;
    uint32_t data_crc;
    int is_final_block_ret;
    int ret;

    data_buf=(u8*)malloc(data_buf_size);
    _check(data_buf!=0, LIBDEFLATE_DESTREAM_MEM_ALLOC_ERROR);
    code_buf=(u8*)malloc(code_buf_size);
    _check(code_buf!=0, LIBDEFLATE_DESTREAM_MEM_ALLOC_ERROR);


    do{//loop for multi concatenated gzip
        data_crc=0;
        is_final_block_ret=0;
        {//gzip head
            if (code_cur+GZIP_MIN_OVERHEAD>code_buf_size)
                _read_code_from_file();
            ret=libdeflate_gzip_decompress_head(code_buf,code_buf_size-code_cur,&actual_in_nbytes_ret);
            _check_d(ret);
            code_cur+=actual_in_nbytes_ret;
        }

        while(1){
            //     [ ( dict ) |     dataBuf                 ]              [            codeBuf              ]
            //     ^              ^         ^               ^              ^                     ^           ^
            //  data_buf       out_cur   data_cur     data_buf_size     code_buf              code_cur code_buf_size
            size_t kLimitDataSize=curBlockSize/2+kDictSize;
            size_t kLimitCodeSize=code_buf_size/2;
            size_t actual_out_nbytes_ret;
            uint16_t dec_state;
        __datas_prepare:
            if (is_final_block_ret||(data_cur>kLimitDataSize)){//save data to out file
                if (out)
                    _check(0==full_write_proc(out,data_buf+kDictSize,data_cur-kDictSize), LIBDEFLATE_DESTREAM_WRITE_FILE_ERROR);
                data_crc=libdeflate_crc32(data_crc,data_buf+kDictSize,data_cur-kDictSize);
                out_cur+=data_cur-kDictSize;
                if (isMultiGz){//dict data for next block
                    memmove(data_buf,data_buf+data_cur-kDictSize,kDictSize);
                    data_cur=kDictSize;
                }
                if (is_final_block_ret)
                    break;
            }
            if (code_cur>kLimitCodeSize)
                _read_code_from_file();
            dec_state=libdeflate_deflate_decompress_get_state(d);
            ret=libdeflate_deflate_decompress_block(d,code_buf+code_cur,code_buf_size-code_cur,
                    data_buf+data_cur-kDictSize,kDictSize,data_buf_size-data_cur,
                    &actual_in_nbytes_ret,&actual_out_nbytes_ret,
                    LIBDEFLATE_STOP_BY_ANY_BLOCK,&is_final_block_ret);
            if (ret!=LIBDEFLATE_SUCCESS){
                if (((in_cur==in_size)||((size_t)((code_buf_size-code_cur)-actual_in_nbytes_ret)>kInputSufficientSize))
                        &&(ret!=LIBDEFLATE_INSUFFICIENT_SPACE))
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
                        assert(data_cur==kDictSize);
                        if (_dict_buf_back==0){
                            _dict_buf_back=(u8*)malloc(kDictSize);
                            _check(_dict_buf_back!=0, LIBDEFLATE_DESTREAM_MEM_ALLOC_ERROR);
                        }
                        memcpy(_dict_buf_back,data_buf,kDictSize);
                        free(data_buf); data_buf=0;
                    }
                    if (_code_buf_size>code_buf_size){
                        u8* _code_buf=(u8*)realloc(code_buf,_code_buf_size);
                        _check(_code_buf!=0, LIBDEFLATE_DESTREAM_MEM_ALLOC_ERROR);
                        code_buf=_code_buf; _code_buf=0;
                        memcpy(code_buf+_code_buf_size-loaded_in_size,code_buf+code_cur,loaded_in_size);
                        code_cur+=_code_buf_size-code_buf_size;
                        code_buf_size=_code_buf_size;
                    }
                    {
                        data_buf=(u8*)malloc(_data_buf_size);
                        _check(data_buf!=0, LIBDEFLATE_DESTREAM_MEM_ALLOC_ERROR);
                        memcpy(data_buf,_dict_buf_back,kDictSize);
                        data_buf_size=_data_buf_size;
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
            _check(saved_uncompress_nbytes==(u32)(out_cur-last_out_cur), LIBDEFLATE_DESTREAM_DATA_SIZE_ERROR);
            last_out_cur=out_cur;
        }
    } while (isMultiGz && (0<(u64)((in_size-in_cur)+(code_buf_size-code_cur))));

    if (_actual_in_nbytes_ret)
        *_actual_in_nbytes_ret=(in_cur-(size_t)(code_buf_size-code_cur));
    if (_actual_out_nbytes_ret)
        *_actual_out_nbytes_ret=out_cur;

_out:
    libdeflate_deflate_decompress_block_reset(d);
    if (data_buf) free(data_buf);
    if (code_buf) free(code_buf);
    if (_dict_buf_back) free(_dict_buf_back);
    return err_code;
}


int gzip_decompress_by_stream(struct libdeflate_decompressor *d,
							  struct file_stream *in, u64 in_size, struct file_stream *out,
							  u64* actual_in_nbytes_ret,u64* actual_out_nbytes_ret){
	return _gzip_decompress_by_stream(d,in,in_size,false,out,xread,full_write,
									  actual_in_nbytes_ret,actual_out_nbytes_ret);
}

int gzips_decompress_by_stream(struct libdeflate_decompressor *d,
							  struct file_stream *in, u64 in_size, struct file_stream *out,
							  u64* actual_out_nbytes_ret){
	return _gzip_decompress_by_stream(d,in,in_size,true,out,xread,full_write,
									  0,actual_out_nbytes_ret);
}

//------------------------------------------------------------------------------------------------
// multi-thread
static const size_t kBestWBufCount=3;
static const size_t kWBufSize=1024*256;
static const size_t kRBufSize=1024*64;

struct work_buf_t{
    work_buf_t* next;
    size_t      cur;
    u8*         buf;
};

static work_buf_t* malloc_list(size_t listCount,size_t bufSize){
    work_buf_t* list=(work_buf_t*)malloc((sizeof(work_buf_t)+bufSize)*listCount);
    u8* buf=(u8*)&list[listCount];
    for (size_t i=0;i<listCount;++i,buf+=bufSize){
        list[i].buf=buf;
        list[i].next=(i+1<listCount)?&list[i+1]:0;
    }
    return list;
}

struct stream_mt_t{
    file_stream*    base;
    int             err_code;
    u64             data_len;
    work_buf_t*     cur_work_buf;
    inline  stream_mt_t():base(0),err_code(0),data_len(0),
                          cur_work_buf(0),free_list(0),work_list(0),
                          _pmem(0),_is_work_exit(0),_is_work_finished(0){}
    inline ~stream_mt_t(){ if (_pmem) free(_pmem); }
    inline bool init(file_stream* _base,u64 _data_len,size_t bufSize){
        assert((base==0)&&(_base!=0));
        base=_base;
        data_len=_data_len;
        _pmem=free_list=malloc_list(kBestWBufCount,bufSize);
        return (_pmem!=0);
    }
    inline void work_end(){
        _is_work_exit.store(1);
    }inline void work_finished(){
        _is_work_finished.store(1);
    }
    void push_free_buf(work_buf_t* node_buf){
        std::lock_guard<std::mutex> _auto_locker(_lock);
        node_buf->next=free_list;
        free_list=node_buf;
    }
    inline work_buf_t* pop_free_buf(){//wait a free node
        return _pop_buf(&free_list);
    }
    void push_work_buf(work_buf_t* node_buf){
        node_buf->next=0;
        std::lock_guard<std::mutex> _auto_locker(_lock);
        work_buf_t** plist=&work_list;
        while (*plist) plist=&((*plist)->next);//to last node
        *plist=node_buf;
    }
    inline work_buf_t* pop_work_buf(){//wait a work node
        return _pop_buf(&work_list);
    }
protected:
    work_buf_t*     free_list;
    work_buf_t*     work_list;
    void*           _pmem;
    std::mutex      _lock;
    std::atomic<int> _is_work_exit;
    std::atomic<int> _is_work_finished;
    
    work_buf_t* _pop_buf(work_buf_t** plist){//wait a free node
        while (!_is_work_exit.load()){ 
            {
                std::lock_guard<std::mutex> _auto_locker(_lock);
                work_buf_t* result=*plist;
                if (result){
                    *plist=(*plist)->next;
                    return result;
                }else if (_is_work_finished.load())
                    break;
            }
            std::this_thread::yield(); //todo: wait by signal, not use thread yield
        }
        return 0; //exit work loop
    }
};

struct in_stream_mt:public stream_mt_t{
};

static ssize_t xread_mt(struct file_stream *strm, void *buf, size_t count){
    in_stream_mt* self=(in_stream_mt*)strm;
    u8* dst=(u8*)buf;
    const size_t count_bck=count;
    size_t result=0;
    work_buf_t* cur_buf=self->cur_work_buf;
    while (count){
        if (cur_buf==0){
            cur_buf=self->pop_work_buf();
            if (cur_buf==0)
                break;//fail or exit
        }
        
        const size_t _blen=kRBufSize-cur_buf->cur;
        const size_t clen=(count<_blen)?count:_blen;
        memcpy(dst,cur_buf->buf+cur_buf->cur,clen);
        cur_buf->cur+=clen;
        if (cur_buf->cur==kRBufSize){
            self->push_free_buf(cur_buf);
            cur_buf=0;
        }
        result+=clen;
        dst+=clen;
        count-=clen;
    }
    self->cur_work_buf=cur_buf;
    return (result==count_bck)?result:-1;
}

static void _read_in_thread(in_stream_mt* self){
    u64         data_len=self->data_len;
    work_buf_t* wbuf=0;
    try{
        while ((data_len>0)&&(wbuf=self->pop_free_buf())){
            const size_t clen=(kRBufSize<data_len)?kRBufSize:(size_t)data_len;
            wbuf->cur=kRBufSize-clen;
            if (xread(self->base,wbuf->buf+wbuf->cur,clen)==clen){
                self->push_work_buf(wbuf);
                data_len-=clen;
            }else{
                self->err_code=LIBDEFLATE_DESTREAM_MT_READ_FILE_ERROR;
                break;
            }
        }
    }catch(...){
        self->err_code=LIBDEFLATE_DESTREAM_MT_READ_THREAD_EXCEPTION_ERROR;
    }
    self->data_len=data_len;
    if (self->err_code!=0)
        self->work_end();
    else
        self->work_finished();
}

struct out_stream_mt:public stream_mt_t{
    void wait_flush_write(){
        if ((cur_work_buf)&&(cur_work_buf->cur>0)){
            push_work_buf(cur_work_buf);
            cur_work_buf=0;
        }
        work_finished();
        while (!_is_work_exit.load()){
            std::this_thread::yield(); //todo: wait by signal, not use thread yield
        }
    }
};
static int full_write_mt(struct file_stream *strm, const void *buf, size_t count){
    out_stream_mt* self=(out_stream_mt*)strm;
    const u8* src=(const u8*)buf;
    const size_t count_bck=count;
    size_t result=0;
    work_buf_t* cur_buf=self->cur_work_buf;
    while (count){
        if (cur_buf==0){
            cur_buf=self->pop_free_buf();
            if (cur_buf==0)
                break;//fail or exit
            cur_buf->cur=0; //to empty
        }
        
        const size_t _blen=kWBufSize-cur_buf->cur;
        const size_t clen=(count<_blen)?count:_blen;
        memcpy(cur_buf->buf+cur_buf->cur,src,clen);
        cur_buf->cur+=clen;
        if (cur_buf->cur==kWBufSize){
            self->push_work_buf(cur_buf);
            cur_buf=0;
        }
        result+=clen;
        src+=clen;
        count-=clen;
    }
    self->cur_work_buf=cur_buf;
    return (result==count_bck)?0:-1;
}
static void _write_out_thread(out_stream_mt* self){
    u64         data_len=self->data_len;
    work_buf_t* wbuf=0;
    try{
        while (wbuf=self->pop_work_buf()){
            const size_t clen=wbuf->cur;
            if (full_write(self->base,wbuf->buf,clen)==0){
                self->push_free_buf(wbuf);
                data_len+=clen;
            }else{
                self->err_code=LIBDEFLATE_DESTREAM_MT_WRITE_FILE_ERROR;
                break;
            }
        }
    }catch(...){
        self->err_code=LIBDEFLATE_DESTREAM_MT_WRITE_THREAD_EXCEPTION_ERROR;
    }
    self->data_len=data_len;
    self->work_end();
}

int _gzip_decompress_by_stream_mt(struct libdeflate_decompressor *d,
							      struct file_stream *in, u64 in_size,bool isMultiGz,struct file_stream *out,
							      int thread_num,u64* actual_in_nbytes_ret,u64* actual_out_nbytes_ret){
    if (thread_num<=1){
	    return _gzip_decompress_by_stream(d,in,in_size,isMultiGz,out,xread,full_write,
									      actual_in_nbytes_ret,actual_out_nbytes_ret);
    }
    xread_t         xread_proc=xread;
    full_write_t    full_write_proc=full_write;
    in_stream_mt    in_mt;
    out_stream_mt   out_mt;
    size_t          thread_size=0;

    if (out&&(thread_size+1<thread_num)){
        thread_size++;
        if (!out_mt.init(out,0,kWBufSize)) return LIBDEFLATE_DESTREAM_MEM_ALLOC_ERROR;
        out=(struct file_stream*)&out_mt;
        full_write_proc=full_write_mt;
    }
    if (thread_size+1<thread_num){
        thread_size++;
        if (!in_mt.init(in,in_size,kRBufSize)) return LIBDEFLATE_DESTREAM_MEM_ALLOC_ERROR;
        in=(struct file_stream*)&in_mt;
        xread_proc=xread_mt;
    }
    try{
        std::vector<std::thread> threads(thread_size);
        if (in_mt.base)  threads[--thread_size]=std::thread(_read_in_thread,&in_mt);
        if (out_mt.base) threads[--thread_size]=std::thread(_write_out_thread,&out_mt);
        u64 work_write_ret=0;
        int ret=_gzip_decompress_by_stream(d,in,in_size,isMultiGz,out,xread_proc,full_write_proc,
                                           actual_in_nbytes_ret,&work_write_ret);
        if (in_mt.base)
            in_mt.work_end();
        if (out_mt.base){
            if (ret==LIBDEFLATE_SUCCESS)
                out_mt.wait_flush_write();
            out_mt.work_end();
        }
        for (size_t i=0;i<threads.size();i++)
            threads[i].join();
        if (out_mt.err_code!=LIBDEFLATE_SUCCESS)
            ret=out_mt.err_code;
        if (in_mt.err_code!=LIBDEFLATE_SUCCESS)
            ret=in_mt.err_code;
        if ((ret==LIBDEFLATE_SUCCESS)&&(out_mt.base)&&(out_mt.data_len!=work_write_ret))
            ret=LIBDEFLATE_DESTREAM_MT_OUT_LACK_ERROR;
        if ((ret==LIBDEFLATE_SUCCESS)&&actual_out_nbytes_ret)
            *actual_out_nbytes_ret=work_write_ret;
        return ret;
    }catch(...){
        return LIBDEFLATE_DESTREAM_MT_EXCEPTION_ERROR;
    }
}

int gzip_decompress_by_stream_mt(struct libdeflate_decompressor *d,
							     struct file_stream *in, u64 in_size,struct file_stream *out,
							     int thread_num,u64* actual_in_nbytes_ret,u64* actual_out_nbytes_ret){
    return _gzip_decompress_by_stream_mt(d,in,in_size,false,out,thread_num,actual_in_nbytes_ret,actual_out_nbytes_ret);
}
int gzips_decompress_by_stream_mt(struct libdeflate_decompressor *d,
							      struct file_stream *in, u64 in_size,struct file_stream *out,
							      int thread_num,u64* actual_out_nbytes_ret){
	return _gzip_decompress_by_stream_mt(d,in,in_size,true,out,thread_num,0,actual_out_nbytes_ret);
}
