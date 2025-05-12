#include "disk_scheduler.h"

#include <iostream>

DiskScheduler::DiskScheduler(const std::string& file_name, int queue_depth, unsigned int flag,unsigned int batch_threshold):registered_buffers_{},fd_(-1),batch_submit_threshold_(batch_threshold == 0 ? 1 : batch_threshold) {
  int ret = io_uring_queue_init(queue_depth, &ring_, flag);
  if (ret < 0) {
    throw std::runtime_error("io_uring_queue_init failed: " + std::string(strerror(-ret)));
  }

  fd_ = open(file_name.c_str(), O_RDWR | O_CREAT | O_DIRECT, 0644); // Added O_DIRECT
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

  if (!registered_buffers_.empty()) {
    if (ring_.ring_fd != -1) {
      io_uring_unregister_buffers(&ring_);
    }
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

void DiskScheduler::register_buffers(const norb::vector<void *> &buffers) {
  struct iovec *iovs = new iovec[buffers.size()];
  registered_buffers_ = buffers;
  for (size_t i = 0; i < buffers.size(); ++i) {
    iovs[i].iov_base = buffers[i];
    iovs[i].iov_len = BUFFER_SIZE;
  }
  int ret = io_uring_register_buffers(&ring_, iovs, buffers.size());
  delete[] iovs;
  if (ret < 0) {
    io_uring_unregister_files(&ring_);
    io_uring_queue_exit(&ring_);
    close(fd_);
    throw std::runtime_error("io_uring_register_buffers failed");
  }
}
void DiskScheduler::submit_request(IORequest *req) {
  struct io_uring_sqe *sqe = io_uring_get_sqe(&ring_);
  if (!sqe) {
    int submitted = io_uring_submit(&ring_);
    if (submitted < 0) {
      throw std::runtime_error("io_uring_submit failed during get_sqe retry: " + std::string(strerror(-submitted)));
    }
    sqe = io_uring_get_sqe(&ring_);
    if (!sqe) {
      throw std::runtime_error("io_uring submission queue full even after submit");
    }
  }


  if (req->buffer_id_ < 0 || static_cast<size_t>(req->buffer_id_) >= registered_buffers_.size()) {
      io_uring_sqe_set_data(sqe, nullptr);
      throw std::out_of_range("Invalid buffer_id provided to submit_request: " + std::to_string(req->buffer_id_));
  }
  void* buffer_addr = registered_buffers_[req->buffer_id_];

  const int file_index = 0;

  if (req->type_ == IORequest::Read) {
    io_uring_prep_read_fixed(sqe, file_index, buffer_addr, req->count_, req->offset_, req->buffer_id_);
  } else if (req->type_ == IORequest::Write) {
    io_uring_prep_write_fixed(sqe, file_index, buffer_addr, req->count_, req->offset_, req->buffer_id_);
  } else {
    throw std::runtime_error("Unknown IORequest type encountered in submit_request");
  }

  io_uring_sqe_set_data(sqe, req);

  sqe->flags |= IOSQE_FIXED_FILE;
  sqes_prepared_count_++;

  if (sqes_prepared_count_ >= batch_submit_threshold_) {
    int ret = io_uring_submit(&ring_);
    if (ret < 0) {
      throw std::runtime_error("io_uring_submit failed on batch threshold: " + std::string(strerror(-ret)));
    }
    sqes_prepared_count_ = 0;
  }
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

void DiskScheduler::handle_completions() {
  io_uring_cqe* cqe;
  unsigned head;
  unsigned count = 0;
  io_uring_for_each_cqe(&ring_, head, cqe) {
    IORequest* req = reinterpret_cast<IORequest*>(cqe->user_data);
    req->result_ = (cqe->res >= 0) &&
                 (static_cast<size_t>(cqe->res) == req->count_);

    if (req->coro_handle) {
      req->coro_handle.resume();  // 恢复协程
    }
    ++count;
  }
  io_uring_cq_advance(&ring_, count);
}

IOAwaitable DiskScheduler::async_read(int buffer_id, size_t count, off_t offset) {
  return {*this, IORequest::Read, buffer_id, count, offset};
}

IOAwaitable DiskScheduler::async_write(int buffer_id, size_t count, off_t offset) {
  return {*this, IORequest::Write, buffer_id, count, offset};
}