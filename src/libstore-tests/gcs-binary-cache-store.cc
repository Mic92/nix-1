#include "nix/store/gcs-binary-cache-store.hh"

#include <gtest/gtest.h>

namespace nix {

TEST(GCSBinaryCacheStore, resolvesEndpointFromParams)
{
    Store::Config::Params params{
        {"endpoint", "localhost:4443"},
        {"scheme", "http"},
        {"user-project", "billing-proj"},
        {"storage-class", "NEARLINE"},
    };
    GCSBinaryCacheStoreConfig config{"my-bucket", params};

    EXPECT_EQ(config.cacheUri.scheme, "gs");
    EXPECT_TRUE(config.cacheUri.query.empty());
    EXPECT_TRUE(std::holds_alternative<ParsedURL::Authority>(config.resolvedEndpoint));
    EXPECT_EQ(config.userProject.get(), "billing-proj");
    EXPECT_EQ(config.storageClass.get(), std::optional<std::string>{"NEARLINE"});
}

TEST(GCSBinaryCacheStore, rejectsTooSmallChunkSize)
{
    EXPECT_THROW(
        (GCSBinaryCacheStoreConfig{"foobar", {{"multipart-upload", "true"}, {"multipart-chunk-size", "1048576"}}}),
        UsageError);
}

} // namespace nix
