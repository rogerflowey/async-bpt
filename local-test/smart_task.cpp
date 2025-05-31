#include "smart_task.h"



wutong::Task<void> wait() {

  co_return;
}

wutong::SmartTask<void> print_fun(int a) {
  co_await wait();
  std::cout<<a<<std::endl;
}


int main() {
  print_fun(1);
  print_fun(2);
  print_fun(3);
}
