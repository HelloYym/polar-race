rm -rf /tmp/test_engine_k
make clean
make
g++ -std=c++11 -o test/test -g -I. test/test.cc -L./lib -lengine -lpthread -lrt
test/test
