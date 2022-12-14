#include <stdio.h>
#include <iostream>
#include <cstdint>
#include <bit>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string>
#include <vector>
#include <string.h>
#ifndef _WIN32
#include <unistd.h>
#else
#include <io.h>
#endif
#include <libpmem2.h>

#include <vector>
//MMAP and files
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <condition_variable>
#include <mutex>
#include "rocksdb/write_batch.h"
#include "rocksdb/db.h"
#include "rocksdb/slice.h"
#include "rocksdb/options.h"

#ifndef custom_pmem
#define custom_pmem
#include "db/db_impl/db_impl.h"
#endif

using namespace std;

// This is used for write

// Only for WiscKey read
struct job_struct_read{
    char *key;
    char *value;
    u_short key_length;
    u_short value_length;
    long offset;
    bool status;
    u_short threadID;
    job_struct_read():status(false){};
    ~job_struct_read();
};

// This is used for zero copy read
struct job_pointer{
    //Pointer in pmem gets deleted together with the pmem_unmap
    u_short key_length;
    u_short value_length;
    char *key_offset;
    char *value_offset;
    u_long offset;
    bool status;
    job_pointer():key_length(0),value_length(0),status(false){};
};

// START|start*****|gc******|current*****|STOP
struct offset_helper{
    u_long offset_start;
    u_long offset_gc;
    u_long offset_current;
    u_long offset_max;
};

class pmem_manager{
    bool initiated;
    int fd;
	struct pmem2_config *cfg;
	struct pmem2_map *map;
	struct pmem2_source *src;
    size_t pmem_size;
    u_short thread_number;
    int init_pmem(u_short nThread);
    int load_pmem(u_short nThread);
    int reset_pmem();
    int load_header(u_short nThread);
    int close_pmem();

    public:
    string pmem_dir;
    char *pmem_addr;
    pmem2_persist_fn persist_fn;
    // Currently unused
    rocksdb::DB* db;
    // Provide access to the LSM tree to the log workers.
    rocksdb::DBImpl* DBI;
    u_long* offsets; 
    pmem_manager();
    ~pmem_manager();
    int open_pmem(u_short nThread, bool start_new, string dir);
    char config_;
    bool print_debug=true;

    // Only for 1 thread
    u_long offset;

    // Max write for each partition
    u_long max_write;

    // The definition for the current files
    offset_helper current_offset;

 
    long insertST(string key, u_short key_length, string value, u_short value_length);
    void insertNT(const char* key, u_short key_length, const char* value, u_short value_length, long write_offset);
    void insertBatch(rocksdb::WriteBatch* wb);
    void insertManual(char* data, long length, long i_offset);
    void insertJS(rocksdb::job_struct* js);
    void persist(u_long insert_offset, size_t length);
    
    // int readST(long offset, job_struct &the_job); Deprecated
    int readSTNC(job_pointer *the_job);

    void load_llsm();
};
    
