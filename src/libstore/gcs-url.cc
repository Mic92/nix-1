#include "nix/store/gcs-url.hh"
#include "nix/util/error.hh"
#include "nix/util/util.hh"

#include <ranges>
#include <string_view>

using namespace std::string_view_literals;

namespace nix {

ParsedGCSURL ParsedGCSURL::parse(const ParsedURL & parsed)
try {
    if (parsed.scheme != "gs"sv)
        throw BadURL("URI scheme '%s' is not 'gs'", parsed.scheme);

    /* Like S3, the bucket name is the URI authority.
       TODO: Validate against https://cloud.google.com/storage/docs/buckets#naming
       (3-63 chars, lowercase/digits/dash/underscore/dot, no `goog` prefix, …). */
    if (!parsed.authority || parsed.authority->host.empty()
        || parsed.authority->hostType != ParsedURL::Authority::HostType::Name)
        throw BadURL("URI has a missing or invalid bucket name");

    auto getOptionalParam = [&](std::string_view key) -> std::optional<std::string> {
        const auto & query = parsed.query;
        auto it = query.find(key);
        if (it == query.end())
            return std::nullopt;
        return it->second;
    };

    auto endpoint = getOptionalParam("endpoint");
    if (parsed.path.size() <= 1 || !parsed.path.front().empty())
        throw BadURL("URI has a missing or invalid key");

    auto path = std::views::drop(parsed.path, 1) | std::ranges::to<std::vector<std::string>>();

    return ParsedGCSURL{
        .bucket = parsed.authority->host,
        .key = std::move(path),
        .scheme = getOptionalParam("scheme"),
        .userProject = getOptionalParam("user-project"),
        .generation = getOptionalParam("generation"),
        .endpoint = [&]() -> decltype(ParsedGCSURL::endpoint) {
            if (!endpoint)
                return std::monostate();
            return std::visit(
                [](auto v) -> decltype(ParsedGCSURL::endpoint) { return v; }, parseUrlOrAuthority(*endpoint));
        }(),
    };
} catch (BadURL & e) {
    e.addTrace({}, "while parsing GCS URI: '%s'", parsed.to_string());
    throw;
}

ParsedURL ParsedGCSURL::toHttpsUrl() const
{
    /* Resolve scheme/authority/base-path from the endpoint variant, then build
       the URL once. Always path-style: GCS also supports virtual-hosted-style
       (`bucket.storage.googleapis.com`) but path-style works for all bucket
       names including dotted ones, and emulators typically only support it. */
    struct Resolved
    {
        std::string scheme;
        std::optional<ParsedURL::Authority> authority;
        std::vector<std::string> path;
    };

    auto schemeOr = [&](std::string_view def) { return scheme.value_or(std::string{def}); };

    auto resolved = std::visit(
        overloaded{
            [&](std::monostate) -> Resolved {
                return {schemeOr("https"), ParsedURL::Authority{.host = "storage.googleapis.com"}, {""}};
            },
            [&](const ParsedURL::Authority & auth) -> Resolved { return {schemeOr("https"), auth, {""}}; },
            [&](const ParsedURL & url) -> Resolved { return {url.scheme, url.authority, url.path}; },
        },
        endpoint);

    resolved.path.push_back(bucket);
    resolved.path.insert(resolved.path.end(), key.begin(), key.end());

    /* GCS' XML API accepts the object generation as a query parameter on
       GET/HEAD; pass it through so callers can pin a specific version. */
    StringMap query;
    if (generation)
        query["generation"] = *generation;

    return ParsedURL{
        .scheme = std::move(resolved.scheme),
        .authority = std::move(resolved.authority),
        .path = std::move(resolved.path),
        .query = std::move(query),
    };
}

} // namespace nix
