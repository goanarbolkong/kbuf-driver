// SPDX-License-Identifier: GPL-2.0
/*
 * kbufpp.hpp - a modern C++ RAII wrapper for the kbuf character device.
 *
 * Header-only, C++20. Wraps the raw fd / ioctl / mmap UAPI in move-only
 * handles so a device, an exported dma-buf, and an mmap ring are all closed
 * exactly once, on scope exit, even when an exception unwinds. The byte and
 * ring transfer paths take std::span<std::byte>, so callers never pass a bare
 * pointer/length pair. Failures throw std::system_error carrying the errno.
 *
 * The two handles mirror the kernel's two data paths:
 *   - kbuf::Device  - open/read/write/ioctl on /dev/kbufN (blocking or SPSC).
 *   - kbuf::MappedRing - the mmap zero-copy "magic ring", driven by libkbuf's
 *     single-producer/single-consumer atomics.
 *
 * Everything is a thin, zero-overhead inline over the C ABI in kbuf.h /
 * libkbuf.h; there is no hidden allocation and no virtual dispatch.
 */
#ifndef KBUFPP_HPP
#define KBUFPP_HPP

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "kbuf.h"
#include "libkbuf.h"

namespace kbuf {

[[noreturn]] inline void throw_errno(const char *what)
{
	throw std::system_error(errno, std::generic_category(), what);
}

/* Move-only owner of a file descriptor; closes it once on destruction. */
class unique_fd {
public:
	unique_fd() noexcept = default;
	explicit unique_fd(int fd) noexcept : fd_(fd) {}
	~unique_fd() { reset(); }

	unique_fd(unique_fd &&o) noexcept : fd_(std::exchange(o.fd_, -1)) {}
	unique_fd &operator=(unique_fd &&o) noexcept
	{
		if (this != &o) {
			reset();
			fd_ = std::exchange(o.fd_, -1);
		}
		return *this;
	}
	unique_fd(const unique_fd &) = delete;
	unique_fd &operator=(const unique_fd &) = delete;

	int get() const noexcept { return fd_; }
	explicit operator bool() const noexcept { return fd_ >= 0; }
	int release() noexcept { return std::exchange(fd_, -1); }

	void reset(int fd = -1) noexcept
	{
		if (fd_ >= 0)
			::close(fd_);
		fd_ = fd;
	}

private:
	int fd_ = -1;
};

enum class Mode { blocking = KBUF_MODE_BLOCKING, spsc = KBUF_MODE_SPSC };

class MappedRing;

/*
 * RAII handle for a /dev/kbufN character device. Move-only: a moved-from
 * Device holds no fd. Blocking read()/write() forward to the syscall path;
 * the ioctl helpers wrap the versioned UAPI.
 */
class Device {
public:
	explicit Device(std::string_view path, int flags = O_RDWR)
		: fd_(::open(std::string(path).c_str(), flags))
	{
		if (!fd_)
			throw_errno("kbuf::Device::open");
	}

	int fd() const noexcept { return fd_.get(); }

	std::size_t read(std::span<std::byte> out)
	{
		ssize_t n = ::read(fd_.get(), out.data(), out.size());

		if (n < 0)
			throw_errno("kbuf::Device::read");
		return static_cast<std::size_t>(n);
	}

	std::size_t write(std::span<const std::byte> in)
	{
		ssize_t n = ::write(fd_.get(), in.data(), in.size());

		if (n < 0)
			throw_errno("kbuf::Device::write");
		return static_cast<std::size_t>(n);
	}

	kbuf_stats stats() const
	{
		kbuf_stats s{};

		if (::ioctl(fd_.get(), KBUF_IOCGSTATS, &s) != 0)
			throw_errno("KBUF_IOCGSTATS");
		return s;
	}

	void reset()
	{
		if (::ioctl(fd_.get(), KBUF_IOCRESET) != 0)
			throw_errno("KBUF_IOCRESET");
	}

	void resize(std::uint32_t num_buffers, std::uint32_t buffer_size)
	{
		kbuf_resize r{ num_buffers, buffer_size };

		if (::ioctl(fd_.get(), KBUF_IOCRESIZE, &r) != 0)
			throw_errno("KBUF_IOCRESIZE");
	}

	void set_mode(Mode m)
	{
		int v = static_cast<int>(m);

		if (::ioctl(fd_.get(), KBUF_IOCSMODE, &v) != 0)
			throw_errno("KBUF_IOCSMODE");
	}

	/* Export the mmap data ring as a dma-buf; the returned fd owns it. */
	unique_fd export_dmabuf()
	{
		int dfd = -1;

		if (::ioctl(fd_.get(), KBUF_IOCEXPORT, &dfd) != 0)
			throw_errno("KBUF_IOCEXPORT");
		return unique_fd(dfd);
	}

	/* Run the in-kernel importer self-test against an exported dma-buf. */
	void run_import_selftest(const unique_fd &dmabuf)
	{
		int dfd = dmabuf.get();

		if (::ioctl(fd_.get(), KBUF_IOCIMPORT, &dfd) != 0)
			throw_errno("KBUF_IOCIMPORT");
	}

	MappedRing map();

private:
	unique_fd fd_;
};

/*
 * RAII view of the mmap zero-copy ring. Move-only; unmaps on destruction.
 * write()/read() are the single-producer/single-consumer fast path - no
 * syscall, just libkbuf's acquire/release atomics over the shared pages.
 */
class MappedRing {
public:
	explicit MappedRing(int fd)
	{
		if (kbuf_map_open(fd, &m_) != 0)
			throw_errno("kbuf::MappedRing::mmap");
	}
	~MappedRing() { kbuf_map_close(&m_); }

	MappedRing(MappedRing &&o) noexcept : m_(o.m_) { o.m_.base = nullptr; }
	MappedRing &operator=(MappedRing &&o) noexcept
	{
		if (this != &o) {
			kbuf_map_close(&m_);
			m_ = o.m_;
			o.m_.base = nullptr;
		}
		return *this;
	}
	MappedRing(const MappedRing &) = delete;
	MappedRing &operator=(const MappedRing &) = delete;

	void reset() noexcept { kbuf_map_reset(&m_); }
	std::size_t capacity() const noexcept { return m_.cap; }

	/* The raw ring bytes (first of the two contiguous copies); the same
	 * pages a dma-buf export hands out, for callers that index directly.
	 */
	std::span<std::byte> data() noexcept
	{
		return { reinterpret_cast<std::byte *>(m_.data), m_.cap };
	}

	std::size_t write(std::span<const std::byte> in) noexcept
	{
		return kbuf_map_write(&m_, in.data(), in.size());
	}

	std::size_t read(std::span<std::byte> out) noexcept
	{
		return kbuf_map_read(&m_, out.data(), out.size());
	}

private:
	kbuf_map m_{};
};

inline MappedRing Device::map() { return MappedRing(fd_.get()); }

} // namespace kbuf

#endif /* KBUFPP_HPP */
