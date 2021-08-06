#include "db/db_impl/pmem/pmem_manager.h"
using namespace std;


// Destructor
pmem_manager::~pmem_manager(){
    if(initiated){
        close_pmem();
    }
}

// Initialize from 0
pmem_manager::pmem_manager(){
    initiated=false;
}

// Initialize the pmem address
int pmem_manager::open_pmem(u_short nThread, bool start_new){
    if ((fd = open("/dev/dax0.1", O_RDWR)) < 0) {
        printf("Open failed;");
        perror("open");
        exit(1);
    }

    if (pmem2_config_new(&cfg)) {
        pmem2_perror("pmem2_config_new");
        exit(1);
    }

    if (pmem2_source_from_fd(&src, fd)) {
        pmem2_perror("pmem2_source_from_fd");
        exit(1);
    }

    if (pmem2_config_set_required_store_granularity(cfg,
            PMEM2_GRANULARITY_PAGE)) {
        pmem2_perror("pmem2_config_set_required_store_granularity");
        exit(1);
    }

    if (pmem2_map_new(&map, cfg, src)) {
        pmem2_perror("pmem2_map");
        exit(1);
    }
    pmem_addr = (char*)pmem2_map_get_address(map);
    pmem_size = pmem2_map_get_size(map);
    persist_fn = pmem2_get_persist_fn(map);

    if(start_new){
        init_pmem(nThread);
    }

    if(load_header(nThread)!=0){
        cout<<"Load header fail"<<endl;
        init_pmem(nThread);
        if(load_header(nThread)!=0){
            cout<<"Load header fail again"<<endl;
        }
    }
    initiated=true;
    return 0;
}

// Clear all of the mappings
int pmem_manager::close_pmem(){
    initiated=false;
    // Update the offset and persist it.
    offsets[2]=current_offset.offset_current;
    persist_fn(offsets+3,8*4);
    pmem2_map_delete(&map);
    pmem2_source_delete(&src);
    pmem2_config_delete(&cfg);
    close(fd);
    return 0;
}

// Initialize the PMEM and partition the PMEM.
// offsets[0+n*2] = last GC
// offsets[1+n*2] = write continue offset, offset_current
int pmem_manager::init_pmem(u_short nThread){
    // total_write is the size of the header
    long total_write=1;
    pmem_addr[0]='1';
    // Total write + 1 byte
    memcpy(pmem_addr+1,&nThread,2);
    total_write+=2;
    // Total write + 2 bytes;
    offsets=(long*)(pmem_addr+3);
    // Offset sizes (3 longs)
    total_write+=nThread*8*4;
    // Size for each partition, -1 is to ensure that no overcapacity writes
    max_write=(pmem_size-total_write-1);
    // The start position
    offsets[0]=total_write;
    // The last GC pos
    offsets[1]=total_write;
    // The last write pos
    offsets[2]=total_write;
    // The max write
    offsets[3]=total_write+max_write;
    // Now persist the header
    persist_fn(pmem_addr,total_write);
    offset=0;
    return 0;
}

int pmem_manager::load_header(u_short nThread){
    /*
    if(pmem_addr[0]!=1){
        // Not initialized so lets initialize it.
        init_pmem(nThread);
    }
    */
    int return_val=0;
    u_short *temp_nThread=(u_short*)(pmem_addr+1);
    if(temp_nThread[0]!=nThread){
        // Error the number of thread is not matching
        return_val=1;
        init_pmem(3);
    };
    
    // Load the offsets
    offsets=(long*)(pmem_addr+3);

    // Now fill in the offset_helper for each threads.
    // The start position
    current_offset.offset_start=offsets[0];
    // The last GC pos
    current_offset.offset_gc=offsets[1];
    // The last write pos
    current_offset.offset_current=offsets[2];
    // The max write
    current_offset.offset_max=current_offset.offset_start+max_write-1;
    //cout<<"T"<<i<<" offset start "<<current_offset[i].offset_current<<" stop "<< current_offset[i].offset_max<<endl;

    return return_val;
}

int pmem_manager::reset_pmem(){
    pmem_addr[0]=0;
    persist_fn(pmem_addr,1);
    return 0;
}

// Write using persistency from libpmem2
long pmem_manager::insertST(string key, u_short key_length, string value, u_short value_length){
    long original_offset = offset;
    //struture of each write key_length | value_length | key | value
    //Write and persist key
    offset+=4+key_length+value_length;
    memcpy(pmem_addr+original_offset,&key_length,2);
    memcpy(pmem_addr+original_offset+2,&value_length,2);
    //Write and persist key value
    memcpy(pmem_addr+original_offset+4,key.c_str(),  key_length);
    memcpy(pmem_addr+original_offset+4+key_length,value.c_str(), value_length);
    persist_fn(pmem_addr+original_offset,4+key_length+value_length);
    return original_offset;
}

void pmem_manager::insertNT(const char* key, u_short key_length, const char* value, u_short value_length, long write_offset){
    //Write and persist key and value length
    memcpy(pmem_addr+write_offset,&key_length,2);
    memcpy(pmem_addr+write_offset+2,&value_length,2);

    // Write and persist key and value
    memcpy(pmem_addr+write_offset+4,key,key_length);
    memcpy(pmem_addr+write_offset+4+key_length,value, value_length);
    persist_fn(pmem_addr+write_offset,4+key_length+value_length);
}

void pmem_manager::insertBatch(rocksdb::WriteBatch* wb){
    size_t v_size=wb->writebatch_data->size();
    for(size_t i=0;i<v_size;i++){
        rocksdb::job_struct* js=wb->writebatch_data->at(i);
        memcpy(pmem_addr+js->offset,&(js->key_length),2);
        memcpy(pmem_addr+js->offset+2,&(js->value_length),2);
        memcpy(pmem_addr+js->offset+4,js->key,js->key_length);
        memcpy(pmem_addr+js->offset+4+js->key_length,js->value, js->value_length);
    }
}

void pmem_manager::insertJS(rocksdb::job_struct* js){
    memcpy(pmem_addr+js->offset,&(js->key_length),2);
    memcpy(pmem_addr+js->offset+2,&(js->value_length),2);
    memcpy(pmem_addr+js->offset+4,js->key,js->key_length);
    memcpy(pmem_addr+js->offset+4+js->key_length,js->value, js->value_length);
}



void pmem_manager::persist(long insert_offset, long length){
    persist_fn(pmem_addr+insert_offset,length);
}

/* Deprecated
// Read with copy to DRAM first
int pmem_manager::readST(long offset, job_struct &the_job){
    memcpy(&the_job.key_length,pmem_addr+offset,2);
    memcpy(&the_job.value_length,pmem_addr+offset+2,2);
    the_job.key=(char*)malloc(the_job.key_length);
    the_job.value=(char*)malloc(the_job.value_length);
    memcpy(the_job.key,pmem_addr+offset+4,the_job.key_length);
    memcpy(the_job.value,pmem_addr+offset+4+the_job.key_length,the_job.value_length);
    return 0;
}
*/

// This one is just passing the pointer toward the requester. Zero copy.
int pmem_manager::readSTNC(job_pointer *the_job){
    //Get the address of the data
    u_short *header=(u_short*)(pmem_addr+the_job->offset);

    //Get the length
    the_job->key_length=header[0];
    the_job->value_length=header[1];

    //Assign the address based on the length
    //4 is the length of the header
    the_job->key_offset=(char*)pmem_addr+the_job->offset+4;
    //Value start after the key.
    the_job->value_offset=the_job->key_offset+the_job->key_length;
    return 0;
}
