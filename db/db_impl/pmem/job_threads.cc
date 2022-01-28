#include "db/db_impl/pmem/job_threads.h"

using namespace std;

// This implementation is based on this sweet code by Martin York
// https://stackoverflow.com/questions/5799924/thread-wait-for-parent

job_threads::job_threads() { initiated = false; }

// Constructor, create the thread pool
int job_threads::init(u_short threadCount_write, u_short threadCount_read,
                      pmem_manager* pman) {
  // Init phase
  finished = false;
  threads_write.resize(threadCount_write);
  threads_read.resize(threadCount_read);
  this_pman = pman;
  throttle = false;
  timer_lock = false;
  slow_down = 1000;
  buffer_high_threshold = 100000000;
  buffer_low_threshold = buffer_high_threshold / 2;
  list_capacity = 1000000;
  timerus = 1000;
  wait_count = 0;
  current_buffer_size = 0;
  wo=rocksdb::WriteOptions();
  wo.disableWAL=true;
  ro=rocksdb::ReadOptions();
  // Allocate the buffers
  workQueue_w = (rocksdb::job_struct**)malloc(sizeof(rocksdb::job_struct*) *
                                              list_capacity);
  wtd = new write_threads_data[threadCount_write];
  for (int i = 0; i < threadCount_write; i++) {
    wtd[i].buffer = (rocksdb::job_struct**)malloc(sizeof(rocksdb::job_struct*) *
                                                  list_capacity);
  }
  // Write part
  // If we fail creating either pthread object then throw a fit.
  if (pthread_mutex_init(&mutex_w, NULL) != 0) {
    throw int(1);
  }

  if (pthread_cond_init(&cond_w, NULL) != 0) {
    pthread_mutex_destroy(&mutex_w);
    throw int(2);
  }

  // Start timer thread
  {
    pointer_passer* pass_this = new pointer_passer(0);
    pass_this->timer = true;
    pass_this->job_threads_handler = reinterpret_cast<void*>(this);
    if (pthread_create(&timer_thread, NULL, threadPoolThreadStart, pass_this) !=
        0) {
      pthread_cancel(timer_thread);
      pthread_join(timer_thread, NULL);
      throw int(3);
    }
    timer_alive = true;
  }

  // The GC thread
  {
    pointer_passer* pass_this = new pointer_passer(0);
    pass_this->gc = true;
    pass_this->job_threads_handler = reinterpret_cast<void*>(this);
    if (pthread_create(&gc_thread, NULL, threadPoolThreadStart, pass_this) !=
        0) {
      pthread_cancel(gc_thread);
      pthread_join(gc_thread, NULL);
      throw int(3);
    }
  }

  for (u_short loop = 0; loop < threadCount_write; ++loop) {
    pointer_passer* pass_this = new pointer_passer(loop);
    pass_this->writer = true;
    pass_this->job_threads_handler = reinterpret_cast<void*>(this);
    if (pthread_create(&threads_write[loop], NULL, threadPoolThreadStart,
                       pass_this) != 0) {
      // One thread failed: clean up
      for (unsigned int kill = loop - 1; kill < loop /*unsigned will wrap*/;
           --kill) {
        pthread_cancel(threads_write[kill]);
        pthread_join(threads_write[kill], NULL);
      }
      throw int(3);
    }
  }

  // Read part
  // If we fail creating either pthread object then throw a fit.
  if (pthread_mutex_init(&mutex_r, NULL) != 0) {
    throw int(1);
  }

  if (pthread_cond_init(&cond_r, NULL) != 0) {
    pthread_mutex_destroy(&mutex_r);
    throw int(2);
  }

  for (u_short loop = 0; loop < threadCount_read; ++loop) {
    pointer_passer* pass_this = new pointer_passer(loop);
    pass_this->writer = false;
    pass_this->job_threads_handler = reinterpret_cast<void*>(this);
    if (pthread_create(&threads_read[loop], NULL, threadPoolThreadStart,
                       pass_this) != 0) {
      // One thread failed: clean up
      for (unsigned int kill = loop - 1; kill < loop /*unsigned will wrap*/;
           --kill) {
        pthread_cancel(threads_read[kill]);
        pthread_join(threads_read[kill], NULL);
      }
      throw int(3);
    }
  }
  wo.disableWAL = true;
  initiated = true;
  print_debug=false;

  return 0;
}

// Destructor
job_threads::~job_threads() {
  if (initiated) {
    if(print_debug)
        std::cout << "Closing start" << std::endl;
    finished = true;
    usleep(timerus);

    // Deprecated 0114
    // while (workQueue_w_batch.size() != 0) {
    //   // Wait till job is done
    //   cout << "";
    // }
    // while (workQueue_r.size() != 0) {
    //   // Wait till job is done
    //   cout << "";
    // }
    timer_alive = false;
    {
      void* result;
      pthread_join(timer_thread, &result);
    }

    //////////////////////////////////////////////////////////////////////////
    // Write part
    //////////////////////////////////////////////////////////////////////////
    for (std::vector<pthread_t>::iterator loop = threads_write.begin();
         loop != threads_write.end(); ++loop) {
      // Send enough signals to free all threads.
      pthread_cond_signal(&cond_w);
    }

    while (w_count != 0) {
      // Wait till job is done
      cout << "";
    }
    for (std::vector<pthread_t>::iterator loop = threads_write.begin();
         loop != threads_write.end(); ++loop) {
      // Wait for all threads to exit (they will as finished is true and
      //                               we sent enough signals to make sure
      //                               they are running).
      void* result;
      pthread_join(*loop, &result);
    }
    // Destroy the pthread objects.
    pthread_cond_destroy(&cond_w);
    pthread_mutex_destroy(&mutex_w);

    // Delete all re-maining jobs.
    // Notice how we took ownership of the jobs.
    for (std::list<rocksdb::WriteBatch*>::const_iterator loop =
             workQueue_w_batch.begin();
         loop != workQueue_w_batch.end(); ++loop) {
      delete *loop;
    }

    //////////////////////////////////////////////////////////////////////////
    // Read part
    //////////////////////////////////////////////////////////////////////////
    for (std::vector<pthread_t>::iterator loop = threads_read.begin();
         loop != threads_read.end(); ++loop) {
      // Send enough signals to free all threads.
      pthread_cond_signal(&cond_r);
    }
    for (std::vector<pthread_t>::iterator loop = threads_read.begin();
         loop != threads_read.end(); ++loop) {
      // Wait for all threads to exit (they will as finished is true and
      //                               we sent enough signals to make sure
      //                               they are running).
      void* result;
      pthread_join(*loop, &result);
    }
    // Destroy the pthread objects.
    pthread_cond_destroy(&cond_r);
    pthread_mutex_destroy(&mutex_r);

    // Delete all re-maining jobs.
    // Notice how we took ownership of the jobs.
    for (std::list<job_pointer*>::const_iterator loop = workQueue_r.begin();
         loop != workQueue_r.end(); ++loop) {
      delete *loop;
    }

    if(print_debug)
        cout << "total write " << total_w_count << endl;
    delete[](wtd);
    delete (workQueue_w);
    initiated = false;
  }
}

// Add a new job to the queue
// Signal the condition variable. This will flush a waiting worker
// otherwise the job will wait for a worker to finish processing its current
// job.
void job_threads::addWork_write(rocksdb::job_struct* job) {
  char* rotate=job->whole_data;
  rotate[job->total_length-1]=0;
  {
    MutexLock lock(mutex_w);
    b_cond_w = true;
    if (batchedBatch) {
      // Insert into the buffer
      // Overflow rotation
      if ((this_pman->current_offset.offset_current + job->total_length) >=
          this_pman->current_offset.offset_max) {
        // Rotating, reset the cursor to start
        this_pman->current_offset.offset_current =
          this_pman->current_offset.offset_start;
        rotate[job->total_length-1]=1;
      }
      job->offset = this_pman->current_offset.offset_current;
      this_pman->current_offset.offset_current += job->total_length;
      workQueue_w[w_count] = job;
      // current_buffer_size += job->total_length;
      // w_count++;
      // total_w_count++;
    } else {
      // Manual insertion by thread
      // Very slow
      this_pman->insertJS(job);
    }
    b_cond_w = false;
  }
  if (throttle) {
    // Sleep for 1000 us when insert is too slow
    for(int i =0; i< 10000;i++)
      std::cout<<"";
  }
}

// Add a new job to the queue
// Signal the condition variable. This will flush a waiting worker
// otherwise the job will wait for a worker to finish processing its current
// job.
void job_threads::addWork_write_batch(rocksdb::WriteBatch* job, u_short dimm) {
  MutexLock lock(mutex_w);
  b_cond_w = true;
  for (rocksdb::job_struct* job_j : job->writebatch_data) {
    job_j->offset = this_pman->current_offset.offset_current;
    this_pman->current_offset.offset_current += job_j->total_length;
    workQueue_w[w_count] = job_j;
    current_buffer_size += job->total_write;
    job_j->dimm = dimm;
    w_count++;
    total_w_count++;
  }
  b_cond_w = false;
}

// Deprecated
void job_threads::addWork_read(job_pointer* job) {
  MutexLock lock(mutex_r);
  workQueue_r.push_back(job);
  pthread_cond_signal(&cond_r);
}

// This is the timer loop for write.
void job_threads::timerStart_write() {
  timer_alive=false;
  while (timer_alive) {
    usleep(timerus);
    // Timer lock deprecated 0113
    // while (b_cond_w){
    //     cout<<"";
    // }

    if (finished) {
      break;
    }
    // Always run the GC if this is true
    if(manual_gc){
        gc_run=true;
    }
    // Throttle write when buffer is full
    if (wait_count > 0) {
      // Some workers are idle, so wake them up
      pthread_cond_signal(&cond_w);
    } else {
        // No available worker so possible congestion
        // Check GC first
        free_space=freespace(this_pman->current_offset.offset_current,
                    this_pman->current_offset.offset_gc,
                    this_pman->current_offset.offset_max);
        if (free_space < gc_auto_trigger_percent) {
            // Free space reached the limit, run GC
            // Trigger GC here.
            gc_run=true;
            if(free_space < gc_throttle){
                throttle = true;
            }
        }

        if (current_buffer_size > buffer_high_threshold) {
            // Check if max memory is over the limit
            throttle = true;
        }
    }

    // Throttle release
    if(free_space > gc_throttle){
        if (current_buffer_size < buffer_low_threshold) {
            // Release the throttle if the memory is back to normal
            throttle = false;
        }
    }

    // Do not change this.
    timer_lock = true;
  }
  if(print_debug)
      cout<<this_pman->pmem_dir<<" Timer done."<<endl; 
}

// The GC threads
void job_threads::workerStart_gc() {
    while(!finished){
        usleep(timerus);
        if(gc_run){
            std::cout<<"GC online."<<std::endl;
            // GC job here
            u_long size_to_GC = 
                std::min(
                    // The calculated space to be freed from how_much
                    this_pman->current_offset.offset_max*gc_how_much/100
                    ,
                    // The current space used by the 
                    this_pman->current_offset.offset_max*(100-free_space)/100
                );
            if(size_to_GC>0){
                // GC code here
                u_long this_offset=this_pman->current_offset.offset_gc;
                // The total size in bytes for the current gc session
                u_long session_gc_size=0;
                // The total number of KV in the current wb.
                int wb_kv_count=0;
                rocksdb::WriteBatch wb(10000000);
                while(this_offset!=this_pman->current_offset.offset_current&&session_gc_size<size_to_GC){
                    u_short* header=(u_short*)(this_pman->pmem_addr+this_offset);
                    char* key_offset=this_pman->pmem_addr+this_offset+4;
                    char* value_offset=key_offset+header[0];
                    int total_length=4+header[0]+header[1]+1;
                    // cout<<string(key_offset,header[0])<<endl;
                    // cout<<string(value_offset,header[1])<<endl;

                    // Rotation char is value_offset[header1]
                    if(this_offset>this_pman->current_offset.offset_current&&value_offset[header[1]]==1){
                        // Reset the GC cursor
                        this_offset=this_pman->current_offset.offset_start;
                    }else{
                        this_offset+=total_length;
                    }

                    string value;
                    this_pman->DBI->Get(ro,rocksdb::Slice(key_offset,header[0]),&value);
                    // Only add if the value is still valid in the LSM tree.
                    if(strncmp(value.c_str(),value_offset,header[1])==0){
                      // The KV is still the latest version/valid.
                      wb.Put2(rocksdb::Slice(key_offset,header[0]),rocksdb::Slice(value_offset,header[1]));
                      wb_kv_count++;
                    }

                    if(wb_kv_count>gc_wb_size){
                          this_pman->DBI->Write(wo,&wb);
                          this_pman->current_offset.offset_gc=this_offset;
                          wb_kv_count=0;
                          wb=rocksdb::WriteBatch(10000000); 
                    }
                    session_gc_size+=total_length;
                    if(finished){
                      break;
                    }
                }
                if(wb_kv_count>0){
                    this_pman->DBI->Write(rocksdb::WriteOptions(),&wb);
                    this_pman->current_offset.offset_gc=this_offset;
                    wb_kv_count=0;
                    wb=rocksdb::WriteBatch(10000000); 
                }

                std::cout<<"GC offline."<<std::endl;
                // read

                // Insert to write batch
                // write the write batch to the LSM tree 
            }
            // GC finished
            gc_run=false;
        }
    }
    if(print_debug)
        std::cout<<this_pman->pmem_dir<< " GC thread finished."<< std::endl; 
}

// This is the main worker loop for write.
void job_threads::workerStart_write(u_short thread_id) {
  while (!finished) {
    {
      batch_job* jobs = getJob_w(thread_id);
      if (jobs != NULL) {
        // For manual insertion to the LSM tree.
        rocksdb::WriteBatch wb_in(jobs->wb_size);

        // Get the start_offset of this write job.
        u_long start_offset = jobs->jobs[0]->offset;
        // Memory buffer reduces the # of memcpy to the pmem to accelerate the
        // sequential write
        if (memory_buffer) {
          char* temporary_buffer = (char*)malloc(jobs->total_write_byte);
          long cur_pos = 0;

          // Iterate the jobs and insert to the buffer
          for (int i = 0; i < jobs->n_jobs; i++) {
            // u_long insert_offset=jobs->jobs[i]->offset;
            // u_short* dimm=(u_short*)&(insert_offset);
            // dimm[3]=jobs->jobs[i]->dimm;
            // wb_in.Put2(rocksdb::Slice(jobs->jobs[i]->key,jobs->jobs[i]->key_length),
            //   rocksdb::Slice((char*)(&(insert_offset)),8));
            // Write into the buffer
            mempcpy(temporary_buffer + cur_pos, jobs->jobs[i]->whole_data,
                    jobs->jobs[i]->total_length);
            // Update the cursor
            cur_pos += jobs->jobs[i]->total_length;
            // Free the memory
            delete (jobs->jobs[i]);
          }
          this_pman->insertManual(temporary_buffer, jobs->total_write_byte,
                                  start_offset);
          // this_pman->DBI->Write(wo,&wb_in);
          // Now clean the buffer
          free(temporary_buffer);

        } else {
          // Iterate the jobs and insert to the pmem, cheaper on memory
          for (int i = 0; i < jobs->n_jobs; i++) {
            // Write into PMem
            this_pman->insertJS(jobs->jobs[i]);
            // Free the memory added on 0111 because deleting jobs wont delete
            // the whole data
            delete (jobs->jobs[i]);

            // Insert into write batch for the LSM-tree
            // wb_in.Put2(jobs->jobs[i]->key,string((char*)(&(jobs->jobs[i]->offset)),8));
          }
        }
        // Write into the LSM-tree
        // wb_in.pmem_init=true;
        // DBI->Write(wo,&wb_in);

        // Persist the data
        this_pman->persist(start_offset, jobs->total_write_byte);

        // Manage the offset in PMem if not managed yet
        if (this_pman->offsets[2] < jobs->new_offset) {
          this_pman->offsets[2] = jobs->new_offset;
        }
        delete (jobs);
      }
    }
    {  // Insert last data
      if (finished && thread_id == 0 && w_count > 0) {
        if(print_debug)
            std::cout << "Thread " << thread_id << "last data" << endl;
        batch_job* jobs = getJob_w(thread_id);
        if (jobs != NULL) {
          long start_offset = jobs->jobs[0]->offset;
          // Memory buffer reduces the # of memcpy to the pmem to accelerate the
          // sequential write This reduce the IO on Pmem, preferred for low
          // value sizes
          if (memory_buffer) {
            char* temporary_buffer = (char*)malloc(jobs->total_write_byte);
            long cur_pos = 0;

            // Iterate the jobs and insert to the buffer
            for (int i = 0; i < jobs->n_jobs; i++) {
              // Write into the buffer
              mempcpy(temporary_buffer + cur_pos, jobs->jobs[i]->whole_data,
                      jobs->jobs[i]->total_length);
              // Update the cursor
              cur_pos += jobs->jobs[i]->total_length;
              // Free the memory 0111
              delete (jobs->jobs[i]);
            }
            // Finally insert it as a whole data
            this_pman->insertManual(temporary_buffer, jobs->total_write_byte,
                                    start_offset);
            // Now clean the buffer
            free(temporary_buffer);
          } else {
            // Iterate the jobs and insert to the pmem,
            // does not use as much memory, but incurs more IO on the Pmem
            for (int i = 0; i < jobs->n_jobs; i++) {
              // Write into PMem
              this_pman->insertJS(jobs->jobs[i]);
              // Free the memory added on 0111 because deleting jobs wont delete
              // the whole data
              delete (jobs->jobs[i]);

              // Insert into write batch for the LSM-tree
              // wb_in.Put2(jobs->jobs[i]->key,string((char*)(&(jobs->jobs[i]->offset)),8));
            }
          }
          this_pman->persist(start_offset, jobs->total_write_byte);

          // Manage the offset in PMem if not managed yet
          if (this_pman->offsets[2] < jobs->new_offset) {
            this_pman->offsets[2] = jobs->new_offset;
          }
          delete (jobs);
        }
      }
    }
  }
  if(print_debug)
    std::cout << this_pman->pmem_dir << " Thread " << thread_id << " done."
            << std::endl;
  wtd[thread_id].status = true;
}


// This is the main worker loop for read.
void job_threads::workerStart_read() {
  while (!finished) {
    job_pointer* job = getJob_r();
    if (job != NULL) {
      this_pman->readSTNC(job);
      job->status = true;
    }
  }
}

// Get a new job for the write thread
batch_job* job_threads::getJob_w(u_short thread_id) {
  // Temporary w_count, w_count is the # of insertions
  int t_w_count = 0;
  // Temporary current_buffer_size, which is the size of the current batch
  long t_current_buffer_size = 0;
  // long start_offset=0;
  // long new_offset=0;
  {  // Lock region, do as few as possible here
    MutexLock lock(mutex_w);
    wait_count++;
    b_cond_w = true;
    while ((!timer_lock) && (!finished)) {
      b_cond_w = false;
      pthread_cond_wait(&cond_w, &mutex_w);
      // The wait releases the mutex lock and suspends the thread (until a
      // signal). When a thread wakes up it is help until it can acquire the
      // mutex so when we get here the mutex is again locked.
      //
      // Note: You must use while() here. This is because of the situation.
      //   Two workers:  Worker A processing job A.
      //                 Worker B suspended on condition variable.
      //   Parent adds a new job and calls signal.
      //   This wakes up thread B. But it is possible for Worker A to finish its
      //   work and lock the mutex before the Worker B is released from the
      //   above call.
      //
      //   If that happens then Worker A will see that the queue is not empty
      //   and grab the work item in the queue and start processing. Worker B
      //   will then lock the mutext and proceed here. If the above is not a
      //   while then it would try and remove an item from an empty queue. With
      //   a while it sees that the queue is empty and re-suspends on the
      //   condition variable above.
    }
    // Relock the access to prevent other worker from accessing
    // Lock timer from waking up other threads
    timer_lock = false;
    b_cond_w = true;
    // Replace the main buffer with this thread buffer
    if (w_count > 0) {
      rocksdb::job_struct** temp = workQueue_w;
      workQueue_w = wtd[thread_id].buffer;
      wtd[thread_id].buffer = temp;
      t_w_count = w_count;
      t_current_buffer_size = current_buffer_size;
      temp = NULL;

      // Offset management
      /*
      start_offset=this_pman->current_offset.offset_current;
      this_pman->current_offset.offset_current+=current_buffer_size;
      new_offset=this_pman->current_offset.offset_current;*/

      // Reset the writes
      current_buffer_size = 0;
      w_count = 0;
    }
    // Release the write lock
    b_cond_w = false;
    wait_count--;
  }  // Lock region end

  batch_job* result = NULL;

  if (t_w_count > 0) {
    result = new batch_job(t_w_count);
    // Update the write offset after this batch job
    result->new_offset = wtd[thread_id].buffer[t_w_count - 1]->offset +
                         wtd[thread_id].buffer[t_w_count - 1]->total_length;
    // Fill in the batch job total size in bytes.
    result->total_write_byte = t_current_buffer_size;
    result->jobs = wtd[thread_id].buffer;
    // Old and inefficient because it needs to iterate everything. 0111
    // for(int i=0;i<t_w_count;i++){
    //     // Insert into the batch_job
    //     result->jobs[i]=wtd[thread_id].buffer[i];
    //     // Only for insertion to LSM tree in the writer thread
    //     //result->wb_size+=wtd[thread_id].buffer[i]->total_length_separated;
    // }
  }
  return result;
}

// Deprecated 0111
// Get a new job for the read thread
job_pointer* job_threads::getJob_r() {
  MutexLock lock(mutex_r);

  while ((workQueue_r.empty()) && (!finished)) {
    pthread_cond_wait(&cond_r, &mutex_r);
    // The wait releases the mutex lock and suspends the thread (until a
    // signal). When a thread wakes up it is help until it can acquire the mutex
    // so when we get here the mutex is again locked.
    //
    // Note: You must use while() here. This is because of the situation.
    //   Two workers:  Worker A processing job A.
    //                 Worker B suspended on condition variable.
    //   Parent adds a new job and calls signal.
    //   This wakes up thread B. But it is possible for Worker A to finish its
    //   work and lock the mutex before the Worker B is released from the above
    //   call.
    //
    //   If that happens then Worker A will see that the queue is not empty
    //   and grab the work item in the queue and start processing. Worker B will
    //   then lock the mutext and proceed here. If the above is not a while then
    //   it would try and remove an item from an empty queue. With a while it
    //   sees that the queue is empty and re-suspends on the condition variable
    //   above.
  }
  job_pointer* result = NULL;
  if (!finished) {
    result = (workQueue_r.front());
    workQueue_r.pop_front();
  }
  return result;
}

int job_threads::freespace(u_long cur, u_long gc, u_long max) {
  // Case when the log is rotated
  if (gc > cur) {
    return (gc - cur) * 100 / max;
  }
  // Case when write catch up to GC, rare
  if (gc == cur && gc != 0) {
    return 0;
  }
  // Other cases
  return (max - cur + gc) * 100 / max;
}

void* threadPoolThreadStart(void* data) {
  pointer_passer* ps = reinterpret_cast<pointer_passer*>(data);
  job_threads* jt = reinterpret_cast<job_threads*>(ps->job_threads_handler);

  try {
    if (ps->writer) {
      // cout<<"Write Thread started "<<threadID<<endl;
      jt->workerStart_write(ps->threadID);
    } else {
      if (ps->timer) {
        jt->timerStart_write();
      }
      if(ps->gc){
          jt->workerStart_gc();
      }
      // cout<<"Read Thread started "<<threadID<<endl;
      jt->workerStart_read();
    }
    delete ps;
  } catch (...) {
  }
  return NULL;
}