#include <iostream>
#include <glog/logging.h>

using namespace std;

int main(int argc, char *argv[])
{
  LOG(INFO) << "hello glog";
  cout << "here\n";
  int a = 4;
  CHECK(a == 4) << "variable not match";
  return 0;
}
