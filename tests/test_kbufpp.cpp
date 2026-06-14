// SPDX-License-Identifier: GPL-2.0
//
// test_kbufpp.cpp - GoogleTest suite for the kbuf++ RAII wrapper (kbufpp.hpp).
//
// Runs inside the QEMU guest like the C tests. Each case opens /dev/kbuf0,
// drives it through the C++ handles, and asserts on observable behaviour:
// byte round-trips, the ioctl UAPI, move semantics, the mmap ring, and the
// dma-buf export/import path. Returns nonzero (GoogleTest's convention) on any
// failure, which the verification framework reports as a failed test.

#include <array>
#include <cstring>
#include <vector>

#include <gtest/gtest.h>

#include "kbufpp.hpp"

namespace {

constexpr const char *kDev = "/dev/kbuf0";

// A fresh, drained, blocking-mode device for each test.
kbuf::Device fresh()
{
	kbuf::Device d(kDev);
	d.set_mode(kbuf::Mode::blocking);
	d.reset();
	return d; // move out (exercises the move constructor every test)
}

std::span<const std::byte> as_bytes(const std::string &s)
{
	return { reinterpret_cast<const std::byte *>(s.data()), s.size() };
}

TEST(Device, OpensAndReportsGeometry)
{
	kbuf::Device d(kDev);
	auto st = d.stats();
	EXPECT_GT(st.num_buffers, 0u);
	EXPECT_GT(st.buffer_size, 0u);
}

TEST(Device, ByteRoundTrip)
{
	auto d = fresh();
	const std::string msg = "hello kbuf++";
	ASSERT_EQ(d.write(as_bytes(msg)), msg.size());

	std::array<std::byte, 64> buf{};
	std::size_t n = d.read(buf);
	ASSERT_EQ(n, msg.size());
	EXPECT_EQ(0, std::memcmp(buf.data(), msg.data(), n));
}

TEST(Device, StatsCountTraffic)
{
	auto d = fresh();
	const std::string msg = "0123456789";
	d.write(as_bytes(msg));

	auto before = d.stats();
	EXPECT_EQ(before.bytes_produced, msg.size());
	EXPECT_EQ(before.msgs_produced, 1u);

	std::array<std::byte, 16> buf{};
	d.read(buf);
	auto after = d.stats();
	EXPECT_EQ(after.bytes_consumed, msg.size());

	d.reset();
	EXPECT_EQ(d.stats().bytes_produced, 0u);
}

TEST(Device, ResizeOnEmptyRing)
{
	auto d = fresh();
	d.resize(16, 1024);
	auto st = d.stats();
	EXPECT_EQ(st.num_buffers, 16u);
	EXPECT_EQ(st.buffer_size, 1024u);
}

TEST(Device, ModeSwitchRoundTrips)
{
	auto d = fresh();
	d.set_mode(kbuf::Mode::spsc);   // ring is empty -> allowed
	d.set_mode(kbuf::Mode::blocking);
	SUCCEED();
}

TEST(Device, MoveLeavesSourceEmpty)
{
	kbuf::Device a(kDev);
	int fd = a.fd();
	kbuf::Device b = std::move(a);
	EXPECT_EQ(b.fd(), fd);
	EXPECT_LT(a.fd(), 0); // moved-from holds no descriptor
}

TEST(UniqueFd, ClosesOnceAndReleases)
{
	kbuf::unique_fd f(::open(kDev, O_RDWR));
	ASSERT_TRUE(static_cast<bool>(f));
	int raw = f.release();
	EXPECT_FALSE(static_cast<bool>(f));
	EXPECT_GE(raw, 0);
	::close(raw);
}

TEST(MappedRing, SpanRoundTrip)
{
	kbuf::Device d(kDev);
	auto ring = d.map();
	ring.reset();
	EXPECT_EQ(ring.capacity(), static_cast<std::size_t>(KBUF_MMAP_CAPACITY));

	std::vector<std::byte> in(4096);
	for (std::size_t i = 0; i < in.size(); ++i)
		in[i] = static_cast<std::byte>(i);
	ASSERT_EQ(ring.write(in), in.size());

	std::vector<std::byte> out(in.size());
	ASSERT_EQ(ring.read(out), out.size());
	EXPECT_EQ(in, out);
}

TEST(Dmabuf, ExportImportAliasesRing)
{
	kbuf::Device d(kDev);
	auto ring = d.map();
	auto bytes = ring.data();

	const std::uint32_t magic = 0xABCD1234u;
	std::memcpy(bytes.data(), &magic, sizeof(magic));      // word[0]
	std::memset(bytes.data() + sizeof(magic), 0, sizeof(magic)); // word[1]

	auto dbuf = d.export_dmabuf();
	ASSERT_TRUE(static_cast<bool>(dbuf));

	// Importer attaches/maps/vmaps and echoes word[0] -> word[1].
	d.run_import_selftest(dbuf);

	std::uint32_t echoed = 0;
	std::memcpy(&echoed, bytes.data() + sizeof(magic), sizeof(echoed));
	EXPECT_EQ(echoed, magic);
}

} // namespace
