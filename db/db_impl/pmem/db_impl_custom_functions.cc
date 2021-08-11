#include <iostream>
#include "db/db_impl/db_impl.h"
#include <time.h>

namespace ROCKSDB_NAMESPACE {

Iterator *DBImpl::get_iter(){
    return pman->db->NewIterator(ReadOptions());
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
    pman->DBI=this;
    jt=new job_threads();
    jt->init(nThreadWrite, nThreadRead, pman);
    jt->batchedBatch=batchedBatch;
    jt->batchSize=batchSize;
    jt->pipelinedWrite=pipelinedWrite;
    std::cout<<"batchedBatch "<<batchedBatch<<std::endl;
    std::cout<<"batchSize "<<batchSize<<std::endl;
    std::cout<<"pipelinedWrite "<<pipelinedWrite<<std::endl;
}

Slice DBImpl::put_custom(const char *key, u_short key_length, const char *value, u_short value_length){
    rocksdb::job_struct *js=new rocksdb::job_struct(key,key_length,value,value_length);
    jt->addWork_write(js);
    while(!js->status){cout<<"";}; // Wait until the job is done
    long offset=js->offset;
    return rocksdb::Slice((char*)&offset,8); // The size of a long is 8 byte.
}

void DBImpl::put_custom(job_struct* js){
    jt->addWork_write(js);
    if(js->throttle){
        usleep(100);
    }
    //while(!js->status){cout<<"";}
}
void DBImpl::put_custom_wb(WriteBatch* the_batch){
    jt->addWork_write_batch(the_batch);
    while(!the_batch->wb_status){cout<<"";}; // Wait until the job is done
}

Slice DBImpl::get_custom(const char *string_offset){
    // Plasta get the data from pmem here
    long offset=((long*)(string_offset))[0];
    job_pointer jp;
    jp.offset=offset;
    jp.status=false;

    pman->readSTNC(&jp);
    return Slice(jp.value_offset,jp.value_length);
}

}