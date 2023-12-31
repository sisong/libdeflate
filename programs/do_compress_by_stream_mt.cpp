/*
 * do_compress_by_stream_mt.cpp
 * added compression by stream & muti-thread parallel, 2023 housisong
 */
#include <vector>
#include <thread>
#include <mutex>
#include "do_compress_by_stream_mt.h"
#include <assert.h>
namespace {

static const size_t kDictSize  = (1<<15); //MATCHFINDER_WINDOW_SIZE
#define kBlockSize kLibDefBlockSize
#define _check(v,_ret_errCode) do { if (!(v)) {err_code=_ret_errCode; goto _out; } } while (0)
static inline size_t _dictSize_avail(u64 in_read_pos) { return (in_read_pos<kDictSize)?in_read_pos:kDictSize; }

//muti-thread

    struct TWorkBuf{
        struct TWorkBuf*    next;
        size_t              dict_size;
        u64                 in_cur;
        size_t              in_nbytes;
        size_t              code_nbytes;
        bool                is_end_block;
        u8                  buf[1]; //one_buf_size
    };

struct TThreadData{
    struct libdeflate_compressor** c_list;
    uint32_t            in_crc;
    size_t              block_bound;
    struct file_stream* in;
    u64                 in_size;
    struct file_stream* out;
    int                 err_code;

    u64                 _in_cur;
    u64                 _in_cur_writed_end;
    TWorkBuf*           _codeBuf_list;
    TWorkBuf*           _freeBuf_list;
    u8*                 _dictBuf;
    std::mutex          _lock_slight;
    std::mutex          _lock_read;
    std::mutex          _lock_write;
};

    #define _check_td(v,_ret_errCode) do { if (!(v)) {td->err_code=_ret_errCode; goto _out; } } while (0)

    static inline void _insert_freeBufs(TWorkBuf** node_list,TWorkBuf* nodes){
        if (nodes==0) return;
        TWorkBuf* root=nodes;
        while (nodes->next) nodes=nodes->next;
        nodes->next=*node_list;
        *node_list=root;
    }
    static inline TWorkBuf* _pop_one_freeBuf(TWorkBuf** node_list){
        TWorkBuf* result=*node_list;
        if (result)
            *node_list=result->next;
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

    static forceinline void __do_update_err_code(TThreadData* td,int err_code){
        if (td->err_code==0) td->err_code=err_code;
    }
    static inline void _update_err_code(TThreadData* td,int err_code){
        if (err_code!=0){
            std::lock_guard<std::mutex> _auto_locker(td->_lock_slight);
            __do_update_err_code(td,err_code);
        }
    }

    static TWorkBuf* _write_codeBufs(TThreadData* td,TWorkBuf* nodes){
        int err_code=0;
        TWorkBuf* result=0;
        u64 in_cur_writed_end=td->_in_cur_writed_end;
        while(nodes){
            TWorkBuf* _nodes=nodes;
            {
                std::lock_guard<std::mutex> _auto_locker(td->_lock_write);
                while (nodes){
                    assert(in_cur_writed_end==nodes->in_cur);
                    const u8* pcode=nodes->buf+nodes->dict_size+nodes->in_nbytes;
                    int w_ret=full_write(td->out,pcode,nodes->code_nbytes);
                    _check(w_ret==0, 51);

                    in_cur_writed_end=nodes->in_cur+nodes->in_nbytes;
                    nodes=nodes->next;
                }
            }
            _insert_freeBufs(&result,_nodes);
            {
                std::lock_guard<std::mutex> _auto_locker(td->_lock_slight);
                if (err_code!=0) return 0;
                td->_in_cur_writed_end=in_cur_writed_end;
                nodes=_by_order_pop_codeBufs(&td->_codeBuf_list,in_cur_writed_end);
                if (nodes==0) return result; //ok
            }
        }
    _out:
        std::lock_guard<std::mutex> _auto_locker(td->_lock_slight);
        __do_update_err_code(td,err_code);
        return 0; //fail
    }

    static TWorkBuf* _read_to_one_codeBuf(TThreadData* td,TWorkBuf* node){
        int err_code=0;
        {
            std::lock_guard<std::mutex> _auto_locker(td->_lock_read);
            if (td->_in_cur==td->in_size) return 0;
            node->is_end_block=(td->_in_cur+kBlockSize>=td->in_size);
            node->in_nbytes=node->is_end_block?td->in_size-td->_in_cur:kBlockSize;
            node->dict_size=_dictSize_avail(td->_in_cur);
            node->in_cur=td->_in_cur;
            memcpy(node->buf,td->_dictBuf,node->dict_size);

            //read block data
            ssize_t r_len=xread(td->in,node->buf+node->dict_size,node->in_nbytes);
            _check(r_len==node->in_nbytes, 52);
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

    static TWorkBuf* _new_workBuf(TThreadData* td,TWorkBuf* finished_node){
        TWorkBuf* need_write_codeBufs=0;
        if (finished_node){
            std::lock_guard<std::mutex> _auto_locker(td->_lock_slight);
            if (td->err_code!=0) return 0;
            _by_order_insert_one_codeBuf(&td->_codeBuf_list,finished_node);
            need_write_codeBufs=_by_order_pop_codeBufs(&td->_codeBuf_list,td->_in_cur_writed_end);
        }
        TWorkBuf* result=0;
        if (need_write_codeBufs){
            result=_write_codeBufs(td,need_write_codeBufs);
            std::lock_guard<std::mutex> _auto_locker(td->_lock_slight);
            if (td->err_code!=0) return 0;
            _insert_freeBufs(&td->_freeBuf_list,result->next);
        }

        while (result==0){//wait a free buf by loop  //todo: wait by signal
            {
                std::lock_guard<std::mutex> _auto_locker(td->_lock_read);
                if (td->err_code!=0) return 0;
                result=_pop_one_freeBuf(&td->_freeBuf_list);
            }
            if (result==0)
                std::this_thread::yield(); 
        }
        
        result->next=0;
        return _read_to_one_codeBuf(td,result);
    }

static void _compress_blocks_thread(TThreadData* td,size_t thread_i){
    const int is_byte_align=1;
    struct libdeflate_compressor* c=td->c_list[thread_i];
    TWorkBuf* wbuf=0;
    while (wbuf=_new_workBuf(td,wbuf)){
        wbuf->code_nbytes=libdeflate_deflate_compress_block(c,wbuf->buf,wbuf->dict_size,wbuf->in_nbytes,wbuf->is_end_block,
                                                        wbuf->buf+wbuf->dict_size+wbuf->in_nbytes,td->block_bound,is_byte_align);
        if (wbuf->code_nbytes==0){
            _update_err_code(td,41); //compress error
            break;
        }
    }
}

static void _compress_blocks_mt(TThreadData* td,size_t thread_num,u8* pmem,size_t one_buf_size,size_t workBufCount){
    td->_in_cur=0;
    td->_in_cur_writed_end=0;
    td->_codeBuf_list=0;
    td->_freeBuf_list=0;
    for (size_t i=0;i<workBufCount;i++,pmem+=one_buf_size){
        TWorkBuf* wbuf=(TWorkBuf*)pmem;
        wbuf->next=0;
        _insert_freeBufs(&td->_freeBuf_list,(TWorkBuf*)pmem);
    }
    td->_dictBuf=pmem;
    
    try{
        std::vector<std::thread> threads(thread_num-1);
        for (size_t i=0; i<threads.size();i++)
            threads[i]=std::thread(_compress_blocks_thread,td,i);
        _compress_blocks_thread(td,thread_num-1);
        for (size_t i=0;i<threads.size();i++)
            threads[i].join();
        if ((td->err_code==0)&&(td->_in_cur_writed_end!=td->in_size))
            td->err_code=42;
    }catch(...){
        td->err_code=43;
    }
}

} //namespace

int do_compress_by_stream_mt(int compression_level,struct file_stream *in,u64 in_size,
                            struct file_stream *out,int thread_num){
    if (in_size==0) return 0; //ok
    int err_code=0;
    u8* pmem=0;
    struct libdeflate_compressor** c_list=0;
    uint32_t     in_crc=0;
    const s64 _allWorkCount=(in_size+kBlockSize-1)/kBlockSize;
    thread_num=(thread_num<=1)?1:((thread_num<_allWorkCount)?thread_num:_allWorkCount);
    size_t workBufCount=(thread_num<=1)?1:(thread_num+(thread_num-1)/1+1);
    workBufCount=(workBufCount<_allWorkCount)?workBufCount:_allWorkCount;
    const size_t block_bound=libdeflate_deflate_compress_bound_block(kBlockSize);
    size_t one_buf_size=kDictSize+kBlockSize+block_bound;
    one_buf_size=(thread_num<=1)?one_buf_size:(one_buf_size+sizeof(TWorkBuf)+256-1)/256*256;
    pmem=(u8*)malloc(one_buf_size*workBufCount+((thread_num<=1)?0:kDictSize));
    _check(pmem!=0, 11);
    c_list=(struct libdeflate_compressor**)malloc(sizeof(struct libdeflate_compressor*)*thread_num);
    _check(c_list!=0, 12);
    for (size_t i=0; i<thread_num;i++){
        c_list[i]=libdeflate_alloc_compressor(compression_level);
        _check(c_list[i]!=0, 13);
    }

    {//gizp head
        size_t code_nbytes=libdeflate_gzip_compress_head(compression_level,in_size,pmem,block_bound);
        _check(code_nbytes>0, 21);
        int w_ret=full_write(out,pmem,code_nbytes);
        _check(w_ret==0, 22);
    }

    if (thread_num<=1){ // compress blocks single thread
        const int is_byte_align = 0;
        u8* pdata=pmem;
        u8* pcode=pdata+kDictSize+kBlockSize;
        struct libdeflate_compressor* c=c_list[0];
        for (u64 in_cur=0;in_cur<in_size;){//compress by stream
            bool is_end_block=(in_cur+kBlockSize>=in_size);
            size_t in_nbytes=is_end_block?in_size-in_cur:kBlockSize;
            size_t dict_size=_dictSize_avail(in_cur);

            //read block data
            ssize_t r_len=xread(in,pdata+dict_size,in_nbytes);
            _check(r_len==in_nbytes, 31);
            in_crc=libdeflate_crc32(in_crc,pdata+dict_size,in_nbytes);

            //compress the block
            size_t code_nbytes=libdeflate_deflate_compress_block(c,pdata,dict_size,in_nbytes,is_end_block,
                                                              pcode,block_bound,is_byte_align);
            _check(code_nbytes>0, 32);

            //write the block's code
            int w_ret=full_write(out,pcode,code_nbytes);
            _check(w_ret==0, 33);

            //dict data for next block
            in_cur+=in_nbytes;
            size_t nextDictSize=_dictSize_avail(in_cur);
            memmove(pdata,pdata+dict_size+in_nbytes-nextDictSize,nextDictSize);
        }
    }else{ // compress blocks muti-thread
        TThreadData threadData={c_list,in_crc,block_bound,in,in_size,out,err_code};
        _compress_blocks_mt(&threadData,thread_num,pmem,one_buf_size,workBufCount);
        in_crc=threadData.in_crc;
        err_code=threadData.err_code;
    }
    
    {//gizp foot
        size_t code_nbytes=libdeflate_gzip_compress_foot(in_crc,in_size,pmem,block_bound);
        _check(code_nbytes>0, 23);
        int w_ret=full_write(out,pmem,code_nbytes);
        _check(w_ret==0, 24);
    }

_out:
    if (c_list){
        for (size_t i=0; i<thread_num;i++)
            libdeflate_free_compressor(c_list[i]);
        free(c_list);
    }
    if (pmem) free(pmem);
    return err_code;
}

