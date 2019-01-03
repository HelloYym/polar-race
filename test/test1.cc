#include <assert.h>
#include <stdio.h>
#include <string>
#include "../include/engine.h"

static const char kEnginePath[] = "/tmp/test_engine";
static const char kDumpPath[] = "/tmp/test_dump";

using namespace polar_race;

class DumpVisitor : public Visitor {
public:
  DumpVisitor(int* kcnt)
    : key_cnt_(kcnt) {}

  ~DumpVisitor() {}

  void Visit(const PolarString& key, const PolarString& value) {
    printf("Visit %s --> %s\n", key.data(), value.data());
    (*key_cnt_)++;
  }
  
private:
  int* key_cnt_;
};

int main() {
  Engine *engine1 = NULL;

  RetCode ret = Engine::Open(kEnginePath, &engine1);
  assert (ret == kSucc);


  ret = engine1->Write("aaaaaaaa", "aaaaaaaaaaa");
  assert (ret == kSucc);
  ret = engine1->Write("aaaaaaaa", "111111111111111111111111111111111111111111");
  ret = engine1->Write("aaaaaaa9", "9");
  ret = engine1->Write("aaaaaaaa", "4");
  ret = engine1->Write("aaaaaaa9", "a");
  ret = engine1->Write("aaaaaaa9", "s");
  ret = engine1->Write("aaaaaaaa", "kkk");


    ret = engine1->Write("bbbbbbbb", "bbbbbbbbbbbb");
  assert (ret == kSucc);

  ret = engine1->Write("ccdddddd", "cbbbbbbbbbbbb");
  std::string value;
  delete engine1;

  Engine *engine2 = NULL;

  ret = Engine::Open(kEnginePath, &engine2);
  ret = engine2->Read("aaaaaaaa", &value);
  printf("Read aaa value: %s\n", value.c_str());

    ret = engine2->Read("aaaaaaa9", &value);
    printf("Read aaa value: %s\n", value.c_str());
  
  ret = engine2->Read("bbbbbbbb", &value);
  assert (ret == kSucc);
  printf("Read bbb value: %s\n", value.c_str());

  ret = engine2->Read("ccdddddd", &value);
  printf("Read ccc value: %s\n", value.c_str());

  ret = engine2->Read("ddd", &value);
//  printf("Read ddd value: %s\n", value.c_str());


  assert (ret == kNotFound);

  int key_cnt = 0;
  DumpVisitor vistor(&key_cnt);
  ret = engine2->Range("b", "", vistor);
  assert (ret == kSucc);
  printf("Range key cnt: %d\n", key_cnt);
  

  delete engine2;

  return 0;
}
