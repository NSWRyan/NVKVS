#include <pthread.h>
#include <memory>
#include <list>
#include <vector>

#include "db/db_impl/pmem/pmem_manager.h"
#include "rocksdb/write_batch.h"
#ifdef _WIN32
#include <Windows.h>
#else
#include <unistd.h>
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
    bool writer;
};

// pthreads is a C library the call back must be a C function.
extern "C" void* threadPoolThreadStart(void*);


class job_threads
{

    public:
        job_threads();
        ~job_threads();

        void addWork_write(rocksdb::job_struct *job);
        void addWork_read(job_pointer *job);
        void workerStart_write();
        void workerStart_read();
        int init(u_short threadCount_write, u_short threadCount_read, pmem_manager *pman);

        // Stop all write threads
        void stopThreads();


    private:
        friend void* job_threads_Start(void*);
        bool initiated;

        rocksdb::job_struct* getJob_w();
        job_pointer* getJob_r();
        pmem_manager *this_pman;
        bool                finished;   // Threads will re-wait while this is true.
        pthread_mutex_t     mutex_w;      // A lock so that we can sequence accesses.
        pthread_mutex_t     mutex_r;      // A lock so that we can sequence accesses.
        pthread_cond_t      cond_w;       // The condition variable that is used to hold worker threads.
        pthread_cond_t      cond_r;       // The condition variable that is used to hold worker threads.
        std::list<rocksdb::job_struct*>     workQueue_w;  // A queue of jobs.
        std::list<job_pointer*>     workQueue_r;  // A queue of jobs.
        std::vector<pthread_t>threads_write;
        std::vector<pthread_t>threads_read;
};

