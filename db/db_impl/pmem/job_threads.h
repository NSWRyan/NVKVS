#include <pthread.h>
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
        void timerStart_write();
        int init(u_short threadCount_write, u_short threadCount_read, pmem_manager *pman);

        // Stop all write threads
        void stopThreads();
        bool batchedBatch=false;
        bool pipelinedWrite=false;
        int buffer_high_threshold;
        int buffer_low_threshold;
        int list_capacity=10000000;
        int timerus=0;
        int slow_down=0;
        int w_count=0;
        int total_w_count=0;
        bool b_cond_w=true;
        bool timer_lock=true;
        bool throttle=false;
        long current_buffer_size;
        write_threads_data* wtd;
        bool finished;   // Threads will re-wait while this is true.
        u_short wait_count;
        bool memory_buffer=false;

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
        std::vector<pthread_t>threads_write;
        std::vector<pthread_t>threads_read;
        rocksdb::WriteOptions wo;
        bool timer_alive=false;
};

