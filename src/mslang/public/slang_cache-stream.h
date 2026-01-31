#pragma once

// c++ headers ------------------------------------------
#include <cstddef>

#include <iostream>
#include <vector>
#include <span>

namespace mslang {

class VectorOutputBuffer final : public std::streambuf {
public:
  VectorOutputBuffer(std::vector<std::byte>& buffer) : buffer_(buffer) {}

protected:
  // Called when a single character is written
  int_type overflow(int_type ch) override {
    if (ch != traits_type::eof()) {
      buffer_.push_back(static_cast<std::byte>(ch));
      return ch;
    }
    return traits_type::eof();
  }

  // Called when multiple characters are written
  std::streamsize xsputn(const char* s, std::streamsize n) override {
    buffer_.insert(
      buffer_.end(),
      reinterpret_cast<const std::byte*>(s),
      reinterpret_cast<const std::byte*>(s) + n
    );
    return n;
  }

private:
  std::vector<std::byte>& buffer_;
};

class ByteVectorOutputStream final : public std::ostream {
public:
  ByteVectorOutputStream(std::vector<std::byte>& buffer) :
      std::ostream(nullptr),
      buffer_(buffer)
  {
    this->rdbuf(&buffer_);
  }

private:
  VectorOutputBuffer buffer_;
};

class SpanStreambuf : public std::basic_streambuf<char, std::char_traits<char>> {
public:
  SpanStreambuf(std::span<std::byte const> span) {
    // Set the get area pointers to the beginning, current, and end of the vector data
    this->setg(
      const_cast<char*>(reinterpret_cast<char const*>(span.data())), // beginning
      const_cast<char*>(reinterpret_cast<char const*>(span.data())), // current
      const_cast<char*>(reinterpret_cast<char const*>(span.data() + span.size())) // end
    );
  }
};

} // namespace mslang
