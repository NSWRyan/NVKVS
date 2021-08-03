#include <iostream>
#include "db/db_impl/db_impl.h"

namespace ROCKSDB_NAMESPACE {

Slice DBImpl::get_iter(const char *string_offset){
    return get_custom(string_offset);
}

void DBImpl::load_pmem(bool new_old){
    bool batchedBatch=true;
    int batchSize=10000000;
    bool pipelinedWrite=false;
    ifstream main_file("/home/ryan/RyanProject1/rdbshared/config");
    string conf_input;
    getline(main_file,conf_input); // Run PMEM or WiscKey
    getline(main_file,conf_input); // the nThreadWrite
    nThreadWrite = stoi(conf_input);
    getline(main_file,conf_input); // the nThreadRead
    nThreadRead = stoi(conf_input);
    // New config handler
    if(getline(main_file,conf_input)){ // the batchedBatch
        batchSize = stoi(conf_input);
        if(batchSize==0){
            batchedBatch=false;
        }
        
        getline(main_file,conf_input); // the batchSize
        batchSize = stoi(conf_input);

        getline(main_file,conf_input); // the pipelined write
        int i_pw=stoi(conf_input);
        if(i_pw==1){
            pipelinedWrite=true;
        }
    }
    
    cout<<"PMEM is opened with ";
    cout<<nThreadWrite<<" write and "<<nThreadRead<<" read."<<endl;
    main_file.close();

    pman=new pmem_manager();
    if(new_old){
        std::cout<<"new DB"<<std::endl;
        pman->open_pmem(nThreadWrite,true);
    }else{
        std::cout<<"old DB"<<std::endl;
        pman->open_pmem(nThreadWrite,false);
    }
    jt=new job_threads();
    jt->init(nThreadWrite, nThreadRead, pman);
    jt->DBI=this;
    jt->batchedBatch=batchedBatch;
    jt->batchSize=batchSize;
    jt->pipelinedWrite=pipelinedWrite;
    std::cout<<"batchedBatch "<<batchedBatch<<std::endl;
    std::cout<<"batchSize "<<batchSize<<std::endl;
    std::cout<<"pipelinedWrite "<<pipelinedWrite<<std::endl;
}

string DBImpl::put_custom(const char *key, u_short key_length, const char *value, u_short value_length){
    rocksdb::job_struct *js=new rocksdb::job_struct();
    js->key=key;
    js->key_length=key_length;
    js->value=value;
    js->value_length=value_length;
    js->total_length=4+key_length+value_length;
    js->status=false;
    jt->addWork_write(js);
    while(!js->status){cout<<"";}; // Wait until the job is done
    long offset=js->offset;
    return string((char*)&offset,8); // The size of a long is 8 byte.
}

void DBImpl::put_custom_wb(WriteBatch* the_batch){
    jt->addWork_write_batch(the_batch);
    while(!the_batch->wb_status){cout<<"";}; // Wait until the job is done
}

string DBImpl::get_custom(const char *string_offset){
    // Plasta get the data from pmem here
    long offset=((long*)(string_offset))[0];
    job_pointer jp;
    jp.offset=offset;
    jp.status=false;

    pman->readSTNC(&jp);
    //jt->addWork_read(&jp);
    //while(!jp.status){cout<<"";}; // Wait until read is done
    /*
    {
        unique_lock<mutex> lk(jp.m);
        jobs_w[i].cv.wait(lk,[&]{return jobs_w[i].status;});
    }
    */
    return string(jp.value_offset,jp.value_length);
}

}