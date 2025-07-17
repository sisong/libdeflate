/*
 * gzip_compress_by_stream_mt.cpp
 * added compress by stream & multi-thread parallel, 2023--2024 housisong
 */
#include <vector>
#include <thread>
#include <mutex>
#include <atomic>
#include "../lib/gzip_overhead.h"
#include "gzip_compress_by_stream_mt.h"
#include <assert.h>
namespace {
static const size_t kDictSize  = (1<<15); //MATCHFINDER_WINDOW_SIZE
static const size_t kStepSize_better= (size_t)1024*1024*2; //required O(kStepSize_better*thread_num) memory;
static const size_t kStepSize_min =(size_t)1024*256;
#define _check(v,_ret_errCode) do { if (!(v)) {err_code=_ret_errCode; goto _out; } } while (0)
static inline size_t _dictSize_avail(u64 uncompressed_pos) {
                        return (uncompressed_pos<kDictSize)?(size_t)uncompressed_pos:kDictSize; }

//multi-thread

    struct TWorkBuf{
        struct TWorkBuf*    next;
        size_t              dict_size;
        u64                 in_cur;
        size_t              in_nbytes;
        size_t              code_nbytes;
        bool                is_final_block;
        u8                  buf[1]; //one_buf_size
    };

struct TThreadData{
    struct libdeflate_compressor** c_list;
    size_t              in_step_size;
    uint32_t            in_crc;
    size_t              block_bound;
    struct file_stream* in;
    u64                 in_size;
    struct file_stream* out;
    u64                 out_cur;
    std::atomic<int>    err_code;

    u64                 _in_cur;
    std::atomic<u64>    _in_cur_writed_end;
    TWorkBuf*           _codeBuf_list;
    TWorkBuf*           _freeBuf_list;
    u8*                 _dictBuf;
    std::mutex          _lock_slight;
    std::mutex          _lock_read;
    std::mutex          _lock_write;
};

    static inline void _insert_freeBufs(TWorkBuf** node_list,TWorkBuf* nodes){
        assert(nodes!=0);
        TWorkBuf* root=nodes;
        while (nodes->next) nodes=nodes->next;
        nodes->next=*node_list;
        *node_list=root;
    }
    static inline TWorkBuf* _pop_one_freeBuf(TWorkBuf** node_list){
        TWorkBuf* result=*node_list;
        if (result){
            *node_list=result->next;
            result->next=0;
        }
        return result;
    }
    static inline void _by_order_insert_one_codeBuf(TWorkBuf** node_list,TWorkBuf* node){
        assert((node!=0)&&(node->next==0));
        while ((*node_list)&&((*node_list)->in_cur<node->in_cur))
            node_list=&((*node_list)->next);
        node->next=*node_list;
        *node_list=node;
    }
    static TWorkBuf* _by_order_pop_codeBufs(TWorkBuf** node_list,u64 in_cur_writed_end){
        if (((*node_list)==0)||((*node_list)->in_cur!=in_cur_writed_end))
            return 0;
        TWorkBuf** node_end=node_list;
        do{
            in_cur_writed_end+=(*node_end)->in_nbytes;
            node_end=&(*node_end)->next;
        } while ((*node_end)&&((*node_end)->in_cur==in_cur_writed_end));
        TWorkBuf* result=*node_list;
        *node_list=*node_end;
        *node_end=0;
        return result;
    }

    static inline void _update_err_code(TThreadData* td,int err_code){
        if (err_code!=0){
            int _expected=0;
            td->err_code.compare_exchange_strong(_expected,err_code);
        }
    }

    static TWorkBuf* _write_codeBufs(TThreadData* td,TWorkBuf* nodes){
        assert(nodes!=0);
        int err_code=0;
        TWorkBuf* result=nodes;
        u64 in_cur_writed_end=td->_in_cur_writed_end.load();
        {
            std::lock_guard<std::mutex> _auto_locker(td->_lock_write);
            while (nodes){
                assert(in_cur_writed_end==nodes->in_cur);
                const u8* pcode=nodes->buf+nodes->dict_size+nodes->in_nbytes;
                int w_ret=full_write(td->out,pcode,nodes->code_nbytes);
                _check(w_ret==0, LIBDEFLATE_ENSTREAM_MT_WRITE_FILE_ERROR);
                td->out_cur+=nodes->code_nbytes;

                in_cur_writed_end=nodes->in_cur+nodes->in_nbytes;
                nodes=nodes->next;
            }
        }
        td->_in_cur_writed_end.store(in_cur_writed_end);
        return result; //ok

    _out:
        _update_err_code(td,err_code);
        return 0; //fail
    }

    static TWorkBuf* _read_data_to_one_buf(TThreadData* td,TWorkBuf* node){
        assert(node!=0);
        int err_code=0;
        {
            std::lock_guard<std::mutex> _auto_locker(td->_lock_read);
            if (td->_in_cur==td->in_size) return 0;
            node->is_final_block=(td->_in_cur+td->in_step_size>=td->in_size);
            node->in_nbytes=node->is_final_block?td->in_size-td->_in_cur:td->in_step_size;
            node->dict_size=_dictSize_avail(td->_in_cur);
            node->in_cur=td->_in_cur;
            memcpy(node->buf,td->_dictBuf,node->dict_size);

            //read block data
            ssize_t r_len=xread(td->in,node->buf+node->dict_size,node->in_nbytes);
            _check(r_len==node->in_nbytes, LIBDEFLATE_ENSTREAM_MT_READ_FILE_ERROR);
            td->in_crc=libdeflate_crc32(td->in_crc,node->buf+node->dict_size,node->in_nbytes);

            //dict data for next block
            td->_in_cur+=node->in_nbytes;
            size_t nextDictSize=_dictSize_avail(td->_in_cur);
            memcpy(td->_dictBuf,node->buf+node->dict_size+node->in_nbytes-nextDictSize,nextDictSize);
        }
    _out:
        _update_err_code(td,err_code);
        return (err_code==0)?node:0;
    }

    static TWorkBuf* _new_workBuf(TThreadData* td,TWorkBuf* finished_node,size_t thread_i){
        while (true){
            TWorkBuf* result=0;
            while (result==0){//wait a free buf by loop  //todo: wait by signal, not use thread yield
                TWorkBuf* need_write_codeBufs=0;
                {   
                    const u64 in_cur_writed_end=td->_in_cur_writed_end.load();
                    if (in_cur_writed_end==td->in_size) return 0; //all write work finished
                    if (td->err_code.load()!=0) return 0;

                    std::lock_guard<std::mutex> _auto_locker(td->_lock_slight);
                    if (finished_node){
                        _by_order_insert_one_codeBuf(&td->_codeBuf_list,finished_node);
                        finished_node=0;
                    }
                    need_write_codeBufs=_by_order_pop_codeBufs(&td->_codeBuf_list,in_cur_writed_end);//try get write works
                    if (need_write_codeBufs==0)
                        result=_pop_one_freeBuf(&td->_freeBuf_list);
                }
                if (need_write_codeBufs){ //do write works
                    result=_write_codeBufs(td,need_write_codeBufs);
                    if ((result)&&(result->next)){ //only need a free buf
                        std::lock_guard<std::mutex> _auto_locker(td->_lock_slight);
                        _insert_freeBufs(&td->_freeBuf_list,result->next);
                        result->next=0;
                    }
                }
                if (result==0)
                    std::this_thread::yield(); 
            }
        
            result=_read_data_to_one_buf(td,result); //try get a compress work
            if (result) return result;  //got a compress work
            if (thread_i!=0) return 0;  //always keep thread 0 loop until all write work finished, other threads can exit
        }
    }

static void _compress_blocks_thread(TThreadData* td,size_t thread_i){
    const int is_byte_align=1;
    struct libdeflate_compressor* c=td->c_list[thread_i];
    TWorkBuf* wbuf=0;
    try{
        while (wbuf=_new_workBuf(td,wbuf,thread_i)){
            wbuf->code_nbytes=libdeflate_deflate_compress_block(c,wbuf->buf,wbuf->dict_size,wbuf->in_nbytes,
                                                wbuf->is_final_block,wbuf->buf+wbuf->dict_size+wbuf->in_nbytes,
                                                td->block_bound,is_byte_align);
            if (wbuf->code_nbytes==0){
                _update_err_code(td,LIBDEFLATE_ENSTREAM_MT_COMPRESS_BLOCK_ERROR); //compress error
                break;
            }
        }
    }catch(...){
        _update_err_code(td,LIBDEFLATE_ENSTREAM_MT_THREAD_EXCEPTION_ERROR);
    }
}

static void _compress_blocks_mt(TThreadData* td,size_t thread_num,u8* pmem,size_t one_buf_size,size_t workBufCount){
    td->_in_cur=0;
    td->_in_cur_writed_end=0;
    td->_codeBuf_list=0;
    td->_freeBuf_list=0;
    td->err_code=0;
    for (size_t i=0;i<workBufCount;i++,pmem+=one_buf_size){
        TWorkBuf* wbuf=(TWorkBuf*)pmem;
        wbuf->next=0;
        _insert_freeBufs(&td->_freeBuf_list,wbuf);
    }
    td->_dictBuf=pmem;
    
    try{
        std::vector<std::thread> threads(thread_num-1);
        for (size_t i=0; i<threads.size();i++)
            threads[i]=std::thread(_compress_blocks_thread,td,i+1);
        _compress_blocks_thread(td,0);
        for (size_t i=0;i<threads.size();i++)
            threads[i].join();
        if (td->_in_cur_writed_end.load()!=td->in_size)
           _update_err_code(td,LIBDEFLATE_ENSTREAM_MT_OUT_LACK_ERROR);
    }catch(...){
        _update_err_code(td,LIBDEFLATE_ENSTREAM_MT_EXCEPTION_ERROR);
    }
}

static inline size_t _limitStepSize(u64 in_size,size_t in_step_size){
    if (unlikely(in_step_size>in_size)) in_step_size=in_size;
    if (unlikely(in_step_size<kStepSize_min)) in_step_size=kStepSize_min;
    return in_step_size;
}

} //namespace

int gzip_compress_by_stream_mt(int compression_level,struct file_stream *in,u64 in_size,
                               struct file_stream *out,int thread_num,u64* actual_out_nbytes_ret){
    int err_code=0;
    u8* pmem=0;
    struct libdeflate_compressor** c_list=0;
    uint32_t     in_crc=0;
    u64 out_cur=0;
    size_t in_step_size=_limitStepSize(in_size,kStepSize_better);
    const u64 _allWorkCount=(in_size+in_step_size-1)/in_step_size;
    thread_num=(thread_num<=1)?1:((thread_num<_allWorkCount)?thread_num:_allWorkCount);
    size_t workBufCount=(thread_num<=1)?1:(thread_num+(thread_num-1)/2+1);
    workBufCount=(workBufCount<_allWorkCount)?workBufCount:_allWorkCount;
    const size_t block_bound=libdeflate_deflate_compress_bound_block(in_step_size);
    size_t one_buf_size=kDictSize+in_step_size+block_bound;
    one_buf_size=(thread_num<=1)?one_buf_size:(one_buf_size+sizeof(TWorkBuf)+256-1)/256*256;

    size_t _sum_buf_size=one_buf_size*workBufCount + ((thread_num<=1)?0:kDictSize);
    pmem=(u8*)malloc(_sum_buf_size + sizeof(struct libdeflate_compressor*)*thread_num );
    _check(pmem!=0, LIBDEFLATE_ENSTREAM_MEM_ALLOC_ERROR);
    c_list=(struct libdeflate_compressor**)(pmem+_sum_buf_size);
    memset(c_list,0,sizeof(struct libdeflate_compressor*)*thread_num);
    for (size_t i=0; i<thread_num;i++){
        c_list[i]=libdeflate_alloc_compressor(compression_level);
        _check(c_list[i]!=0, LIBDEFLATE_ENSTREAM_ALLOC_COMPRESSOR_ERROR);
    }

    {//gzip head
        size_t code_nbytes=libdeflate_gzip_compress_head(compression_level,in_size,pmem,block_bound);
        _check(code_nbytes>0, LIBDEFLATE_ENSTREAM_GZIP_HEAD_ERROR);
        int w_ret=full_write(out,pmem,code_nbytes);
        _check(w_ret==0, LIBDEFLATE_ENSTREAM_WRITE_FILE_ERROR);
        out_cur+=code_nbytes;
    }

    if (thread_num<=1){ // compress blocks single thread
        const int is_byte_align = 0;
        u8* pdata=pmem;
        u8* pcode=pdata+kDictSize+in_step_size;
        struct libdeflate_compressor* c=c_list[0];
        for (u64 in_cur=0;in_cur<in_size;){//compress by stream
            bool is_final_block=(in_cur+in_step_size>=in_size);
            size_t in_nbytes=is_final_block?in_size-in_cur:in_step_size;
            size_t dict_size=_dictSize_avail(in_cur);

            //read block data
            ssize_t r_len=xread(in,pdata+dict_size,in_nbytes);
            _check(r_len==in_nbytes, LIBDEFLATE_ENSTREAM_READ_FILE_ERROR);
            in_crc=libdeflate_crc32(in_crc,pdata+dict_size,in_nbytes);

            //compress the block
            size_t code_nbytes=libdeflate_deflate_compress_block(c,pdata,dict_size,in_nbytes,is_final_block,
                                                                 pcode,block_bound,is_byte_align);
            _check(code_nbytes>0, LIBDEFLATE_ENSTREAM_COMPRESS_BLOCK_ERROR);

            //write the block's code
            int w_ret=full_write(out,pcode,code_nbytes);
            _check(w_ret==0, LIBDEFLATE_ENSTREAM_WRITE_FILE_ERROR);
            out_cur+=code_nbytes;

            //dict data for next block
            in_cur+=in_nbytes;
            size_t nextDictSize=_dictSize_avail(in_cur);
            memmove(pdata,pdata+dict_size+in_nbytes-nextDictSize,nextDictSize);
        }
    }else{ // compress blocks multi-thread
        TThreadData threadData={c_list,in_step_size,in_crc,block_bound,in,in_size,out,out_cur};
        _compress_blocks_mt(&threadData,thread_num,pmem,one_buf_size,workBufCount);
        in_crc=threadData.in_crc;
        out_cur=threadData.out_cur;
        err_code=threadData.err_code;
    }
    
    {//gzip foot
        size_t code_nbytes=libdeflate_gzip_compress_foot(in_crc,in_size,pmem,block_bound);
        _check(code_nbytes>0, LIBDEFLATE_ENSTREAM_GZIP_FOOT_ERROR);
        int w_ret=full_write(out,pmem,code_nbytes);
        _check(w_ret==0, LIBDEFLATE_ENSTREAM_WRITE_FILE_ERROR);
        out_cur+=code_nbytes;
    }

    if (actual_out_nbytes_ret)
        *actual_out_nbytes_ret=out_cur;

_out:
    if (c_list){
        for (size_t i=0; i<thread_num;i++)
            libdeflate_free_compressor(c_list[i]);
    }
    if (pmem) free(pmem);
    return err_code;
}


int gzip_compress_by_stream(int compression_level,struct file_stream *in,u64 in_size,
                            struct file_stream *out,u64* actual_out_nbytes_ret){
    return gzip_compress_by_stream_mt(compression_level,in,in_size,out,1,actual_out_nbytes_ret);
}
