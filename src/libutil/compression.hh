#pragma once
///@file

#include "ref.hh"
#include "types.hh"
#include "serialise.hh"

#include <string>

namespace nix {

struct CompressionSink : BufferedSink, FinishSink
{
    using BufferedSink::operator ();
    using BufferedSink::writeUnbuffered;
    using FinishSink::finish;
};

class DecompressionSource : public Source {
public:
   DecompressionSource(Source & src);
   ~DecompressionSource();
   size_t read(char * data, size_t len) override;
};

std::string decompress(const std::string & method, std::string_view in);

std::unique_ptr<FinishSink> makeDecompressionSink(const std::string & method, Sink & nextSink);

std::unique_ptr<DecompressionSource> makeDecompressionSource(const std::string & method, Source & nextSource);

std::string compress(const std::string & method, std::string_view in, const bool parallel = false, int level = -1);

ref<CompressionSink> makeCompressionSink(const std::string & method, Sink & nextSink, const bool parallel = false, int level = -1);

MakeError(UnknownCompressionMethod, Error);

MakeError(CompressionError, Error);

}
