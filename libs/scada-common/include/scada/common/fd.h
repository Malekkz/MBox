#ifndef SCADA_COMMON_FD_H
#define SCADA_COMMON_FD_H

namespace scada {

// Owns one file descriptor and closes it on destruction (coding standard,
// section 3). Move-only. An Fd that holds no descriptor is "invalid" and
// `get()` returns -1.
class Fd {
public:
    // Holds no descriptor.
    Fd() noexcept = default;

    // Takes ownership of `fd`. A negative value gives an invalid Fd, so the
    // return value of `open` or `socket` can be passed directly.
    explicit Fd(int fd) noexcept;

    ~Fd();

    Fd(Fd&& other) noexcept;
    Fd& operator=(Fd&& other) noexcept;
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;

    // True if a descriptor is held.
    [[nodiscard]] bool valid() const noexcept { return fd_ >= 0; }

    // Returns the descriptor without giving up ownership, or -1. The caller
    // must not close it and must not use it after this Fd is destroyed,
    // reset or moved from.
    [[nodiscard]] int get() const noexcept { return fd_; }

    // Closes the descriptor, if any. An error from `close` is ignored: the
    // descriptor is gone either way and there is nothing a caller could do.
    void reset() noexcept;

private:
    int fd_ = -1;
};

}  // namespace scada

#endif  // SCADA_COMMON_FD_H
