#include "wk_job_threads.h"

using namespace std;

//This implementation is based on this sweet code by Martin York
//https://stackoverflow.com/questions/5799924/thread-wait-for-parent

wk_job_threads::wk_job_threads(){
    initiated=false;
}
// Constructor, create the thread pool
int wk_job_threads::init(u_short threadCount_write, u_short threadCount_read, wk_manager *wkman)
{
    // Init phase
    finished=false;
    threads_write.resize(threadCount_write);
    threads_read.resize(threadCount_read);
    this_wkman=wkman;

    // Write part
    // If we fail creating either pthread object then throw a fit.
    if (pthread_mutex_init(&mutex_w, NULL) != 0)
    {   throw int(1);
    }

    if (pthread_cond_init(&cond_w, NULL) != 0)
    {
        pthread_mutex_destroy(&mutex_w);
        throw int(2);
    }

    for(u_short loop=0; loop < threadCount_write;++loop)
    {
       pointer_passer_wk *pass_this=new pointer_passer_wk(loop);
       pass_this->writer=true;
       pass_this->wk_job_threads_handler=reinterpret_cast<void*>(this);
       if (pthread_create(&threads_write[loop], NULL, threadPoolThreadStart_wk, pass_this) != 0)
       {
            // One thread failed: clean up
            for(unsigned int kill = loop -1; kill < loop /*unsigned will wrap*/;--kill)
            {
                pthread_cancel(threads_write[kill]);
                pthread_join(threads_write[kill],NULL);
            }
            throw int(3);
       }
    }

    // Read part
    // If we fail creating either pthread object then throw a fit.
    if (pthread_mutex_init(&mutex_r, NULL) != 0)
    {   throw int(1);
    }

    if (pthread_cond_init(&cond_r, NULL) != 0)
    {
        pthread_mutex_destroy(&mutex_r);
        throw int(2);
    }

    for(u_short loop=0; loop < threadCount_read;++loop)
    {
       pointer_passer_wk *pass_this=new pointer_passer_wk(loop);
       pass_this->writer=false;
       pass_this->wk_job_threads_handler=reinterpret_cast<void*>(this);
       if (pthread_create(&threads_read[loop], NULL, threadPoolThreadStart_wk, pass_this) != 0)
       {
            // One thread failed: clean up
            for(unsigned int kill = loop -1; kill < loop /*unsigned will wrap*/;--kill)
            {
                pthread_cancel(threads_read[kill]);
                pthread_join(threads_read[kill],NULL);
            }
            throw int(3);
       }
    }

    initiated=true;
    return 0;
}

//Destructor
wk_job_threads::~wk_job_threads()
{   
    if(initiated){
        //////////////////////////////////////////////////////////////////////////
        // Write part
        //////////////////////////////////////////////////////////////////////////
        finished = true;
        for(std::vector<pthread_t>::iterator loop = threads_write.begin();loop != threads_write.end(); ++loop)
        {
            // Send enough signals to free all threads.
            pthread_cond_signal(&cond_w);
        }

        for(std::vector<pthread_t>::iterator loop = threads_write.begin();loop != threads_write.end(); ++loop)
        {
            // Wait for all threads to exit (they will as finished is true and
            //                               we sent enough signals to make sure
            //                               they are running).
            void*  result;
            pthread_join(*loop, &result);
        }
        // Destroy the pthread objects.
        pthread_cond_destroy(&cond_w);
        pthread_mutex_destroy(&mutex_w);

        // Delete all re-maining jobs.
        // Notice how we took ownership of the jobs.
        for(std::list<job_struct*>::const_iterator loop = workQueue_w.begin(); loop != workQueue_w.end();++loop)
        {
            delete *loop;
        }
        
        //////////////////////////////////////////////////////////////////////////
        //Read part
        //////////////////////////////////////////////////////////////////////////
        for(std::vector<pthread_t>::iterator loop = threads_read.begin();loop != threads_read.end(); ++loop)
        {
            // Send enough signals to free all threads.
            pthread_cond_signal(&cond_r);
        }
        for(std::vector<pthread_t>::iterator loop = threads_read.begin();loop != threads_read.end(); ++loop)
        {
            // Wait for all threads to exit (they will as finished is true and
            //                               we sent enough signals to make sure
            //                               they are running).
            void*  result;
            pthread_join(*loop, &result);
        }
        // Destroy the pthread objects.
        pthread_cond_destroy(&cond_r);
        pthread_mutex_destroy(&mutex_r);

        // Delete all re-maining jobs.
        // Notice how we took ownership of the jobs.
        for(std::list<job_struct_read*>::const_iterator loop = workQueue_r.begin(); loop != workQueue_r.end();++loop)
        {
            delete *loop;
        }
    }
}

// Add a new job to the queue
// Signal the condition variable. This will flush a waiting worker
// otherwise the job will wait for a worker to finish processing its current job.
void wk_job_threads::addWork_write(job_struct *job)
{
    MutexLock_wk  lock(mutex_w);
    workQueue_w.push_back(job);
    pthread_cond_signal(&cond_w);
}

void wk_job_threads::addWork_read(job_struct_read *job)
{
    MutexLock_wk  lock(mutex_r);
    workQueue_r.push_back(job);
    pthread_cond_signal(&cond_r);
}

// This is the main worker loop for write.
void wk_job_threads::workerStart_write(u_short threadID)
{
    while(!finished)
    {
        job_struct* job = getJob_w();
        if(job!=NULL){
            job->offset=this_wkman->insertNT(job->key,job->key_length,job->value,job->value_length,threadID);
            job->threadID=threadID;
            job->status=true;
        }
    }
}

// This is the main worker loop for read.
void wk_job_threads::workerStart_read(u_short threadID)
{
    while(!finished)
    {
        job_struct_read* job = getJob_r();
        if(job!=NULL){
            this_wkman->readSTNC(job,threadID);
            job->status=true;
        }
    }
}

// Get a new job for the write thread
job_struct* wk_job_threads::getJob_w()
{
    MutexLock_wk  lock(mutex_w);

    while((workQueue_w.empty()) && (!finished))
    {
        pthread_cond_wait(&cond_w, &mutex_w);
        // The wait releases the mutex lock and suspends the thread (until a signal).
        // When a thread wakes up it is help until it can acquire the mutex so when we
        // get here the mutex is again locked.
        //
        // Note: You must use while() here. This is because of the situation.
        //   Two workers:  Worker A processing job A.
        //                 Worker B suspended on condition variable.
        //   Parent adds a new job and calls signal.
        //   This wakes up thread B. But it is possible for Worker A to finish its
        //   work and lock the mutex before the Worker B is released from the above call.
        //
        //   If that happens then Worker A will see that the queue is not empty
        //   and grab the work item in the queue and start processing. Worker B will
        //   then lock the mutext and proceed here. If the above is not a while then
        //   it would try and remove an item from an empty queue. With a while it sees
        //   that the queue is empty and re-suspends on the condition variable above.
    }
    job_struct*  result=NULL;
    if (!finished)
    {    result=(workQueue_w.front());
         workQueue_w.pop_front();
    }
    return result;
}

// Get a new job for the read thread
job_struct_read* wk_job_threads::getJob_r()
{
    MutexLock_wk  lock(mutex_r);

    while((workQueue_r.empty()) && (!finished))
    {
        pthread_cond_wait(&cond_r, &mutex_r);
        // The wait releases the mutex lock and suspends the thread (until a signal).
        // When a thread wakes up it is help until it can acquire the mutex so when we
        // get here the mutex is again locked.
        //
        // Note: You must use while() here. This is because of the situation.
        //   Two workers:  Worker A processing job A.
        //                 Worker B suspended on condition variable.
        //   Parent adds a new job and calls signal.
        //   This wakes up thread B. But it is possible for Worker A to finish its
        //   work and lock the mutex before the Worker B is released from the above call.
        //
        //   If that happens then Worker A will see that the queue is not empty
        //   and grab the work item in the queue and start processing. Worker B will
        //   then lock the mutext and proceed here. If the above is not a while then
        //   it would try and remove an item from an empty queue. With a while it sees
        //   that the queue is empty and re-suspends on the condition variable above.
    }
    job_struct_read*  result=NULL;
    if (!finished)
    {    result=(workQueue_r.front());
         workQueue_r.pop_front();
    }
    return result;
}

void* threadPoolThreadStart_wk(void* data)
{
    pointer_passer_wk *ps = reinterpret_cast<pointer_passer_wk*>(data);
    wk_job_threads *jt=reinterpret_cast<wk_job_threads*>(ps->wk_job_threads_handler);
    try
    {
        u_short threadID=ps->threadID;
        if(ps->writer){
            //cout<<"Write Thread started "<<threadID<<endl;
            jt->workerStart_write(threadID);
        }else{
            //cout<<"Read Thread started "<<threadID<<endl;
            jt->workerStart_read(threadID);
        }
        delete ps;
    }
    catch(...){}
    return NULL;
}