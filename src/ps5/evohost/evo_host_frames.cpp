// SPDX-License-Identifier: GPL-3.0-or-later
#include "evo_host_frames.h"

#include <algorithm>
#include <cstring>

#include "compose.h"

namespace prosperoweb::evohost {

std::vector<std::uint8_t> pack(std::uint16_t type, const void* body, std::size_t body_len,
                               std::string_view text, std::size_t* cut) {
  const std::size_t room =
      EVOB_MAX_PACKET > sizeof(evob_hdr) + body_len ? EVOB_MAX_PACKET - sizeof(evob_hdr) - body_len : 0;
  const std::size_t text_len = std::min(text.size(), room);
  if (cut) {
    *cut = text.size() - text_len;
  }
  evob_hdr h{EVOB_MAGIC, EVOB_VERSION, type, static_cast<std::uint32_t>(body_len + text_len)};
  std::vector<std::uint8_t> out(sizeof h + body_len + text_len);
  std::memcpy(out.data(), &h, sizeof h);
  if (body_len) {
    std::memcpy(out.data() + sizeof h, body, body_len);
  }
  if (text_len) {
    std::memcpy(out.data() + sizeof h + body_len, text.data(), text_len);
  }
  return out;
}

bool parse(const std::uint8_t* data, std::size_t n, Packet* out) {
  evob_hdr h;
  if (!data || n < sizeof h) {
    return false;
  }
  std::memcpy(&h, data, sizeof h);
  if (h.magic != EVOB_MAGIC || h.version != EVOB_VERSION || h.len != n - sizeof h) {
    return false;
  }
  out->type = h.type;
  out->payload = data + sizeof h;
  out->len = h.len;
  return true;
}

void FramePair::reset(int width, int height) {
  bounds_ = display::Rect{0, 0, width, height};
  for (auto& p : pending_) {
    p.clear();
    p.add(bounds_);
  }
  frame_damage_.clear();
  frame_damage_.add(bounds_);
}

void FramePair::note_damage(const display::Region& dirty) {
  display::Region d = dirty;
  d.clip(bounds_);
  if (d.empty()) {
    return;
  }
  pending_[0].add(d);
  pending_[1].add(d);
  frame_damage_.add(d);
}

void FramePair::released(std::uint32_t buf) {
  if (buf < 2) {
    free_[buf] = true;
  }
}

int FramePair::next_buffer() const {
  if (frame_damage_.empty()) {
    return -1;
  }
  const int other = last_ ^ 1;  // alternate when both are free
  if (free_[other]) {
    return other;
  }
  return free_[last_] ? last_ : -1;
}

void FramePair::publish(std::uint32_t buf, const std::uint32_t* cache, int cache_stride_px,
                        std::uint32_t* dst, int dst_stride_px, evob_frame* frame) {
  std::memset(frame, 0, sizeof *frame);
  if (buf >= 2) {
    return;
  }
  for (const display::Rect& r : pending_[buf].rects()) {
    for (int y = r.y; y < r.bottom(); y++) {
      display::copy_swap_row(cache + static_cast<std::size_t>(y) * cache_stride_px + r.x,
                             dst + static_cast<std::size_t>(y) * dst_stride_px + r.x, r.w);
    }
  }
  auto to_wire = [](const display::Rect& r) {
    return evob_rect{static_cast<std::uint16_t>(r.x), static_cast<std::uint16_t>(r.y),
                     static_cast<std::uint16_t>(r.w), static_cast<std::uint16_t>(r.h)};
  };
  const auto& rects = frame_damage_.rects();
  if (rects.size() > EVOB_MAX_RECTS) {
    frame->rects[0] = to_wire(frame_damage_.bounds());
    frame->nrects = 1;
  } else {
    for (const display::Rect& r : rects) {
      frame->rects[frame->nrects++] = to_wire(r);
    }
  }
  frame->buf = buf;
  frame->seq = ++seq_;
  pending_[buf].clear();
  frame_damage_.clear();
  free_[buf] = false;
  last_ = static_cast<int>(buf);
}

}  // namespace prosperoweb::evohost
