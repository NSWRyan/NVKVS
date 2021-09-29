#include <iostream>
#include "db/db_impl/db_impl.h"
#include <time.h>

namespace ROCKSDB_NAMESPACE {

Iterator *DBImpl::get_iter(){
    return pman0->db->NewIterator(ReadOptions());
}

void DBImpl::load_pmem(bool new_old){
    // First do the config
    jt0=NULL;
    jt1=NULL;
    pman0=NULL;
    pman1=NULL;
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
    int temp;
    // Dual writer new configs
    if(getline(main_file,conf_input)){ 
        temp=stoi(conf_input); // Dual writer enable
        if(temp==1){
            dual_writer=true;
        }
        getline(main_file,dimm_dir0);
        getline(main_file,dimm_dir1);
    }

    getline(main_file,conf_input); // Pipelined write
    temp=stoi(conf_input);
    if(temp==1){
        pipelinedWrite=true;
    }
    // Pipelined write is a future feature.

    //Second load the PMem managers
    // Now load the first PMem managers
    cout<<"PMEM0 is opened with ";
    cout<<nThreadWrite<<" write and "<<nThreadRead<<" read."<<endl;
    main_file.close();
    pman0=new pmem_manager();
    if(new_old){
        std::cout<<"new DB"<<std::endl;
        pman0->open_pmem(nThreadWrite,true,dimm_dir0);
    }else{
        std::cout<<"old DB"<<std::endl;
        pman0->open_pmem(nThreadWrite,false,dimm_dir0);
    }
    pman0->DBI=this;
    jt0=new job_threads();
    jt0->init(nThreadWrite, nThreadRead, pman0);
    jt0->batchedBatch=batchedBatch;
    jt0->batchSize=batchSize;
    jt0->pipelinedWrite=pipelinedWrite;
    std::cout<<"batchedBatch "<<batchedBatch<<std::endl;
    std::cout<<"batchSize "<<batchSize<<std::endl;
    std::cout<<"pipelinedWrite "<<pipelinedWrite<<std::endl;

    if(dual_writer){
        // Init the other DIMM
        cout<<"PMEM1 is opened with ";
        cout<<nThreadWrite<<" write and "<<nThreadRead<<" read."<<endl;
        main_file.close();
        pman1=new pmem_manager();
        if(new_old){
            std::cout<<"new DB"<<std::endl;
            pman1->open_pmem(nThreadWrite,true,dimm_dir1);
        }else{
            std::cout<<"old DB"<<std::endl;
            pman1->open_pmem(nThreadWrite,false,dimm_dir1);
        }
        pman1->DBI=this;
        jt1=new job_threads();
        jt1->init(nThreadWrite, nThreadRead, pman1);
        jt1->batchedBatch=batchedBatch;
        jt1->batchSize=batchSize;
        jt1->pipelinedWrite=pipelinedWrite;
        std::cout<<"batchedBatch "<<batchedBatch<<std::endl;
        std::cout<<"batchSize "<<batchSize<<std::endl;
        std::cout<<"pipelinedWrite "<<pipelinedWrite<<std::endl;
    }

    std::cout<<"Init done "<<std::endl;
}

Slice DBImpl::put_custom(const char *key, u_short key_length, const char *value, u_short value_length){
    rocksdb::job_struct *js=new rocksdb::job_struct(key,key_length,value,value_length);
    jt0->addWork_write(js);
    while(!js->status){cout<<"";}; // Wait until the job is done
    u_long offset=js->offset;
    return rocksdb::Slice((char*)&offset,8); // The size of a long is 8 byte.
}

void DBImpl::put_custom(job_struct* js){
    if(dual_writer){
        if(pmem_insertion++%2==0){
            // Insert into PMem0
            jt0->addWork_write(js);
        }else{
            // Insert into PMem1
            jt1->addWork_write(js);
            js->dimm=1;
        }
    }else{
        jt0->addWork_write(js);
    }
    // Process the offset here
    
    if(js->throttle){
        usleep(10);
    }
    //while(!js->status){cout<<"";}
}
void DBImpl::put_custom_wb(WriteBatch* the_batch){
    jt0->addWork_write_batch(the_batch);
    while(!the_batch->wb_status){cout<<"";}; // Wait until the job is done
}

Slice DBImpl::get_custom(const char *string_offset){
    // Plasta get the data from pmem here
    u_short dimm=(string_offset[5]>>5)&0x07;
    u_long offset;
    memcpy(&offset,string_offset,6);

    job_pointer jp;
    jp.offset=offset&mask;
    jp.status=false;
    // std::cout<<"read dimm "<<dimm<<std::endl;
    // std::cout<<"read offset "<<jp.offset<<std::endl;
    if(dimm==0){
        pman0->readSTNC(&jp);
    }
    else{
        pman1->readSTNC(&jp);
    }
    return Slice(jp.value_offset,jp.value_length);
}

}