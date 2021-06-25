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
#include <fstream>
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
#include "db/db_impl/pmem/job_threads.h"
#define DB_DIR "/home/ryan/RyanProject1/rdbshared/wisckey_db/"
#define wk_capacity 1024*1024*128
using namespace std;

struct wk_offset_helper{
    struct offset_helper o_h;
    fstream olog_file;
    ifstream ilog_file;
};

struct read_thread_helper{
    vector<ifstream> file_ifstreams;
};

class wk_manager{
    vector<wk_offset_helper> wkoffsets;
    vector<read_thread_helper> rh;
    u_short this_nThread;
    u_short this_nThreadR;
    bool initiated;
    public:
    wk_manager();
    ~wk_manager();
    int open(u_short nThreadRead, u_short nThread, bool new_old);
    int init(u_short nThread);
    int load(u_short nThread);
    int close();
    long insertNT(const char* key, u_short key_length, const char* value, u_short value_length, u_short thread_ID);
    // int readST(long offset, job_struct &the_job); Deprecated
    int readSTNC(job_struct_read *the_job, u_short threadID);
};
    
