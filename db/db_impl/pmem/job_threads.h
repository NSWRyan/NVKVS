#include <pthread.h>
#include <algorithm>
#include <memory>
#include <list>
#include <vector>
#include <time.h>
#include "db/db_impl/pmem/pmem_manager.h"
#include "rocksdb/write_batch.h"
#ifdef _WIN32
#include <Windows.h>
#else
#include <unistd.h>
#endif

#ifndef custom_pmem
#define custom_pmem
#include "db/db_impl/db_impl.h"
#endif

struct MutexLock
{
    MutexLock(pthread_mutex_t& m) : mutex(m)    { pthread_mutex_lock(&mutex); }
    ~MutexLock()                                { pthread_mutex_unlock(&mutex); }
    private:
        pthread_mutex_t&    mutex;
};


struct pointer_passer{
    void *job_threads_handler;
    u_short threadID;
    pointer_passer(u_short ID):threadID(ID){};
    bool writer=false;
    bool timer=false;
    bool gc=false;
};

struct batch_helper{
    vector<rocksdb::WriteBatch*>* v_wb_result=NULL;
    int write_size=0;
    ~batch_helper(){
        delete(v_wb_result);
    }
};

struct batch_job{
    int n_jobs;

    // The size of write batch for the LSM-tree insertion
    int wb_size;

    long total_write_byte=0;

    // new_offset is the new write offset after this batch_job 
    u_long new_offset;
    rocksdb::job_struct** jobs;
    batch_job(int i_n_jobs){
        n_jobs=i_n_jobs;
        wb_size=0;
        total_write_byte=0;
        new_offset=0;
        // No need to malloc now 0111
        //jobs=(rocksdb::job_struct**)malloc(sizeof(rocksdb::job_struct*)*i_n_jobs);
    };
    ~batch_job(){
        // Deprecated 0111 now deleted after copied.
        // Free the memory from the jobs.
        // for(int i=0;i<n_jobs;i++){
        //     delete(jobs[i]);
        // }
        // No need to delete jobs because jobs is now reused for future write batch job. 0111
        //delete(jobs);
    };
};

struct write_threads_data{
    rocksdb::job_struct** buffer=NULL;
    bool status;
    write_threads_data(){
        status=false;
    };
    ~write_threads_data(){
        delete(buffer);
    };
};

// pthreads is a C library the call back must be a C function.
extern "C" void* threadPoolThreadStart(void*);

class job_threads
{
    public:
        job_threads();
        ~job_threads();
        void addWork_write(rocksdb::job_struct *job);
        void addWork_write_batch(rocksdb::WriteBatch *job, u_short dimm);
        void addWork_read(job_pointer *job);
        void workerStart_write(u_short thread_id);
        void workerStart_read();
        void workerStart_gc();
        void timerStart_write();
        int init(u_short threadCount_write, u_short threadCount_read, pmem_manager *pman);

        // Stop all write threads
        void stopThreads();
        // Calculate the % free space of the pmem
        int freespace(u_long cur, u_long gc, u_long max);
        // Direct write by the client thread if true.
        // The default is false
        bool batchedBatch=false;
        // Depreciated
        bool pipelinedWrite=false;
        // The threshold to throttle the client threads.
        int buffer_high_threshold;
        // The threshold to release the throttle.
        int buffer_low_threshold;
        // The number of KV in a single buffer
        int list_capacity=10000000;
        // The timer polling in us.
        int timerus=0;
        // us the thread need to wait before continue.
        int slow_down=0;
        // The number of kv in the current buffer.
        int w_count=0;
        // The totak kv in this session.
        int total_w_count=0;
        // Write lock, the simple one as a guarantee if mutex failed.
        bool b_cond_w=true;
        // Write lock, the simple one as a guarantee if mutex failed.
        bool timer_lock=true;
        // Tell the client thread to slow down if there is a congestion.
        bool throttle=false;
        // The total KV sizes in the current buffer.
        long current_buffer_size;
        // The revolver style buffer for quick swap/reload
        // wtd contains 1 buffer for each write thread 
        write_threads_data* wtd;
        // Threads will re-wait while this is false.
        bool finished;   
        // The number of thread in the waiting room
        u_short wait_count;
        // Enable the batched write to Pmem, this reduces IO.
        // The default is false
        bool memory_buffer=false;
        // Start GC automatically if the free space left is
        // less than this number.
        // The default is 20%
        int gc_auto_trigger_percent=20;
        // Slow down write if free space is less than gc_throttle.
        // gc_throttle should be lower than gc_auto_trigger_percent
        // Default is 10%
        int gc_throttle=10;
        // Manual GC enable, 0 for off, 1 for always on, default is false
        bool manual_gc=false;
        // Set this to true to run the GC thread
        bool gc_run=false;
        // How much % should the GC do at a time
        // The default is 10% of the max
        int gc_how_much=10;
        // GC write batch size
        int gc_wb_size=200;
        // The current free_space of the pmem
        int free_space=100;
        bool print_debug=true;

    private:
        friend void* job_threads_Start(void*);
        bool initiated;

        batch_job* getJob_w(u_short thread_id);
        job_pointer* getJob_r();
        pmem_manager *this_pman;
        pthread_mutex_t     mutex_w;      // A lock so that we can sequence accesses.
        pthread_mutex_t     mutex_r;      // A lock so that we can sequence accesses.
        pthread_cond_t      cond_w;       // The condition variable that is used to hold worker threads.
        pthread_cond_t      cond_r;       // The condition variable that is used to hold worker threads.
        rocksdb::job_struct**     workQueue_w;  // A queue of jobs.
        int workQueue_w_count=0;
        std::list<rocksdb::WriteBatch*>     workQueue_w_batch;  // A queue of jobs.
        std::list<job_pointer*>     workQueue_r;  // A queue of jobs.
        pthread_t timer_thread;
        pthread_t gc_thread;
        std::vector<pthread_t>threads_write;
        std::vector<pthread_t>threads_read;
        rocksdb::WriteOptions wo;
        bool timer_alive=false;
};

