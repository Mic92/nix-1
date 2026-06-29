#pragma once
///@file
#include "nix/util/url.hh"

#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace nix {

/**
 * Parsed Google Cloud Storage `gs://` URL.
 *
 * GCS is accessed via its XML API at `https://storage.googleapis.com`, which
 * is intentionally S3-compatible for basic object operations. Unlike S3, GCS
 * has no region in the URL (buckets are globally addressable) and no profile
 * concept (authentication uses OAuth2 bearer tokens, see gcp-creds.hh). We
 * always use path-style addressing (`/bucket/key`), which works for all bucket
 * names including those containing dots.
 */
struct ParsedGCSURL
{
    std::string bucket;
    /**
     * @see ParsedURL::path. This is a vector for the same reason.
     * Unlike ParsedURL::path this doesn't include the leading empty segment,
     * since the bucket name is necessary.
     */
    std::vector<std::string> key;
    /** `https` (default) or `http`. */
    std::optional<std::string> scheme;
    /** Billing project for requester-pays buckets (sent as `x-goog-user-project`). */
    std::optional<std::string> userProject;
    /** Object generation (GCS object versioning), passed through as a query parameter. */
    std::optional<std::string> generation;
    /**
     * The endpoint can be either missing (defaults to `storage.googleapis.com`),
     * be an absolute URI (with a scheme like `http:`), or an authority
     * (so an IP address or a registered name).
     */
    std::variant<std::monostate, ParsedURL, ParsedURL::Authority> endpoint;

    static ParsedGCSURL parse(const ParsedURL & uri);

    /**
     * Convert this ParsedGCSURL to an HTTP(S) ParsedURL targeting the GCS XML API.
     * The scheme defaults to HTTPS but respects the 'scheme' setting and custom
     * endpoint schemes. Path-style addressing is always used.
     */
    ParsedURL toHttpsUrl() const;

    auto operator<=>(const ParsedGCSURL & other) const = default;
};

} // namespace nix
