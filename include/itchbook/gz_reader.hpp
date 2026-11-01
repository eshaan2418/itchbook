// Streaming reader for (optionally gzip-compressed) ITCH files. Uses zlib's gzread, which also
// passes uncompressed input through transparently. "-" reads stdin. Large captures are never
// decompressed to disk: the replay tools decompress chunk-by-chunk into a reusable buffer.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace itchbook {

class GzReader {
 public:
  explicit GzReader(const std::string& path);
  ~GzReader();
  GzReader(const GzReader&) = delete;
  GzReader& operator=(const GzReader&) = delete;

  [[nodiscard]] bool ok() const noexcept { return handle_ != nullptr; }
  // Reads up to n bytes into dst. Returns bytes read; 0 at EOF; -1 on error.
  long read(std::uint8_t* dst, std::size_t n);

 private:
  void* handle_ = nullptr;
};

// Chunked frame source: fills a buffer with whole frames only, carrying a partial trailing
// frame over to the next fill. `fill` returns a [data, size) view valid until the next call.
class FrameChunker {
 public:
  FrameChunker(GzReader& r, std::size_t chunk_bytes) : r_(r), buf_(chunk_bytes) {}
  // Returns false at EOF (or read error) with no complete frames left.
  bool fill(const std::uint8_t*& data, std::size_t& size);
  [[nodiscard]] bool error() const noexcept { return error_; }
  [[nodiscard]] std::size_t bytes_read() const noexcept { return total_; }

 private:
  GzReader& r_;
  std::vector<std::uint8_t> buf_;
  std::size_t consumed_ = 0;   // bytes handed out in the last view
  std::size_t valid_ = 0;      // valid bytes in buf_
  std::size_t total_ = 0;
  bool eof_ = false;
  bool error_ = false;
};

}  // namespace itchbook
