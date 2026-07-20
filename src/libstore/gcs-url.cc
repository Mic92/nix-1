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
     * Only reject the cases that would make the resulting HTTPS URL malformed. */
    if (!parsed.authority || parsed.authority->host.empty()
        || parsed.authority->hostType != ParsedURL::Authority::HostType::Name)
        throw BadURL("URI has a missing or invalid bucket name");

    auto getOptionalParam = [&](std::string_view key) -> std::optional<std::string> {
        auto it = parsed.query.find(key);
        if (it == parsed.query.end())
            return std::nullopt;
        return it->second;
    };

    /* See the struct docstring:
     * Refusing here means a `gs://` URL cannot name a non-Google host,
     * so the bearer token can never be leaked elsewhere.
     */
    if (getOptionalParam("endpoint") || getOptionalParam("scheme"))
        throw BadURL("'endpoint' and 'scheme' are not accepted in a gs:// URL; configure them on the store instead");

    if (parsed.path.size() <= 1 || !parsed.path.front().empty())
        throw BadURL("URI has a missing or invalid key");

    auto path = std::views::drop(parsed.path, 1) | std::ranges::to<std::vector<std::string>>();

    for (auto & seg : path)
        if (seg.empty() || seg == "." || seg == "..")
            throw BadURL("URI key has an invalid path segment '%s'", seg);

    return ParsedGCSURL{
        .bucket = parsed.authority->host,
        .key = std::move(path),
        /* Goes verbatim into the `x-goog-user-project` header.
         * Restrict to the GCP project-id charset so it cannot smuggle CR/LF from a URL.
         */
        .userProject = [&]() -> std::optional<std::string> {
            auto v = getOptionalParam("user-project");
            if (v && v->find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789-") != std::string::npos)
                throw BadURL("invalid 'user-project' value '%s' in GCS URL", *v);
            return v;
        }(),
        .generation = getOptionalParam("generation"),
    };
} catch (BadURL & e) {
    e.addTrace({}, "while parsing GCS URI: '%s'", parsed.to_string());
    throw;
}

ParsedURL ParsedGCSURL::toHttpsUrl(std::string_view scheme, const Endpoint & endpoint) const
{
    /* We always use path-style as it is supported for all bucket names including dotted ones and emulators.
     * GCS also supports virtual-hosted-style (`bucket.storage.googleapis.com`).
     */
    struct Resolved
    {
        std::string scheme;
        std::optional<ParsedURL::Authority> authority;
        std::vector<std::string> path;
    };

    auto resolved = std::visit(
        overloaded{
            [&](std::monostate) -> Resolved {
                return {std::string(scheme), ParsedURL::Authority{.host = "storage.googleapis.com"}, {""}};
            },
            [&](const ParsedURL::Authority & auth) -> Resolved { return {std::string(scheme), auth, {""}}; },
            [&](const ParsedURL & url) -> Resolved { return {url.scheme, url.authority, url.path}; },
        },
        endpoint);

    resolved.path.push_back(bucket);
    resolved.path.insert(resolved.path.end(), key.begin(), key.end());

    /* GCS' XML API accepts the object generation as a query parameter on GET/HEAD.
       We allow it to pass it through so callers can pin a specific version. */
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
