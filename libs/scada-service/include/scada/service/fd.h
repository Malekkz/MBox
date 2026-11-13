#ifndef SCADA_SERVICE_FD_H
#define SCADA_SERVICE_FD_H

namespace scada::service {

// Owns one file descriptor and closes it on destruction. Move-only.
//
// libscada-common will provide the project-wide `Fd` (coding standard,
// section 3). Until issue P0-06 adds it, this is the only owner type; it moves
// there then.
class Fd {
public:
    // An empty owner; valid() is false.
    Fd() noexcept = default;

    // Takes ownership of `descriptor`. A negative value gives an empty owner.
    explicit Fd(int descriptor) noexcept : fd_(descriptor) {}

    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    Fd(Fd&& other) noexcept;
    Fd& operator=(Fd&& other) noexcept;
    ~Fd();

    // True if a descriptor is owned.
    [[nodiscard]] bool valid() const noexcept { return fd_ >= 0; }

    // The descriptor, borrowed for the duration of a call, or -1 if empty.
    [[nodiscard]] int get() const noexcept { return fd_; }

private:
    void reset() noexcept;

    int fd_ = -1;
};

}  // namespace scada::service

#endif  // SCADA_SERVICE_FD_H
