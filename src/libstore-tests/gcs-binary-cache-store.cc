#include "nix/store/gcs-binary-cache-store.hh"

#include <gtest/gtest.h>

namespace nix {

TEST(GCSBinaryCacheStore, uriSettingsCarriedInQuery)
{
    /* The bucket becomes the authority, and scheme/endpoint/user-project are
       recovered per request from the URL by setupForGCS(), so the config must
       copy them into cacheUri.query. */
    Store::Config::Params params{
        {"endpoint", "localhost:4443"},
        {"user-project", "billing-proj"},
        {"storage-class", "NEARLINE"},
    };
    GCSBinaryCacheStoreConfig config{"my-bucket", params};

    EXPECT_EQ(config.cacheUri.scheme, "gs");
    EXPECT_EQ(config.cacheUri.authority, (ParsedURL::Authority{.host = "my-bucket"}));
    EXPECT_EQ(config.cacheUri.query.at("endpoint"), "localhost:4443");
    EXPECT_EQ(config.cacheUri.query.at("user-project"), "billing-proj");
    /* storage-class is sent as a header, not a URL param. */
    EXPECT_FALSE(config.cacheUri.query.contains("storage-class"));
    EXPECT_EQ(config.storageClass.get(), std::optional<std::string>{"NEARLINE"});
}

TEST(GCSBinaryCacheStore, rejectsTooSmallChunkSize)
{
    EXPECT_THROW(
        (GCSBinaryCacheStoreConfig{"foobar", {{"multipart-upload", "true"}, {"multipart-chunk-size", "1048576"}}}),
        UsageError);
}

} // namespace nix
