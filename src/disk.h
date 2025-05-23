#pragma once

#include "error.h"
#include "fd.h"
#include "freelist.h"
#include "mmap.h"
#include "os.h"
#include "page.h"
#include "shadow_page.h"
#include <expected>
#include <fstream>
#include <mutex>
#include <sys/fcntl.h>
namespace kv {

class DiskHandler final {
  static constexpr std::size_t INIT_MMAP_SIZE = 1 << 30;

public:
  DiskHandler() noexcept = default;
  [[nodiscard]] std::expected<std::size_t, Error>
  Open(std::filesystem::path path) noexcept {
    constexpr auto flags = (O_RDWR | O_CREAT);
    constexpr auto mode = 0666;
    LOG_TRACE("Opening db file: {}", path.string());

    // acquire a file descriptor
    auto fd = ::open(path.c_str(), flags, mode);
    if (fd == -1) {
      Close();
      LOG_ERROR("Failed to open db file");
      return std::unexpected{Error{"IO error"}};
    }
    fd_ = Fd{fd};
    path_ = path;
    page_size_ = OS::OSPageSize();

    // acquire file descriptor lock
    if (::flock(fd_.GetFd(), LOCK_EX) == -1) {
      LOG_ERROR("Failed to lock db file");
      Close();
      return std::unexpected{Error{"Failed to lock db file"}};
    }

    // open fstream for io
    fs_.open(path, std::ios::in | std::ios::out | std::ios::binary);
    if (!fs_.is_open()) {
      LOG_ERROR("Failed to open db file after creation: {}", path.string());
      Close();
      return std::unexpected{Error{"IO error"}};
    }
    fs_.exceptions(std::ios::goodbit);

    // set up mmap for io
    if (auto err_opt = mmap_handle_.Mmap(path_, fd_.GetFd(), INIT_MMAP_SIZE)) {
      return std::unexpected{*err_opt};
    }

    auto file_sz_or_err = OS::FileSize(path_);
    if (!file_sz_or_err) {
      Close();
      return std::unexpected{file_sz_or_err.error()};
    }

    auto file_sz = file_sz_or_err.value();
    opened_ = true;
    return file_sz;
  }

  // get page from mmap
  [[nodiscard]] Page &GetPageFromMmap(Pgid id) noexcept {
    assert(opened_);
    std::size_t pos = id * page_size_;

    assert(mmap_handle_.Valid() && pos + sizeof(Page) <= mmap_handle_.Size());

    // LOG_INFO("Accessing mmap memory address: {}, page id: {}",
    // GetAddress(pos), id);

    auto &p = *reinterpret_cast<Page *>(GetAddress(pos));
    assert(p.Id() == id);
    p.AssertMagic();
    return p;
  }

  [[nodiscard]] void *GetAddress(std::size_t pos) const noexcept {
    return static_cast<std::byte *>(mmap_handle_.MmapPtr()) + pos;
  }

  [[nodiscard]] std::expected<PageBuffer, Error>
  CreatePageBufferFromDisk(std::size_t offset, std::size_t size) noexcept {
    assert(opened_);
    if (!fs_.is_open()) {
      return std::unexpected{Error{"Fs is not open"}};
    }

    if (fs_.exceptions() != std::ios::goodbit) {
      return std::unexpected{Error{"Fs exceptions must be disabled"}};
    }

    fs_.seekg(offset, std::ios::beg);
    if (!fs_) {
      return std::unexpected{Error{"Failed to seek to the offset"}};
    }

    PageBuffer buffer(size, page_size_);
    fs_.read(reinterpret_cast<char *>(buffer.GetBuffer().data()),
             size * page_size_);
    if (!fs_) {
      return std::unexpected{Error{"Failed to read data from disk"}};
    }

    return buffer;
  }

  void Close() noexcept {
    // assert(opened_);
    // release the mmap region to trigger the deconstructor that will unmap the
    // region
    if (fs_.is_open()) {
      fs_.close();
    }
    mmap_handle_.Reset();
    auto e = fd_.Reset();
    assert(!e);
  }

  [[nodiscard]] std::size_t PageSize() const noexcept {
    assert(opened_);
    return page_size_;
  }

  [[nodiscard]] std::optional<Error> WritePageBuffer(PageBuffer &buf,
                                                     Pgid start_pgid) noexcept {
    return WriteRaw(reinterpret_cast<char *>(buf.GetBuffer().data()),
                    buf.GetBuffer().size(), start_pgid * page_size_);
  }

  [[nodiscard]] std::optional<Error> WritePage(const Page &p) noexcept {
    const auto size = (p.Overflow() + 1) * PageSize();
    return WriteRaw(reinterpret_cast<const char *>(&p), size,
                    p.Id() * PageSize());
  }

  [[nodiscard]] std::optional<Error> Sync() const noexcept {
    return fd_.Sync();
  }

  // Allocate a shadow page
  [[nodiscard]] std::expected<ShadowPage, Error>
  Allocate(Meta &rwtx_meta, std::size_t count) noexcept {
    auto shadow_page = ShadowPage{PageBuffer(count, page_size_)};
    auto &p = shadow_page.Get();
    p.SetOverflow(count - 1);

    // // don't use freelist for now
    // auto id_opt = freelist_.Allocate(count);
    // // valid allocation
    // if (id_opt.has_value()) {
    //   return p;
    // }

    auto cur_wm = rwtx_meta.GetWatermark();
    p.SetId(cur_wm);
    assert(p.Id() > 2);
    auto min_sz = (p.Id() + count) * page_size_;
    if (min_sz > mmap_handle_.Size()) {
      auto err = mmap_handle_.Mmap(path_, fd_.GetFd(), min_sz);
      if (err) {
        return std::unexpected{*err};
      }
    }

    rwtx_meta.SetWatermark(cur_wm + count);
    return shadow_page;
  }

private:
  [[nodiscard]] std::optional<Error> WriteRaw(const char *data, size_t size,
                                              std::size_t offset) noexcept {
    fs_.seekp(offset);
    fs_.write(data, size);
    if (fs_.fail()) {
      return Error{"IO Error"};
    }
    return fd_.Sync(); // flush to disk
  }

private:
  bool opened_{false};
  // path of the database file
  std::filesystem::path path_{""};
  // fstream of the database file
  std::fstream fs_;
  // file descriptor handle
  Fd fd_;
  // page size of the db
  std::size_t page_size_{OS::DEFAULT_PAGE_SIZE};
  // mutex to protect mmap access
  std::mutex mmaplock_;
  // mmap handle that will unmap when released
  MmapDataHandle mmap_handle_;
  // Freelist used to track reusable pages
  Freelist freelist_;
};

} // namespace kv
