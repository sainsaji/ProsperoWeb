// SPDX-License-Identifier: GPL-3.0-or-later
//
// The portable half of the --evo-host mode (src/ps5/evohost): EVO's wire
// messages and the two shared frame buffers.
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include <doctest/doctest.h>

#include "evo_host_frames.h"

using namespace prosperoweb;
using namespace prosperoweb::evohost;

TEST_SUITE("evo_wire") {
  TEST_CASE("a packed message parses back") {
    const evob_key k{13, 1, 1};
    const auto pkt = pack(EVOB_KEY, &k, sizeof k, "a");
    Packet p;
    REQUIRE(parse(pkt.data(), pkt.size(), &p));
    CHECK(p.type == EVOB_KEY);
    evob_key back{};
    REQUIRE(p.body(&back));
    CHECK(back.key_code == 13);
    CHECK(back.modifiers == 1);
    CHECK(back.pressed == 1);
    CHECK(p.text(sizeof back) == "a");
  }

  TEST_CASE("a message without a struct carries only text") {
    const auto pkt = pack(EVOB_LOAD, nullptr, 0, "https://example.com/");
    Packet p;
    REQUIRE(parse(pkt.data(), pkt.size(), &p));
    CHECK(p.type == EVOB_LOAD);
    CHECK(p.text() == "https://example.com/");
  }

  TEST_CASE("text is cut to fit one packet") {
    const std::string big(EVOB_MAX_PACKET * 2, 'x');
    const evob_event ev{EVOB_EV_SCRIPT, 0};
    std::size_t cut = 0;
    const auto pkt = pack(EVOB_EVENT, &ev, sizeof ev, big, &cut);
    CHECK(pkt.size() == EVOB_MAX_PACKET);
    CHECK(cut == big.size() - (EVOB_MAX_PACKET - sizeof(evob_hdr) - sizeof ev));
    Packet p;
    CHECK(parse(pkt.data(), pkt.size(), &p));
  }

  TEST_CASE("foreign or broken packets are refused") {
    const evob_flag f{1};
    auto pkt = pack(EVOB_VISIBLE, &f, sizeof f);
    Packet p;
    CHECK_FALSE(parse(pkt.data(), pkt.size() - 1, &p));  // length disagrees
    CHECK_FALSE(parse(pkt.data(), 4, &p));                // shorter than a header
    pkt[0] ^= 0xFF;                                       // magic
    CHECK_FALSE(parse(pkt.data(), pkt.size(), &p));
    CHECK_FALSE(parse(nullptr, 0, &p));
    evob_size s{};
    const auto empty = pack(EVOB_RESIZE, nullptr, 0);
    REQUIRE(parse(empty.data(), empty.size(), &p));
    CHECK_FALSE(p.body(&s));  // RESIZE without its struct
  }
}

TEST_SUITE("evo_frames") {
  constexpr int W = 64;
  constexpr int H = 32;

  std::vector<std::uint32_t> cache_with(std::uint32_t argb) {
    return std::vector<std::uint32_t>(static_cast<std::size_t>(W) * H, argb);
  }

  TEST_CASE("the first frame is the whole page, R and B swapped") {
    FramePair fp;
    fp.reset(W, H);
    auto cache = cache_with(0xFF112233u);  // ARGB: A ff, R 11, G 22, B 33
    std::vector<std::uint32_t> buf(static_cast<std::size_t>(W) * H, 0);
    const int b = fp.next_buffer();
    REQUIRE(b == 0);
    evob_frame f;
    fp.publish(static_cast<std::uint32_t>(b), cache.data(), W, buf.data(), W, &f);
    CHECK(f.buf == 0);
    CHECK(f.seq == 1);
    REQUIRE(f.nrects == 1);
    CHECK(f.rects[0].w == W);
    CHECK(f.rects[0].h == H);
    // RGBA in memory: bytes 11 22 33 ff, the word 0xFF332211.
    CHECK(buf[0] == 0xFF332211u);
    CHECK(buf.back() == 0xFF332211u);
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(buf.data());
    CHECK(bytes[0] == 0x11);
    CHECK(bytes[1] == 0x22);
    CHECK(bytes[2] == 0x33);
    CHECK(bytes[3] == 0xFF);
    CHECK(fp.next_buffer() == -1);  // nothing new
  }

  TEST_CASE("buffers alternate and a held buffer is never written") {
    FramePair fp;
    fp.reset(W, H);
    auto cache = cache_with(0xFF000000u);
    std::vector<std::uint32_t> b0(cache.size()), b1(cache.size());
    std::uint32_t* bufs[2] = {b0.data(), b1.data()};
    evob_frame f;
    fp.publish(0, cache.data(), W, bufs[0], W, &f);

    display::Region d;
    d.add(display::Rect{0, 0, 8, 8});
    fp.note_damage(d);
    REQUIRE(fp.next_buffer() == 1);
    fp.publish(1, cache.data(), W, bufs[1], W, &f);
    // buffer 1 had never been published: it gets the whole page, while the
    // FRAME names only the new damage.
    CHECK(f.nrects == 1);
    CHECK(f.rects[0].w == 8);

    fp.note_damage(d);
    CHECK(fp.next_buffer() == -1);  // EVO holds both
    fp.released(1);
    CHECK(fp.next_buffer() == 1);
    fp.released(0);
    CHECK(fp.next_buffer() == 0);  // alternates
  }

  TEST_CASE("a buffer catches up on everything it missed") {
    FramePair fp;
    fp.reset(W, H);
    auto cache = cache_with(0xFF000000u);
    std::vector<std::uint32_t> b0(cache.size()), b1(cache.size());
    evob_frame f;
    fp.publish(0, cache.data(), W, b0.data(), W, &f);
    fp.publish(1, cache.data(), W, b1.data(), W, &f);
    fp.released(0);

    // Two changes while buffer 1 is held: buffer 0 publishes the first ...
    cache[0] = 0xFFFF0000u;  // red at (0,0)
    display::Region a;
    a.add(display::Rect{0, 0, 1, 1});
    fp.note_damage(a);
    REQUIRE(fp.next_buffer() == 0);
    fp.publish(0, cache.data(), W, b0.data(), W, &f);

    cache[static_cast<std::size_t>(W) * (H - 1) + (W - 1)] = 0xFF0000FFu;  // blue, far corner
    display::Region b;
    b.add(display::Rect{W - 1, H - 1, 1, 1});
    fp.note_damage(b);
    fp.released(1);
    REQUIRE(fp.next_buffer() == 1);
    fp.publish(1, cache.data(), W, b1.data(), W, &f);
    // ... and buffer 1 gets both, though its FRAME names only the second.
    CHECK(b1[0] == 0xFF0000FFu);  // red as RGBA
    CHECK(b1.back() == 0xFFFF0000u);  // blue as RGBA
    REQUIRE(f.nrects == 1);
    CHECK(f.rects[0].x == W - 1);
  }

  TEST_CASE("damage outside the page is ignored") {
    FramePair fp;
    fp.reset(W, H);
    auto cache = cache_with(0);
    std::vector<std::uint32_t> b0(cache.size());
    evob_frame f;
    fp.publish(0, cache.data(), W, b0.data(), W, &f);
    display::Region d;
    d.add(display::Rect{W + 10, 0, 5, 5});
    fp.note_damage(d);
    CHECK(fp.next_buffer() == -1);
  }
}
