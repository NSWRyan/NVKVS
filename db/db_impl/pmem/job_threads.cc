#include "db/db_impl/pmem/job_threads.h"

using namespace std;

//This implementation is based on this sweet code by Martin York
//https://stackoverflow.com/questions/5799924/thread-wait-for-parent

job_threads::job_threads(){
    initiated=false;
}

// Constructor, create the thread pool
int job_threads::init(u_short threadCount_write, u_short threadCount_read, pmem_manager *pman)
{
    // Init phase
    finished=false;
    threads_write.resize(threadCount_write);
    threads_read.resize(threadCount_read);
    this_pman=pman;
    throttle=false;
    timer_lock=false;
    slow_down=1000;
    buffer_high_threshold=1000000000;
    buffer_low_threshold=500000000;
    list_capacity=1000000;
    timerus=1000;
    current_buffer_size=0;
    // Allocate the buffers
    workQueue_w=(rocksdb::job_struct**)malloc(sizeof(rocksdb::job_struct*)*list_capacity);
    wtd=new write_threads_data[threadCount_write];
    for(int i=0;i<threadCount_write;i++){
        wtd[i].buffer=(rocksdb::job_struct**)malloc(sizeof(rocksdb::job_struct*)*list_capacity);
    }
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

    // Start timer thread
    {
        pointer_passer *pass_this=new pointer_passer(0);
        pass_this->timer=true;
        pass_this->job_threads_handler=reinterpret_cast<void*>(this);
        if (pthread_create(&timer_thread, NULL, threadPoolThreadStart, pass_this) != 0)
        {
             pthread_cancel(timer_thread);
             pthread_join(timer_thread,NULL);
             throw int(3);
        }
        timer_alive=true;
    }

    for(u_short loop=0; loop < threadCount_write;++loop)
    {
        pointer_passer *pass_this=new pointer_passer(loop);
        pass_this->writer=true;
        pass_this->job_threads_handler=reinterpret_cast<void*>(this);
        if (pthread_create(&threads_write[loop], NULL, threadPoolThreadStart, pass_this) != 0)
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
       pointer_passer *pass_this=new pointer_passer(loop);
       pass_this->writer=false;
       pass_this->job_threads_handler=reinterpret_cast<void*>(this);
       if (pthread_create(&threads_read[loop], NULL, threadPoolThreadStart, pass_this) != 0)
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
    wo.disableWAL=true;
    initiated=true;

    return 0;
}

//Destructor
job_threads::~job_threads()
{
    if(initiated){
        finished = true;
        usleep(timerus);

        while(workQueue_w_batch.size()!=0){
            // Wait till job is done
            cout<<"";
        }

        while(workQueue_r.size()!=0){
            // Wait till job is done
            cout<<"";
        }
        timer_alive=false;
        {
            void*  result;
            pthread_join(timer_thread, &result);
        }

        //////////////////////////////////////////////////////////////////////////
        // Write part
        //////////////////////////////////////////////////////////////////////////
        for(std::vector<pthread_t>::iterator loop = threads_write.begin();loop != threads_write.end(); ++loop)
        {
            // Send enough signals to free all threads.
            pthread_cond_signal(&cond_w);
        }

        while(w_count!=0){
            // Wait till job is done
            cout<<"";
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
        for(std::list<rocksdb::WriteBatch*>::const_iterator loop = workQueue_w_batch.begin(); loop != workQueue_w_batch.end();++loop)
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
        for(std::list<job_pointer*>::const_iterator loop = workQueue_r.begin(); loop != workQueue_r.end();++loop)
        {
            delete *loop;
        }

        delete[](wtd);
        delete(workQueue_w);
        initiated=false;
    }
}

// Add a new job to the queue
// Signal the condition variable. This will flush a waiting worker
// otherwise the job will wait for a worker to finish processing its current job.
void job_threads::addWork_write(rocksdb::job_struct *job)
{
    MutexLock lock(mutex_w);
    b_cond_w=true;
    // Insert into the buffer
    workQueue_w[w_count]=job;
    current_buffer_size+=job->total_length;
    job->offset=this_pman->current_offset.offset_current;
    this_pman->current_offset.offset_current+=job->total_length;
    job->status=true;
    w_count++;
    b_cond_w=false;
    if(throttle){
        // Sleep for 1000 ms when insert is too slow
        job->throttle=true;
    }
}

// Add a new job to the queue
// Signal the condition variable. This will flush a waiting worker
// otherwise the job will wait for a worker to finish processing its current job.
void job_threads::addWork_write_batch(rocksdb::WriteBatch *job)
{
    MutexLock lock(mutex_w);
    b_cond_w=true;
    workQueue_w_batch.push_back(job);
    pthread_cond_signal(&cond_w);
    b_cond_w=false;
}

void job_threads::addWork_read(job_pointer *job)
{
    MutexLock lock(mutex_r);
    workQueue_r.push_back(job);
    pthread_cond_signal(&cond_r);
}

// This is the main worker loop for write.
void job_threads::workerStart_write(u_short thread_id)
{
    while(!finished)
    {
        {
            batch_job* jobs = getJob_w(thread_id);
            if(jobs!=NULL){
                //rocksdb::WriteBatch wb_in(jobs->wb_size);
                for(int i=0;i<jobs->n_jobs;i++){
                    // Write into PMem
                    this_pman->insertJS(jobs->jobs[i]);
                    // Insert into write batch for the LSM-tree
                    //wb_in.Put2(jobs->jobs[i]->key,string((char*)(&(jobs->jobs[i]->offset)),8));
                }
                // Write into the LSM-tree
                //wb_in.pmem_init=true;
                //DBI->Write(wo,&wb_in);

                // Persist the data
                long start_offset=jobs->jobs[0]->offset;
                this_pman->persist(start_offset,jobs->total_write_byte);

                // Manage the offset in PMem if not managed yet
                if(this_pman->offsets[2]<jobs->new_offset){
                    this_pman->offsets[2]=jobs->new_offset;
                }

                delete(jobs);
                
            }
        }
        {
            if(finished&&thread_id==0&&w_count>0){
                batch_job* jobs = getJob_w(thread_id);
                if(jobs!=NULL){
                    //rocksdb::WriteBatch wb_in(jobs->wb_size);
                    for(int i=0;i<jobs->n_jobs;i++){
                        // Write into PMem
                        this_pman->insertJS(jobs->jobs[i]);
                        // Insert into write batch for the LSM-tree
                        //wb_in.Put2(jobs->jobs[i]->key,string((char*)(&(jobs->jobs[i]->offset)),8));
                    }
                    // Write into the LSM-tree
                    //wb_in.pmem_init=true;
                    //DBI->Write(wo,&wb_in);

                    // Persist the data
                    long start_offset=jobs->jobs[0]->offset;
                    this_pman->persist(start_offset,jobs->total_write_byte);

                    // Manage the offset in PMem if not managed yet
                    if(this_pman->offsets[2]<jobs->new_offset){
                        this_pman->offsets[2]=jobs->new_offset;
                    }
                    delete(jobs);
                }
            }
        }
    }
}

// This is the main worker loop for read.
void job_threads::workerStart_read()
{
    while(!finished)
    {
        job_pointer* job = getJob_r();
        if(job!=NULL){
            this_pman->readSTNC(job);
            job->status=true;
        }
    }
}


// This is the timer loop for write.
void job_threads::timerStart_write()
{
    while(timer_alive)
    {
        usleep(timerus);
        while (b_cond_w){
            if(finished){
                break;
            }
            cout<<"";
        }
        // Throttle write when buffer is full
        if(current_buffer_size>buffer_high_threshold){
            throttle=true;
        }else if(throttle && current_buffer_size<=buffer_low_threshold){
            throttle=false;
        }
        timer_lock=true;
        pthread_cond_signal(&cond_w);
    }
}

// Get a new job for the write thread
batch_job* job_threads::getJob_w(u_short thread_id)
{
    int t_w_count=0;
    long t_current_buffer_size=0;
    //long start_offset=0;
    //long new_offset=0;
    {   // Lock region, do as few as possible here
        MutexLock lock(mutex_w);
        b_cond_w=true;
        while((!timer_lock) && (!finished))
        {
            b_cond_w=false;
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
        // Relock the access to prevent other worker from accessing
        // Lock timer from waking up other threads
        timer_lock=false;
        b_cond_w=true;
        // Replace the main buffer with this thread buffer
        if(w_count>0){
            rocksdb::job_struct** temp=workQueue_w;
            workQueue_w=wtd[thread_id].buffer;
            wtd[thread_id].buffer=temp;
            t_w_count=w_count;
            t_current_buffer_size=current_buffer_size;
            temp=NULL;

            // Offset management
            /*
            start_offset=this_pman->current_offset.offset_current;
            this_pman->current_offset.offset_current+=current_buffer_size;
            new_offset=this_pman->current_offset.offset_current;*/

            //Reset the writes
            current_buffer_size=0;
            w_count=0;
        }
        // Release the write lock
        b_cond_w=false;
    }   // Lock region end

    batch_job* result=NULL;

    if (t_w_count>0)
    {   
        result=new batch_job(t_w_count);
        result->new_offset=wtd[thread_id].buffer[t_w_count-1]->offset+wtd[thread_id].buffer[t_w_count-1]->total_length;
        result->total_write_byte=t_current_buffer_size;

        for(int i=0;i<t_w_count;i++){
            // Insert into the batch_job
            result->jobs[i]=wtd[thread_id].buffer[i];
            result->wb_size+=wtd[thread_id].buffer[i]->total_length_separated;
        }
    }
    return result;
}

// Get a new job for the read thread
job_pointer* job_threads::getJob_r()
{
    MutexLock  lock(mutex_r);

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
    job_pointer*  result=NULL;
    if (!finished)
    {    result=(workQueue_r.front());
         workQueue_r.pop_front();
    }
    return result;
}

void* threadPoolThreadStart(void* data)
{
    pointer_passer *ps = reinterpret_cast<pointer_passer*>(data);
    job_threads *jt=reinterpret_cast<job_threads*>(ps->job_threads_handler);
    
    try
    {
        if(ps->writer){
            //cout<<"Write Thread started "<<threadID<<endl;
            jt->workerStart_write(ps->threadID);
        }else{
            if(ps->timer){
                jt->timerStart_write();
            }
            //cout<<"Read Thread started "<<threadID<<endl;
            jt->workerStart_read();
        }
        delete ps;
    }
    catch(...){}
    return NULL;
}