#include <pthread.h>
#include <memory>
#include <list>
#include <vector>


#include "db/db_impl/pmem/wk_manager.h"
#ifdef _WIN32
#include <Windows.h>
#else
#include <unistd.h>
#endif

struct MutexLock_wk
{
    MutexLock_wk(pthread_mutex_t& m) : mutex(m)    { pthread_mutex_lock(&mutex); }
    ~MutexLock_wk()                                { pthread_mutex_unlock(&mutex); }
    private:
        pthread_mutex_t&    mutex;
};


struct pointer_passer_wk{
    void *wk_job_threads_handler;
    u_short threadID;
    pointer_passer_wk(u_short ID):threadID(ID){};
    bool writer;
};

// pthreads is a C library the call back must be a C function.
extern "C" void* threadPoolThreadStart_wk(void*);


class wk_job_threads
{

    public:
        
        wk_job_threads();
        ~wk_job_threads();

        void addWork_write(job_struct *job);
        void addWork_read(job_struct_read *job);
        void workerStart_write(u_short threadID);
        void workerStart_read(u_short threadID);
        int init(u_short threadCount_write, u_short threadCount_read, wk_manager *pman);

        // Stop all write threads
        void stopThreads();


    private:
        bool initiated;
        friend void* wk_job_threads_Start(void*);


        job_struct* getJob_w();
        job_struct_read* getJob_r();
        wk_manager *this_wkman;
        bool                finished;   // Threads will re-wait while this is true.
        pthread_mutex_t     mutex_w;      // A lock so that we can sequence accesses.
        pthread_mutex_t     mutex_r;      // A lock so that we can sequence accesses.
        pthread_cond_t      cond_w;       // The condition variable that is used to hold worker threads.
        pthread_cond_t      cond_r;       // The condition variable that is used to hold worker threads.
        std::list<job_struct*>     workQueue_w;  // A queue of jobs.
        std::list<job_struct_read*>     workQueue_r;  // A queue of jobs.
        std::vector<pthread_t>threads_write;
        std::vector<pthread_t>threads_read;
};

