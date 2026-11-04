// SPDX-License-Identifier: GPL-3.0-or-later
//
// The portable half of the --evo-host mode (frontend/ps5/evo_host.cpp):
// packing and parsing the EVO wire messages (evo_browser_wire.h) and the
// bookkeeping of the two shared frame buffers. No WebKit, no sockets, so the
// host tests cover it.
//
// The host keeps one private page cache (ARGB32, what WKPagePaint writes)
// and two shared buffers EVO maps (RGBA8, premultiplied). A buffer that EVO
// still holds is never written. Each buffer remembers what changed in the
// cache since it was last brought up to date, so after a publish it equals
// the cache everywhere; the FRAME message carries only what changed since
// the previous FRAME, which is what EVO copies into its texture.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <vector>

#include "damage_region.h"
#include "evo_browser_wire.h"

namespace prosperoweb::evohost {

// One packet: header, the fixed struct `body` (body_len bytes), then `text`.
// The text is cut so the packet fits EVOB_MAX_PACKET; *cut gets how many
// bytes were dropped.
std::vector<std::uint8_t> pack(std::uint16_t type, const void* body, std::size_t body_len,
                               std::string_view text = {}, std::size_t* cut = nullptr);

// A parsed packet. payload points into the caller's buffer.
struct Packet {
  std::uint16_t type = 0;
  const std::uint8_t* payload = nullptr;
  std::size_t len = 0;

  // The fixed struct at the front of the payload; false if it is shorter.
  template <class T>
  bool body(T* out) const {
    if (len < sizeof(T)) {
      return false;
    }
    std::memcpy(out, payload, sizeof(T));
    return true;
  }
  // The bytes after a fixed struct of `skip` bytes (the whole payload for 0).
  std::string_view text(std::size_t skip = 0) const {
    if (skip > len) {
      return {};
    }
    return {reinterpret_cast<const char*>(payload) + skip, len - skip};
  }
};

// False for a packet that is not ours (short, bad magic or version, or a
// length that disagrees with the packet size).
bool parse(const std::uint8_t* data, std::size_t n, Packet* out);

class FramePair {
 public:
  // Both buffers start free and wholly out of date over width x height.
  void reset(int width, int height);

  // The cache was repainted in `dirty` (page pixels).
  void note_damage(const display::Region& dirty);
  // EVO sent RELEASE for buf.
  void released(std::uint32_t buf);
  bool is_free(std::uint32_t buf) const { return buf < 2 && free_[buf]; }

  // The buffer to publish next: a free one, while anything changed since the
  // last FRAME. -1 when there is nothing to send or no buffer is free.
  int next_buffer() const;

  // Brings buffer `buf` up to date from the cache (R and B swapped) and fills
  // `frame` with the rects that changed since the previous FRAME (their
  // bounding box if there are more than EVOB_MAX_RECTS). The buffer is then
  // EVO's until it is released.
  void publish(std::uint32_t buf, const std::uint32_t* cache, int cache_stride_px,
               std::uint32_t* dst, int dst_stride_px, evob_frame* frame);

  std::uint32_t seq() const { return seq_; }

 private:
  display::Rect bounds_;
  display::Region pending_[2];   // per buffer: changed since it was last published
  display::Region frame_damage_;  // changed since the last FRAME
  bool free_[2] = {true, true};
  int last_ = 1;                  // the buffer published last
  std::uint32_t seq_ = 0;
};

}  // namespace prosperoweb::evohost
