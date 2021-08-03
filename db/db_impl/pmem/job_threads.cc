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
    initiated=true;
    return 0;
}

//Destructor
job_threads::~job_threads()
{
    if(initiated){
        pthread_cond_signal(&cond_w);
        while(workQueue_w_batch.size()!=0){
            // Wait till job is done
            cout<<"";
        }
        while(workQueue_r.size()!=0){
            // Wait till job is done
            cout<<"";
        }

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
    }
}

// Add a new job to the queue
// Signal the condition variable. This will flush a waiting worker
// otherwise the job will wait for a worker to finish processing its current job.
// Deprecated and replaced with batch
void job_threads::addWork_write(rocksdb::job_struct *job)
{
    MutexLock  lock(mutex_w);
    workQueue_w.push_back(job);
    pthread_cond_signal(&cond_w);
}

// Add a new job to the queue
// Signal the condition variable. This will flush a waiting worker
// otherwise the job will wait for a worker to finish processing its current job.
void job_threads::addWork_write_batch(rocksdb::WriteBatch *job)
{
    MutexLock  lock(mutex_w);
    workQueue_w_batch.push_back(job);
    pthread_cond_signal(&cond_w);
}

void job_threads::addWork_read(job_pointer *job)
{
    MutexLock  lock(mutex_r);
    workQueue_r.push_back(job);
    pthread_cond_signal(&cond_r);
}

// This is the main worker loop for write.
void job_threads::workerStart_write()
{
    while(!finished)
    {
        batch_helper* jobs = getJob_w();
        // This is the batched write size for the LSM tree
        
        if(jobs!=NULL){
            rocksdb::WriteBatch* wb_compilation = new rocksdb::WriteBatch(jobs->write_size);
        
            // Iterate all WriteBatches
            int jobs_size=jobs->v_wb_result->size();
            // Persist management
            long to_persist_length=0;
            long persist_offset=jobs->v_wb_result->at(0)->offset_start;

            for (int wb_index=0;wb_index<jobs_size;wb_index++){
                // Write batch level
                rocksdb::WriteBatch* temp_wb=jobs->v_wb_result->at(wb_index);
                to_persist_length+=temp_wb->total_write;

                // Offset management
                long start_offset=temp_wb->offset_start;
                int v_size=temp_wb->writebatch_data->size();
                for(int i=0;i<v_size;i++){
                    // Job_struct level
                    rocksdb::job_struct* temp_js=temp_wb->writebatch_data->at(i);

                    // Offset management
                    temp_js->offset=start_offset;
                    start_offset+=temp_js->total_length;
                    wb_compilation->Put2(temp_js->key,
                        string((char*)(&(temp_js->offset)),8));
                }

                // Now write into PMEM
                this_pman->insertBatch(temp_wb);

                // Tell the client that the write is done
                temp_wb->wb_status=true;
            }
            
            wb_compilation->pmem_init=true;
            DBI->Write(rocksdb::WriteOptions(),wb_compilation);
            this_pman->persist(persist_offset,to_persist_length);
            delete(jobs);
            delete(wb_compilation);
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

// Get a new job for the write thread
batch_helper* job_threads::getJob_w()
{
    MutexLock  lock(mutex_w);

    while((workQueue_w_batch.empty()) && (!finished))
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
    batch_helper* result=NULL;

    // WriteBatches                    (v_wb_result)
    //       WriteBatch                (wb_result)
    //              Job_struct         (wb_result->writebatch_data)
    if (!finished)
    {   
        result=new batch_helper();
        result->v_wb_result=new vector<rocksdb::WriteBatch*>();
        long current_offset=this_pman->current_offset.offset_current;
        long total_write=0;

        if(!batchedBatch){
            // Single batch
            if(!workQueue_w_batch.empty()){
                // Obtain the jobs
                rocksdb::WriteBatch* wb_result=workQueue_w_batch.front();
                
                // Offset management
                wb_result->offset_start=current_offset;
                current_offset+=wb_result->total_write;
                total_write+=wb_result->total_write;

                // Insert the wb to the compiled wb
                result->v_wb_result->push_back(wb_result);
                result->write_size+=wb_result->total_write_rdb;

                // Remove the job from the list
                workQueue_w_batch.pop_front();
            }
        }else{
            // Multiple batch
            while((!workQueue_w_batch.empty())&&total_write<batchSize){
                // Obtain the jobs
                rocksdb::WriteBatch* wb_result=workQueue_w_batch.front();
                
                // Offset management
                wb_result->offset_start=current_offset;
                current_offset+=wb_result->total_write;
                total_write+=wb_result->total_write;

                // Insert the wb to the compiled wb
                result->v_wb_result->push_back(wb_result);
                result->write_size+=wb_result->total_write_rdb;

                // Remove the job from the list
                workQueue_w_batch.pop_front();
            }
        }
        // PMEM offset management
        this_pman->current_offset.offset_current += total_write;
        this_pman->offsets[2]=this_pman->current_offset.offset_current;
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
            jt->workerStart_write();
        }else{
            //cout<<"Read Thread started "<<threadID<<endl;
            jt->workerStart_read();
        }
        delete ps;
    }
    catch(...){}
    return NULL;
}