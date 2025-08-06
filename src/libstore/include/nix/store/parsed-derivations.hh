#pragma once
///@file

#include "nix/util/types.hh"
#include "nix/util/error.hh"
#include "nix/store/path.hh"
#include "nix/util/json-utils.hh"

#include <nlohmann/json.hpp>

namespace nix {

class Store;
struct DerivationOptions;
struct DerivationOutput;

typedef std::map<std::string, DerivationOutput> DerivationOutputs;

struct StructuredAttrs
{
    static constexpr std::string_view envVarName{"__json"};

private:
    mutable std::optional<nlohmann::json> parsedJson;

public:
    std::string rawJson;
    // For building up JSON during derivation construction
    nlohmann::json structuredAttrs;

    bool operator==(const StructuredAttrs &) const = default;

    const nlohmann::json & getStructuredAttrs() const
    {
        if (!rawJson.empty() && !parsedJson) {
            try {
                parsedJson = nlohmann::json::parse(rawJson);
            } catch (std::exception & e) {
                throw Error("cannot process %s attribute: %s", envVarName, e.what());
            }
            return *parsedJson;
        } else if (parsedJson) {
            return *parsedJson;
        }
        return structuredAttrs;
    }

    /**
     * Unconditionally parse from a JSON string. Used by `tryExtract`.
     */
    static StructuredAttrs parse(std::string && encoded);

    /**
     * Like `tryParse`, but removes the env var which encoded the structured
     * attrs from the map if one is found.
     */
    static std::optional<StructuredAttrs> tryExtract(StringPairs & env);

    /**
     * Opposite of `tryParse`, at least if one makes a map from this
     * single key-value PR.
     */
    std::pair<std::string_view, std::string> unparse() const;

    /**
     * Ensures that the structured attrs "env var" is not in used, so we
     * are free to use it instead.
     */
    static void checkKeyNotInUse(const StringPairs & env);

    nlohmann::json prepareStructuredAttrs(
        Store & store,
        const DerivationOptions & drvOptions,
        const StorePathSet & inputPaths,
        const DerivationOutputs & outputs) const;

    /**
     * As a convenience to bash scripts, write a shell file that
     * maps all attributes that are representable in bash -
     * namely, strings, integers, nulls, Booleans, and arrays and
     * objects consisting entirely of those values. (So nested
     * arrays or objects are not supported.)
     *
     * @param prepared This should be the result of
     * `prepareStructuredAttrs`, *not* the original `structuredAttrs`
     * field.
     */
    static std::string writeShell(const nlohmann::json & prepared);
};

} // namespace nix
