#include <gtest/gtest.h>
#include <zlib.h>

#include <cstdio>
#include <filesystem>

#include "itchbook/gz_reader.hpp"
#include "itchbook/order_book.hpp"
#include "itchbook/synth.hpp"

using namespace itchbook;

// Chunked, gzip-streamed replay must produce exactly the same book as parsing the whole buffer,
// for chunk sizes that split frames at every possible offset.
TEST(Chunker, GzipChunkedReplayEqualsInMemory) {
  const auto buf = generate_synthetic({.seed = 11, .messages = 20000});
  const auto path = std::filesystem::temp_directory_path() / "itchbook_chunker_test.gz";
  {
    gzFile f = gzopen(path.c_str(), "wb");
    ASSERT_NE(f, nullptr);
    ASSERT_EQ(gzwrite(f, buf.data(), static_cast<unsigned>(buf.size())), static_cast<int>(buf.size()));
    gzclose(f);
  }
  auto whole = std::make_unique<OrderBook<>>();
  parse(buf.data(), buf.size(), *whole);

  for (std::size_t chunk : {64ul, 97ul, 4096ul, 1ul << 20}) {
    GzReader r(path.string());
    ASSERT_TRUE(r.ok());
    FrameChunker c(r, chunk);
    auto b = std::make_unique<OrderBook<>>();
    const std::uint8_t* data;
    std::size_t size;
    std::size_t total = 0;
    while (c.fill(data, size)) {
      parse(data, size, *b);
      total += size;
    }
    EXPECT_FALSE(c.error());
    EXPECT_EQ(total, buf.size()) << "chunk=" << chunk;
    EXPECT_TRUE(b->stats() == whole->stats()) << "chunk=" << chunk;
    for (std::uint16_t l = 1; l <= 8; ++l) EXPECT_EQ(b->bbo(l), whole->bbo(l));
  }
  std::filesystem::remove(path);
}
