#include <gtest/gtest.h>

#include "nix/store/filetransfer.hh"
#include "nix/store/gcp-creds.hh"

namespace nix {

namespace {
std::optional<std::string> findHeader(const Headers & hs, std::string_view name)
{
    for (auto & [k, v] : hs)
        if (k == name)
            return v;
    return std::nullopt;
}
} // anonymous namespace

#if NIX_WITH_GCS_AUTH
namespace {
struct FixedTokenProvider : GcpCredentialProvider
{
    std::optional<GcpCredentials> result;

    std::optional<GcpCredentials> maybeGetCredentials() override
    {
        return result;
    }

    std::optional<GcpCredentials> tryRefreshCredentials(FileTransfer &) noexcept override
    {
        return result;
    }
};
} // anonymous namespace
#endif

/**
 * Fixture that neutralises the real GCP credential chain to make setupForGCS()
 * tests stay hermetic and never touch the network or host ADC files.
 */
class FileTransferRequestGCS : public ::testing::Test
{
protected:
#if NIX_WITH_GCS_AUTH
    ref<FixedTokenProvider> stub = make_ref<FixedTokenProvider>();

    void SetUp() override
    {
        setGcpCredentialsProviderForTesting(stub);
    }

    void TearDown() override
    {
        setGcpCredentialsProviderForTesting(makeGcpCredentialsProvider());
    }
#endif
};

TEST_F(FileTransferRequestGCS, rewritesUrlAndUserProject)
{
    FileTransferRequest req(VerbatimURL{std::string{"gs://my-bucket/nar/abc.nar.xz?user-project=billing"}});
    req.setupForGCS();

    EXPECT_EQ(req.uri.to_string(), "https://storage.googleapis.com/my-bucket/nar/abc.nar.xz");
    EXPECT_EQ(findHeader(req.headers, "x-goog-user-project"), std::optional<std::string>{"billing"});
}

TEST_F(FileTransferRequestGCS, preResolvedTokenWins)
{
#if NIX_WITH_GCS_AUTH
    /* Ensure the provider would have supplied something.
     * We we know the pre-resolved token actually short-circuits it.
     */
    stub->result = GcpCredentials{.accessToken = "provider-token", .expiresAt = {}};
#endif
    FileTransferRequest req(VerbatimURL{std::string{"gs://b/k"}});
    req.preResolvedGcpAccessToken = "forwarded-token";
    req.setupForGCS();

    EXPECT_EQ(req.bearerToken, std::optional<std::string>{"forwarded-token"});
    EXPECT_FALSE(req.refreshBearerToken);
}

#if NIX_WITH_GCS_AUTH

TEST_F(FileTransferRequestGCS, usesProviderToken)
{
    stub->result = GcpCredentials{.accessToken = "provider-token", .expiresAt = {}};

    FileTransferRequest req(VerbatimURL{std::string{"gs://b/k"}});
    req.setupForGCS();

    EXPECT_EQ(req.bearerToken, std::optional<std::string>{"provider-token"});
    ASSERT_TRUE(req.refreshBearerToken);

    stub->result = GcpCredentials{.accessToken = "refreshed-token", .expiresAt = {}};
    EXPECT_EQ(req.refreshBearerToken(), std::optional<std::string>{"refreshed-token"});
}

TEST_F(FileTransferRequestGCS, anonymousWhenNoCredentials)
{
    stub->result = std::nullopt;

    FileTransferRequest req(VerbatimURL{std::string{"gs://b/k"}});
    req.setupForGCS();

    EXPECT_FALSE(req.bearerToken.has_value());
}

#endif

} // namespace nix
