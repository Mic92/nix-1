#include "nix/store/gcs-url.hh"
#include "nix/util/tests/gmock-matchers.hh"

#include <gtest/gtest.h>
#include <gmock/gmock.h>

namespace nix {

// =============================================================================
// ParsedGCSURL parse tests
// =============================================================================

struct ParsedGCSURLTestCase
{
    std::string url;
    ParsedGCSURL expected;
    std::string description;
};

class ParsedGCSURLTest : public ::testing::WithParamInterface<ParsedGCSURLTestCase>, public ::testing::Test
{};

TEST_P(ParsedGCSURLTest, parseGCSURLSuccessfully)
{
    const auto & testCase = GetParam();
    auto parsed = ParsedGCSURL::parse(parseURL(testCase.url));
    ASSERT_EQ(parsed, testCase.expected);
}

INSTANTIATE_TEST_SUITE_P(
    QueryParams,
    ParsedGCSURLTest,
    ::testing::Values(
        ParsedGCSURLTestCase{
            "gs://my-bucket/my-key.txt",
            {
                .bucket = "my-bucket",
                .key = {"my-key.txt"},
            },
            "basic_gs_bucket",
        },
        ParsedGCSURLTestCase{
            "gs://prod-cache/nix/store/abc123.nar.xz",
            {
                .bucket = "prod-cache",
                .key = {"nix", "store", "abc123.nar.xz"},
            },
            "nested_key",
        },
        ParsedGCSURLTestCase{
            "gs://bucket/key?endpoint=custom.gcs.local&scheme=http",
            {
                .bucket = "bucket",
                .key = {"key"},
                .scheme = "http",
                .endpoint = ParsedURL::Authority{.host = "custom.gcs.local"},
            },
            "custom_endpoint_authority",
        },
        ParsedGCSURLTestCase{
            /* A bare host:port endpoint must parse as an authority, not as a
               URL with scheme "localhost". */
            "gs://bucket/key?endpoint=localhost:4443",
            {
                .bucket = "bucket",
                .key = {"key"},
                .endpoint = ParsedURL::Authority{.host = "localhost", .port = 4443},
            },
            "bare_host_port_endpoint",
        },
        ParsedGCSURLTestCase{
            "gs://bucket/key?endpoint=http://server:9000",
            {
                .bucket = "bucket",
                .key = {"key"},
                .endpoint =
                    ParsedURL{
                        .scheme = "http",
                        .authority = ParsedURL::Authority{.host = "server", .port = 9000},
                        .path = {""},
                    },
            },
            "custom_endpoint_full_url",
        },
        ParsedGCSURLTestCase{
            "gs://bucket/key?endpoint=http://server:9000&scheme=http&user-project=proj&generation=42",
            {
                .bucket = "bucket",
                .key = {"key"},
                .scheme = "http",
                .userProject = "proj",
                .generation = "42",
                .endpoint =
                    ParsedURL{
                        .scheme = "http",
                        .authority = ParsedURL::Authority{.host = "server", .port = 9000},
                        .path = {""},
                    },
            },
            "all_params",
        }),
    [](const ::testing::TestParamInfo<ParsedGCSURLTestCase> & info) { return info.param.description; });

// =============================================================================
// Invalid GCS URL tests
// =============================================================================

struct InvalidGCSURLTestCase
{
    std::string url;
    std::string expectedErrorSubstring;
    std::string description;
};

class InvalidParsedGCSURLTest : public ::testing::WithParamInterface<InvalidGCSURLTestCase>, public ::testing::Test
{};

TEST_P(InvalidParsedGCSURLTest, parseGCSURLErrors)
{
    const auto & testCase = GetParam();

    ASSERT_THAT(
        [&testCase]() { ParsedGCSURL::parse(parseURL(testCase.url)); },
        ::testing::ThrowsMessage<BadURL>(testing::HasSubstrIgnoreANSIMatcher(testCase.expectedErrorSubstring)));
}

INSTANTIATE_TEST_SUITE_P(
    InvalidUrls,
    InvalidParsedGCSURLTest,
    ::testing::Values(
        InvalidGCSURLTestCase{"gs:///key", "error: URI has a missing or invalid bucket name", "empty_bucket"},
        InvalidGCSURLTestCase{"gs://127.0.0.1", "error: URI has a missing or invalid bucket name", "ip_address_bucket"},
        InvalidGCSURLTestCase{"gs://", "error: URI has a missing or invalid bucket name", "completely_empty"},
        InvalidGCSURLTestCase{"gs://bucket", "error: URI has a missing or invalid key", "missing_key"},
        InvalidGCSURLTestCase{"s3://bucket/key", "error: URI scheme 's3' is not 'gs'", "wrong_scheme"}),
    [](const ::testing::TestParamInfo<InvalidGCSURLTestCase> & info) { return info.param.description; });

// =============================================================================
// GCS URL to HTTPS conversion tests
// =============================================================================

struct GCSToHttpsConversionTestCase
{
    ParsedGCSURL input;
    ParsedURL expected;
    std::string expectedRendered;
    std::string description;
};

class GCSToHttpsConversionTest : public ::testing::WithParamInterface<GCSToHttpsConversionTestCase>,
                                 public ::testing::Test
{};

TEST_P(GCSToHttpsConversionTest, ConvertsCorrectly)
{
    const auto & testCase = GetParam();
    auto result = testCase.input.toHttpsUrl();
    EXPECT_EQ(result, testCase.expected) << "Failed for: " << testCase.description;
    EXPECT_EQ(result.to_string(), testCase.expectedRendered);
}

INSTANTIATE_TEST_SUITE_P(
    GCSToHttpsConversion,
    GCSToHttpsConversionTest,
    ::testing::Values(
        // Default endpoint: path-style at storage.googleapis.com
        GCSToHttpsConversionTestCase{
            ParsedGCSURL{
                .bucket = "my-bucket",
                .key = {"my-key.txt"},
            },
            ParsedURL{
                .scheme = "https",
                .authority = ParsedURL::Authority{.host = "storage.googleapis.com"},
                .path = {"", "my-bucket", "my-key.txt"},
            },
            "https://storage.googleapis.com/my-bucket/my-key.txt",
            "basic_default_endpoint",
        },
        GCSToHttpsConversionTestCase{
            ParsedGCSURL{
                .bucket = "prod-cache",
                .key = {"nix", "store", "abc123.nar.xz"},
            },
            ParsedURL{
                .scheme = "https",
                .authority = ParsedURL::Authority{.host = "storage.googleapis.com"},
                .path = {"", "prod-cache", "nix", "store", "abc123.nar.xz"},
            },
            "https://storage.googleapis.com/prod-cache/nix/store/abc123.nar.xz",
            "nested_key_default_endpoint",
        },
        // Custom endpoint as authority
        GCSToHttpsConversionTestCase{
            ParsedGCSURL{
                .bucket = "bucket",
                .key = {"key"},
                .scheme = "http",
                .endpoint = ParsedURL::Authority{.host = "custom.gcs.local"},
            },
            ParsedURL{
                .scheme = "http",
                .authority = ParsedURL::Authority{.host = "custom.gcs.local"},
                .path = {"", "bucket", "key"},
            },
            "http://custom.gcs.local/bucket/key",
            "custom_endpoint_authority",
        },
        // Custom endpoint as full URL with port
        GCSToHttpsConversionTestCase{
            ParsedGCSURL{
                .bucket = "bucket",
                .key = {"key"},
                .endpoint =
                    ParsedURL{
                        .scheme = "http",
                        .authority = ParsedURL::Authority{.host = "server", .port = 9000},
                        .path = {""},
                    },
            },
            ParsedURL{
                .scheme = "http",
                .authority = ParsedURL::Authority{.host = "server", .port = 9000},
                .path = {"", "bucket", "key"},
            },
            "http://server:9000/bucket/key",
            "custom_endpoint_with_port",
        },
        // generation maps to query parameter
        GCSToHttpsConversionTestCase{
            ParsedGCSURL{
                .bucket = "bucket",
                .key = {"key"},
                .generation = "1234567890",
            },
            ParsedURL{
                .scheme = "https",
                .authority = ParsedURL::Authority{.host = "storage.googleapis.com"},
                .path = {"", "bucket", "key"},
                .query = {{"generation", "1234567890"}},
            },
            "https://storage.googleapis.com/bucket/key?generation=1234567890",
            "with_generation",
        }),
    [](const ::testing::TestParamInfo<GCSToHttpsConversionTestCase> & info) { return info.param.description; });

} // namespace nix
