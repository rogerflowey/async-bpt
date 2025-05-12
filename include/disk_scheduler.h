#include "stlite/vector.hpp"
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
  int buffer_id_;
  size_t count_;
  off_t offset_;
  std::coroutine_handle<> coro_handle;
  bool result_ = false;

  IORequest(IOType type, int bid, size_t c, off_t o)
    : type_(type), buffer_id_(bid), count_(c), offset_(o) {}
};
struct IOAwaitable;
class DiskScheduler {
  struct io_uring ring_;
  int fd_;
  int files_to_register_[1];
  norb::vector<void*> registered_buffers_;

  const unsigned int batch_submit_threshold_;
  unsigned int sqes_prepared_count_ = 0;
public:
  DiskScheduler(const std::string& file_name,int queue_depth,unsigned int flag=0,unsigned int batch_threshold = 16);
  ~DiskScheduler();
  void register_buffers(const norb::vector<void*>& buffers);
  void submit_request(IORequest* req);
  void flush_requests();
  void handle_completions();
  IOAwaitable async_read(int buffer_id, size_t count, off_t offset);
  IOAwaitable async_write(int buffer_id, size_t count, off_t offset);
};



struct IOAwaitable {
  DiskScheduler& scheduler;
  IORequest req;

  IOAwaitable(DiskScheduler& s, IORequest::IOType t, int bid, size_t c, off_t o)
    : scheduler(s), req{t,bid,c,o} {}

  bool await_ready() const noexcept { return false; }

  void await_suspend(std::coroutine_handle<> h) {
    req.coro_handle = h;
    scheduler.submit_request(&req);
  }

  bool await_resume() noexcept {
    return req.result_;
  }
};




