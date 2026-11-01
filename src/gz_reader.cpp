#include "itchbook/gz_reader.hpp"

#include <unistd.h>
#include <zlib.h>

#include <algorithm>
#include <cstring>

#include "itchbook/parser.hpp"

namespace itchbook {

GzReader::GzReader(const std::string& path) {
  gzFile f = path == "-" ? gzdopen(dup(STDIN_FILENO), "rb") : gzopen(path.c_str(), "rb");
  if (f) gzbuffer(f, 1u << 20);
  handle_ = f;
}

GzReader::~GzReader() {
  if (handle_) gzclose(static_cast<gzFile>(handle_));
}

long GzReader::read(std::uint8_t* dst, std::size_t n) {
  if (!handle_) return -1;
  std::size_t got = 0;
  while (got < n) {
    const unsigned ask = static_cast<unsigned>(std::min<std::size_t>(n - got, 1u << 30));
    const int r = gzread(static_cast<gzFile>(handle_), dst + got, ask);
    if (r < 0) return -1;
    if (r == 0) break;
    got += static_cast<std::size_t>(r);
  }
  return static_cast<long>(got);
}

bool FrameChunker::fill(const std::uint8_t*& data, std::size_t& size) {
  // Move the unconsumed tail of the previous view to the front.
  const std::size_t tail = valid_ - consumed_;
  if (tail && consumed_) std::memmove(buf_.data(), buf_.data() + consumed_, tail);
  valid_ = tail;
  consumed_ = 0;
  if (!eof_) {
    const long r = r_.read(buf_.data() + valid_, buf_.size() - valid_);
    if (r < 0) {
      error_ = true;
      eof_ = true;
    } else {
      if (static_cast<std::size_t>(r) < buf_.size() - valid_) eof_ = true;
      valid_ += static_cast<std::size_t>(r);
      total_ += static_cast<std::size_t>(r);
    }
  }
  // Find the end of the last complete frame.
  const std::size_t whole = for_each_frame(buf_.data(), valid_, [](const std::uint8_t*, std::uint16_t) {});
  if (whole == 0) return false;
  data = buf_.data();
  size = whole;
  consumed_ = whole;
  return true;
}

}  // namespace itchbook
