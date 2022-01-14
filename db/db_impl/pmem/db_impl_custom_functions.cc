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
    // Default values
    bool batchedBatch=true;
    int batchSize=100000000;
    int batchTimer=1000;
    bool memory_buffer=true;
    dimm_dir0="/dev/dax0.1";

    // GC

    // The default is 20%
    int gc_auto_trigger_percent=20;
    // Slow down write if free space is less than gc_throttle.
    // gc_throttle should be lower than gc_auto_trigger_percent
    // Default is 10%
    int gc_throttle=10;
    // Manual GC enable, 0 for off, 1 for always on, default is false
    bool manual_gc=false;
    // How much % should the GC do at a time
    // The default is 10% of the max
    int gc_how_much=10;
    // GC write batch size
    int gc_wb_size=200;
    bool print_debug=false;

    ifstream main_file("/home/ryan/RyanProject1/rdbshared/config");
    string conf_input;
    getline(main_file,conf_input); // Run PMEM or WiscKey
    getline(main_file,conf_input); // the nThreadWrite
    nThreadWrite = stoi(conf_input);
    getline(main_file,conf_input); // the nThreadRead
    nThreadRead = stoi(conf_input);
    // New config handler
    if(getline(main_file,conf_input)){ // the batchedBatch
        if(stoi(conf_input)==0){
            batchedBatch=false;
        }

        getline(main_file,conf_input); // the batchSize
        batchSize = stoi(conf_input);

        getline(main_file,conf_input); // the batchTimer
        batchTimer = stoi(conf_input);

        getline(main_file,conf_input); // the memory_buffer
        if(stoi(conf_input)==0){
            memory_buffer=false;
        }
    }
    // Dual writer new configs
    if(getline(main_file,conf_input)){ 
        if(stoi(conf_input)==1){
            dual_writer=true;
        }
        getline(main_file,dimm_dir0);
        getline(main_file,dimm_dir1);
    }else{
        getline(main_file,dimm_dir0);
    }

    // GC new configs
    if(getline(main_file,conf_input)){ 
        gc_auto_trigger_percent=stoi(conf_input);

        getline(main_file,conf_input); 
        gc_throttle = stoi(conf_input);

        getline(main_file,conf_input); 
        if(stoi(conf_input)==1){
            manual_gc=true;
        }

        getline(main_file,conf_input); 
        gc_how_much = stoi(conf_input);

        getline(main_file,conf_input); 
        gc_wb_size = stoi(conf_input);

        getline(main_file,conf_input); 
        if(stoi(conf_input)==1){
            print_debug=true;
        }
    }else{
        getline(main_file,dimm_dir0);
    }

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

    std::cout<<"batchedBatch "<<batchedBatch<<std::endl;
    std::cout<<"batchSize "<<batchSize<<std::endl;
    std::cout<<"batchTimer "<<batchTimer<<std::endl;
    std::cout<<"memory_buffer "<<memory_buffer<<std::endl;

    jt0=new job_threads();
    jt0->init(nThreadWrite, nThreadRead, pman0);
    jt0->batchedBatch=batchedBatch;
    jt0->buffer_high_threshold=batchSize;
    jt0->timerus=batchTimer;
    jt0->memory_buffer=memory_buffer;
    jt0->gc_auto_trigger_percent=gc_auto_trigger_percent;
    jt0->gc_throttle=gc_throttle;
    jt0->manual_gc=manual_gc;
    jt0->gc_how_much=gc_how_much;
    jt0->gc_wb_size=gc_wb_size;
    jt0->print_debug=print_debug;
    std::cout<<"Loaded PMEM0 at"<<dimm_dir0<<std::endl;

    if(dual_writer){
        // Init the other DIMM
        cout<<"PMEM1 is opened with ";
        cout<<nThreadWrite<<" write and "<<nThreadRead<<" read."<<endl;
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
        jt1->buffer_high_threshold=batchSize;
        jt1->timerus=batchTimer;
        jt1->memory_buffer=memory_buffer;
        jt1->gc_auto_trigger_percent=gc_auto_trigger_percent;
        jt1->gc_throttle=gc_throttle;
        jt1->manual_gc=manual_gc;
        jt1->gc_how_much=gc_how_much;
        jt1->gc_wb_size=gc_wb_size;
        jt1->print_debug=print_debug;
        std::cout<<"Loaded PMEM1 at"<<dimm_dir1<<std::endl;

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
        if(pmem_insertion0<pmem_insertion1){
            // Insert into PMem0
            pmem_insertion0++;
            jt0->addWork_write(js);
        }else{
            // Insert into PMem1
            pmem_insertion1++;
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
    if(dual_writer){
        if(pmem_insertion0<pmem_insertion1){
            // Insert into PMem0
            pmem_insertion0+=the_batch->writebatch_data.size();
            jt0->addWork_write_batch(the_batch,0);
        }else{
            // Insert into PMem1
            pmem_insertion1+=the_batch->writebatch_data.size();
            jt1->addWork_write_batch(the_batch,1);
        }
    }else{
            jt0->addWork_write_batch(the_batch,0);
    }
}

Slice DBImpl::get_custom(const char *string_offset){
    // Deprecated 0112
    // // Plasta get the data from pmem here
    // u_short dimm=(string_offset[5]>>5)&0x07;
    // u_long offset;
    // memcpy(&offset,string_offset,6);

    // string_offset is 6 byte, convert it to u_long 8 byte here
    u_long offset=((u_long*)string_offset)[0];
    u_short* mod=(u_short*)&offset;
    // Byte 6 and 7 is u_short for the dimm.
    u_short dimm=mod[3];
    // Remove the byte 6 and 7, u_short is 2 byte
    mod[3]=0;

    job_pointer jp;
    // Mask is deprecated 0112
    // jp.offset=offset&mask;
    jp.offset=offset;
    jp.status=false;
    std::cout<<"read dimm "<<dimm<<std::endl;
    std::cout<<"read offset "<<jp.offset<<std::endl;
    if(dimm==0){
        pman0->readSTNC(&jp);
    }
    else{
        pman1->readSTNC(&jp);
    }
    return Slice(jp.value_offset,jp.value_length);
}

}