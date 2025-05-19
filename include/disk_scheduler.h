#pragma once

#include "stlite/vector.hpp"
#include "tasks.h"
#include <fcntl.h>
#include <future>
#include <liburing.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <coroutine>

class DiskScheduler;
constexpr int BUFFER_SIZE = 4096;


struct IORequest {
  enum IOType { Read, Write };

  IOType type_;
  void* buffer_addr_;
  size_t count_;
  off_t offset_;
  std::coroutine_handle<> coro_handle = nullptr;
  bool finished_ = false;
  bool result_ = false;

  IORequest(IOType type, void* buffer_addr, size_t c, off_t o)
    : type_(type), buffer_addr_(buffer_addr), count_(c), offset_(o) {}
};
struct IOAwaitable;
class DiskScheduler {
  struct io_uring ring_;
  int fd_;
  int files_to_register_[1];

  const unsigned int batch_submit_threshold_;
  unsigned int sqes_prepared_count_ = 0;
  unsigned int unfinished_requests_count_ = 0;
public:
  DiskScheduler(const std::string& file_name,int queue_depth,unsigned int flag=0,unsigned int batch_threshold = 16);
  ~DiskScheduler();
  //void register_buffers(const sjtu::vector<void*>& buffers);
  void submit_request(IORequest* req);
  void flush_requests();
  void handle_completions();
  IOAwaitable async_read(void* buffer_addr, size_t count, off_t offset);
  IOAwaitable async_write(void* buffer_addr, size_t count, off_t offset);
  int get_unfinished_requests_count();
};



struct IOAwaitable {
  DiskScheduler& scheduler;
  std::unique_ptr<IORequest> req;
  //IORequest* req;

  IOAwaitable(DiskScheduler& s, std::unique_ptr<IORequest> req) : scheduler(s), req(std::move(req)) {}
  //IOAwaitable(DiskScheduler& s, IORequest* req) : scheduler(s), req(std::move(req)) {}
  IOAwaitable(const IOAwaitable&) = delete;
  IOAwaitable& operator=(const IOAwaitable&) = delete;
  IOAwaitable(IOAwaitable&& a) noexcept:scheduler(a.scheduler),req(std::move(a.req)){
    //a.req.reset();
    a.req.reset();
  };

  bool await_ready() const noexcept {
    return req->finished_;
  }

  void await_suspend(std::coroutine_handle<> h) {
    req->coro_handle = h;
  }

  bool await_resume() noexcept {
    return req->result_;
  }
};

