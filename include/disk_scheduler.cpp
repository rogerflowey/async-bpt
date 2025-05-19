#include "disk_scheduler.h"

#include <cassert>
#include <iostream>
#include <shared.hpp>
//#define PMA_DEBUG;
DiskScheduler::DiskScheduler(const std::string& file_name, int queue_depth, unsigned int flag,unsigned int batch_threshold):fd_(-1),batch_submit_threshold_(batch_threshold == 0 ? 1 : batch_threshold) {
  int ret = io_uring_queue_init(queue_depth, &ring_, flag);
  if (ret < 0) {
    throw std::runtime_error("io_uring_queue_init failed: " + std::string(strerror(-ret)));
  }

  fd_ = open(file_name.c_str(), O_RDWR | O_CREAT | O_DIRECT, 0644);
  if (fd_ < 0) {
    io_uring_queue_exit(&ring_);
    throw std::runtime_error("file open failed: " + std::string(strerror(errno)));
  }

  files_to_register_[0] = fd_;
  ret = io_uring_register_files(&ring_, files_to_register_, 1);
  if (ret < 0) {
    close(fd_);
    io_uring_queue_exit(&ring_);
    throw std::runtime_error("io_uring_register_files failed: " + std::string(strerror(-ret)));
  }
}
DiskScheduler::~DiskScheduler() {
  try {
    flush_requests();
  } catch (const std::exception& e) {
    std::cerr << "DiskScheduler Destructor: Error flushing requests: " << e.what() << std::endl;
  }

  if (ring_.ring_fd != -1) {
    io_uring_unregister_files(&ring_);
    io_uring_queue_exit(&ring_);
  }
  if (fd_ >= 0) {
    close(fd_);
    fd_ = -1;
  }
}

//void DiskScheduler::register_buffers(const sjtu::vector<void *> &buffers) {
//  struct iovec *iovs = new iovec[buffers.size()];
//  registered_buffers_ = buffers;
//  for (size_t i = 0; i < buffers.size(); ++i) {
//    iovs[i].iov_base = buffers[i];
//    iovs[i].iov_len = BUFFER_SIZE;
//  }
//  int ret = io_uring_register_buffers(&ring_, iovs, buffers.size());
//  delete[] iovs;
//  if (ret < 0) {
//    io_uring_unregister_files(&ring_);
//    io_uring_queue_exit(&ring_);
//    close(fd_);
//    throw std::runtime_error("io_uring_register_buffers failed");
//  }
//}
void DiskScheduler::submit_request(IORequest *req) {

  struct io_uring_sqe *sqe = io_uring_get_sqe(&ring_);
  if (!sqe) {
    int submitted = io_uring_submit(&ring_);
    if (submitted < 0) {
      std::cerr<<"Submit fail:no enough space"<<std::endl;
      throw std::runtime_error("io_uring_submit failed during get_sqe retry: " + std::string(strerror(-submitted)));
    }
    sqe = io_uring_get_sqe(&ring_);
    if (!sqe) {
      std::cerr<<"Submit fail:io_uring_get_sqe corrupted"<<std::endl;
      throw std::runtime_error("io_uring submission queue full even after submit");
    }
  }

  const int file_index = 0;


  if (req->type_ == IORequest::Read) {
    io_uring_prep_read(sqe, file_index, req->buffer_addr_, req->count_, req->offset_);
  } else if (req->type_ == IORequest::Write) {
    io_uring_prep_write(sqe, file_index, req->buffer_addr_, req->count_, req->offset_);
  } else {
    throw std::runtime_error("Unknown IORequest type encountered in submit_request");
  }
  sqe->flags |= IOSQE_FIXED_FILE;

  io_uring_sqe_set_data(sqe, req);


  sqes_prepared_count_++;
  ++unfinished_requests_count_;
}

void DiskScheduler::flush_requests() {
  if (sqes_prepared_count_ == 0 && io_uring_sq_ready(&ring_) == 0) {
    return;
  }
  int ret = io_uring_submit(&ring_);
  if (ret < 0) {
    throw std::runtime_error("io_uring_submit failed in flush_requests: " + std::string(strerror(-ret)));
  }
  sqes_prepared_count_ = 0;
}

bool handle_completions_active = false;
void DiskScheduler::handle_completions() {
  if(handle_completions_active) {
    std::cerr<<"WARNING:Calling handle_completions inside handle_completions"<<std::endl;
    assert(false);
  }
  handle_completions_active=true;
  static unsigned long long time_cnt = 0;
  io_uring_cqe* cqe;
  unsigned head;
  unsigned count = 0;

#ifdef PMA_DEBUG
  std::cout<<"cycle start"<<std::endl;
#endif

  io_uring_for_each_cqe(&ring_, head, cqe) {
    auto* req = reinterpret_cast<IORequest*>(cqe->user_data);
    req->result_ = (cqe->res >= 0) &&
                 (static_cast<size_t>(cqe->res) == req->count_);

#ifdef PMA_DEBUG
    std::cout<<"finish IO on buffer:"<<req->buffer_addr_<<" type:"<<(req->type_==IORequest::Read?"read":"write")<<" page:"<<req->offset_/norb::PAGE_SIZE<<" req:"<<req<<std::endl;
#endif

    req->finished_ = true;
    if (req->coro_handle) {
      req->coro_handle.resume();  // 恢复协程
    }
    ++count;
    --unfinished_requests_count_;
  }
  io_uring_cq_advance(&ring_, count);
  time_cnt++;

#ifdef PMA_DEBUG
  std::cout<<"cycle end"<<std::endl;
#endif
  if(time_cnt%16==0||sqes_prepared_count_ >= batch_submit_threshold_) {
    //flush every 16 loop to ensure that there won't be someone waiting for remaining
    flush_requests();
  }
  handle_completions_active = false;
}

IOAwaitable DiskScheduler::async_read(void* buffer_addr, size_t count, off_t offset) {
  auto req = std::make_unique<IORequest>(IORequest::Read, buffer_addr, count, offset);
  //auto req = new IORequest(IORequest::Read, buffer_addr, count, offset);

  #ifdef PMA_DEBUG
  std::cout<<"read on:"<<buffer_addr<<" req addr:"<<req<<std::endl;
#endif
  submit_request(req.get());
  //submit_request(req);
  return {*this, std::move(req)};
}

IOAwaitable DiskScheduler::async_write(void* buffer_addr, size_t count, off_t offset) {

  auto req = std::make_unique<IORequest>(IORequest::Write, buffer_addr, count, offset);
  //auto* req = new IORequest{IORequest::Write, buffer_addr, count, offset};
#ifdef PMA_DEBUG
  std::cout<<"write on:"<<buffer_addr<<" req addr:"<<req<<std::endl;
#endif
  submit_request(req.get());
  return {*this, std::move(req)};
}

int DiskScheduler::get_unfinished_requests_count() {
  return unfinished_requests_count_;
}