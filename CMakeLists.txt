cmake_minimum_required(VERSION 3.10)
project(engine)

set(CMAKE_CXX_STANDARD 14)

include_directories(engine_race)
include_directories(include)

add_executable(
        engine_race
        engine_race/engine_race.cc
        engine_race/engine_race.h
        engine_race/SortLog.h
        engine_race/CacheQueue.h
        include/engine.h
        include/polar_string.h
#        test/benchmark.cc
        test/histogram.cc
        test/histogram.h
        test/likely.h
        test/random.cc
        test/random.h
        test/testutil.cc
        test/testutil.h
        test/test.cc
        engine_race/PEngine.h engine_race/KVFiles.h engine_race/params.h engine_race/KeyValueLog.h)

find_package(Threads REQUIRED)
target_link_libraries(engine_race Threads::Threads)