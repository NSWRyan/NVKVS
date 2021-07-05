#include "wk_manager.h"
using namespace std;

wk_manager::wk_manager(){initiated=false;}

wk_manager::~wk_manager(){
    if(initiated){
        close();
    }
}
int wk_manager::open(u_short nThreadRead, u_short nThread, bool new_old){
    wkoffsets.resize(nThread);
    this_nThread=nThread;
    this_nThreadR=nThreadRead;
    if(new_old){
        init(nThread);
    }
    load(nThread);
    initiated=true;
    closed=false;
    return 0;
}

int wk_manager::init(u_short nThread){
    long max_write=wk_capacity/nThread;
    (void)(system("exec sudo rm -rf /home/ryan/RyanProject1/rdbshared/wisckey_db/*")+1);
    //Write the number of thread
    ofstream main_file("/home/ryan/RyanProject1/rdbshared/wisckey_db/head",ios::trunc);
    main_file.write((const char *)&nThread, sizeof(u_short));
    main_file.flush();
    main_file.close();
    wkoffsets.resize(nThread);
    for(int i=0;i<nThread;i++){
        // 32 because the first 32 bytes are the offset header
        wkoffsets[i].o_h.offset_current=32;
        wkoffsets[i].o_h.offset_gc=32;
        wkoffsets[i].o_h.offset_max=max_write;
        wkoffsets[i].o_h.offset_start=32;
        string file="/home/ryan/RyanProject1/rdbshared/wisckey_db/log";
        file+=to_string(i);
        wkoffsets[i].olog_file.open(file,ios::binary|ios::out);
        wkoffsets[i].olog_file.write((const char *)&wkoffsets[i].o_h.offset_current,sizeof(long));
        wkoffsets[i].olog_file.write((const char *)&wkoffsets[i].o_h.offset_gc,sizeof(long));
        wkoffsets[i].olog_file.write((const char *)&wkoffsets[i].o_h.offset_max,sizeof(long));
        wkoffsets[i].olog_file.write((const char *)&wkoffsets[i].o_h.offset_start,sizeof(long));
        wkoffsets[i].olog_file.flush();
        wkoffsets[i].olog_file.close();
    }
    return 0;
}

int wk_manager::load(u_short nThread){
    {
        string fmain="/home/ryan/RyanProject1/rdbshared/wisckey_db/head";
        ifstream main_file(fmain);
        if(main_file.is_open())
        {
            u_short read_nThread;
            main_file.read((char *)&read_nThread,sizeof(u_short));
            main_file.close();
            if(read_nThread!=nThread){
                // Mismatch nThread initialize everything.
                init(nThread);
            }
        }else{
            // File is not found so init everything.
            init(nThread);
        }
    }
    for(int i=0;i<nThread;i++){
        string file="/home/ryan/RyanProject1/rdbshared/wisckey_db/log";
        file+=to_string(i);
        wkoffsets[i].olog_file.open(file,ios::binary|ios::out|ios::in);
        wkoffsets[i].ilog_file.open(file);
        if(wkoffsets[i].ilog_file.is_open()){
            // load the offsets from the file
            // 0 = current; 8 = gc; 16 = max; 32 = start
            wkoffsets[i].ilog_file.seekg(0);
            wkoffsets[i].ilog_file.read((char *)&(wkoffsets[i].o_h.offset_current),sizeof(long));
            wkoffsets[i].ilog_file.read((char *)&wkoffsets[i].o_h.offset_gc,sizeof(long));
            wkoffsets[i].ilog_file.read((char *)&wkoffsets[i].o_h.offset_max,sizeof(long));
            wkoffsets[i].ilog_file.read((char *)&wkoffsets[i].o_h.offset_start,sizeof(long));
            wkoffsets[i].ilog_file.close();
        }
        wkoffsets[i].done=true;
    }

    rh.resize(this_nThreadR);
    for(int i=0;i<this_nThreadR;i++){
        rh[i].file_ifstreams.resize(nThread);
        for(int i2=0;i2<nThread;i2++){
            string file2="/home/ryan/RyanProject1/rdbshared/wisckey_db/log";
            file2+=to_string(i2);
            rh[i].file_ifstreams[i2].open(file2);
        }
    }
    return 0;
}

int wk_manager::close(){
    // Wait the worker threads to be closed first.
    while(!closed){
        cout<<"";
    }
    for(int i=0;i<this_nThread;i++){
        // Wait till the write is done
        while (!wkoffsets[i].done){
            cout<<"";
        }
        // Now close the files
        if(wkoffsets[i].olog_file.is_open()){
            wkoffsets[i].olog_file.flush();
            wkoffsets[i].olog_file.close();
        }
        
    }
    for(int i=0;i<this_nThreadR;i++){
        for(int i2=0;i2<this_nThread;i2++){
            rh[i].file_ifstreams[i2].close();
        }
    }
    return 0;
}

long wk_manager::insertNT(const char* key, u_short key_length, const char* value, u_short value_length, u_short thread_ID){
    long original_offset = wkoffsets[thread_ID].o_h.offset_current;
    wkoffsets[thread_ID].done=false;
    // Increment the thread offset
    // struture of each write key_length | value_length | key | value
    wkoffsets[thread_ID].o_h.offset_current+=4+key_length+value_length;
    // Update the offset and persist it.
    wkoffsets[thread_ID].olog_file.seekp(0);
    wkoffsets[thread_ID].olog_file.write((const char *)&wkoffsets[thread_ID].o_h.offset_current,sizeof(long));
    wkoffsets[thread_ID].olog_file.flush();
    
    // Write and persist key and value length
    wkoffsets[thread_ID].olog_file.seekp(original_offset);
    wkoffsets[thread_ID].olog_file.write((const char *)&key_length,sizeof(u_short));
    wkoffsets[thread_ID].olog_file.write((const char *)&value_length,sizeof(u_short));

    // Write and persist key and value
    wkoffsets[thread_ID].olog_file.write(key,key_length);
    wkoffsets[thread_ID].olog_file.write(value,value_length);
    wkoffsets[thread_ID].olog_file.flush();
    wkoffsets[thread_ID].done=true;
    return original_offset;
}

// Not job_pointer because we cant read from a pointer...
/*
int wk_manager::readSTNC(job_struct_read *the_job){
    wkoffsets[the_job->threadID].ilog_file.seekg(the_job->offset);
    wkoffsets[the_job->threadID].ilog_file.read((char *)&the_job->key_length,sizeof(u_short));
    wkoffsets[the_job->threadID].ilog_file.read((char *)&the_job->value_length,sizeof(u_short));
    the_job->key=(char*)malloc(the_job->key_length);
    the_job->value=(char*)malloc(the_job->value_length);
    wkoffsets[the_job->threadID].ilog_file.read(the_job->key,the_job->key_length);
    wkoffsets[the_job->threadID].ilog_file.read(the_job->value,the_job->value_length);
    return 0;
}*/
int wk_manager::readSTNC(job_struct_read *the_job,u_short threadID){
    rh[threadID].file_ifstreams[the_job->threadID].seekg(the_job->offset);
    rh[threadID].file_ifstreams[the_job->threadID].read((char *)&the_job->key_length,sizeof(u_short));
    rh[threadID].file_ifstreams[the_job->threadID].read((char *)&the_job->value_length,sizeof(u_short));
    the_job->key=(char*)malloc(the_job->key_length);
    the_job->value=(char*)malloc(the_job->value_length);
    rh[threadID].file_ifstreams[the_job->threadID].read(the_job->key,the_job->key_length);
    rh[threadID].file_ifstreams[the_job->threadID].read(the_job->value,the_job->value_length);
    return 0;
}

job_struct_read::~job_struct_read(){
    delete key;
    delete value;
}