#pragma once

#include <cstddef>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace ohlcv::replay {

// RAII read-only mmap of a whole file. base() is nullptr if the file couldn't be
// opened or mapped — callers check that and bail. The mapping and fd are
// released in the destructor, so there's no manual munmap/close to forget.
class MappedFile {
public:
    explicit MappedFile(const char* path) {
        fd_ = ::open(path, O_RDONLY);
        if (fd_ < 0) return;
        struct stat st {};
        if (::fstat(fd_, &st) != 0) {
            ::close(fd_);
            fd_ = -1;
            return;
        }
        size_ = static_cast<std::size_t>(st.st_size);
        void* p = ::mmap(nullptr, size_, PROT_READ, MAP_PRIVATE, fd_, 0);
        if (p == MAP_FAILED) {
            ::close(fd_);
            fd_ = -1;
            return;
        }
        base_ = static_cast<const std::byte*>(p);
    }

    ~MappedFile() {
        if (base_ != nullptr) ::munmap(const_cast<std::byte*>(base_), size_);
        if (fd_ >= 0) ::close(fd_);
    }

    MappedFile(const MappedFile&)            = delete;
    MappedFile& operator=(const MappedFile&) = delete;

    [[nodiscard]] const std::byte* base() const noexcept { return base_; }
    [[nodiscard]] std::size_t      size() const noexcept { return size_; }

private:
    const std::byte* base_ = nullptr;
    std::size_t      size_ = 0;
    int              fd_   = -1;
};

}  // namespace ohlcv::replay
