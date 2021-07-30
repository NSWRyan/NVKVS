#include <iostream>
#include "db/db_impl/db_impl.h"

namespace ROCKSDB_NAMESPACE {
void DBImpl::load_pmem_first(){
    ifstream main_file("/home/ryan/RyanProject1/rdbshared/config");
    string conf_input;
    getline(main_file,conf_input); // Run PMEM or WiscKey
    getline(main_file,conf_input); // the nThreadWrite
    nThreadWrite =stoi(conf_input);
    getline(main_file,conf_input); // the nThreadRead
    nThreadRead =stoi(conf_input);
    pmem=true; // Run pmem
    cout<<"PMEM is opened with ";
    cout<<nThreadWrite<<" write and "<<nThreadRead<<" read."<<endl;
    main_file.close();
    
}

Slice DBImpl::get_iter(const char *string_offset){
    return get_custom(string_offset);
}

void DBImpl::load_pmem(bool new_old){
    load_pmem_first();
    if(new_old){
        std::cout<<"new DB"<<std::endl;
        pman.open_pmem(nThreadWrite,true);
    }else{
        std::cout<<"old DB"<<std::endl;
        pman.open_pmem(nThreadWrite,false);
    }
    jt.init(nThreadWrite, nThreadRead, &pman);
}

string DBImpl::put_custom(const char *key, u_short key_length, const char *value, u_short value_length){
    job_struct *js=new job_struct();
    js->key=key;
    js->key_length=key_length;
    js->value=value;
    js->value_length=value_length;
    js->status=false;
    jt.addWork_write(js);
    while(!js->status){cout<<"";}; // Wait until the job is done
    return string((char*)&js->offset,8); // The size of a long is 8 byte.
}

string DBImpl::get_custom(const char *string_offset){
    // Plasta get the data from pmem here
    long offset=((long*)(string_offset))[0];
    job_pointer jp;
    jp.offset=offset;
    jp.status=false;

    pman.readSTNC(&jp);
    //jt.addWork_read(&jp);
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