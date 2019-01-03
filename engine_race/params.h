//
// Created by 杨煜溟 on 2018/11/29.
//

#ifndef ENGINE_PARAMS_H
#define ENGINE_PARAMS_H

#include <string>
#include <mutex>

using namespace std;

const int MAX_RANGE_COUNT = 2;

const int LOG_NUM = 4096;
const int NUM_PER_SLOT = 1024 * 16;
const size_t VALUE_LOG_SIZE = NUM_PER_SLOT << 12;
const size_t KEY_LOG_SIZE = NUM_PER_SLOT << 3;

const int FILE_NUM = 64;

const int SORT_LOG_SIZE = NUM_PER_SLOT;

const int KEY_ENLARGE_SIZE = 20010 * 8;
const int VALUE_ENLARGE_SIZE = 20010 * 4096;
const int SORT_ENLARGE_SIZE = 20010;

const size_t CACHE_SIZE = VALUE_LOG_SIZE;
const int CACHE_NUM = 16;
const int ACTIVE_CACHE_NUM = 8;
const int RESERVE_CACHE_NUM = CACHE_NUM - ACTIVE_CACHE_NUM;
const int PREPARE_CACHE_NUM = 4;


const int PAGE_PER_BLOCK = 4;
const size_t BLOCK_SIZE = PAGE_PER_BLOCK << 12;

const int RECOVER_THREAD = 64;
const int READDISK_THREAD = 2;

const int MAX_LENGTH_INSERT_SORT = 12;

const int RANGE_THRESHOLD = 10000000;


std::mutex sortLogEnlargeMtx;
std::mutex valueLogEnlargeMtx;

struct PMutex {
    pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;

    void lock() { pthread_mutex_lock(&m); }

    void unlock() { pthread_mutex_unlock(&m); }
};

struct PCond {
    PMutex pm;
    pthread_cond_t c = PTHREAD_COND_INITIALIZER;

    void lock() { pm.lock(); }

    void unlock() { pm.unlock(); }

    void wait() {
        pthread_cond_wait(&c, &pm.m);
    }

    void notify_all() {
        pthread_cond_broadcast(&c);
    }

    void notify_one() {
        pthread_cond_signal(&c);
    }
};

#endif //ENGINE_PARAMS_H
