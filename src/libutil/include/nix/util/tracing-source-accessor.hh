#pragma once
///@file

#include "nix/util/source-accessor.hh"
#include "nix/util/sync.hh"

#include <map>
#include <set>

namespace nix {

/**
 * Record of every filesystem path observed through a
 * `TracingSourceAccessor`, reduced to the three invalidation classes
 * a build-system file watcher can act on, plus `builtins.getEnv`
 * reads. Dumped via `$NIX_DUMP_FILE_ACCESS` so external tooling can
 * derive a sound watch/invalidation set from what an evaluation
 * actually read instead of a hand-maintained list.
 *
 * Paths are physical (`getPhysicalPath()`); accesses with no physical
 * path (flake/fetcher accessors) are dropped with a `debug()` line.
 * `env` is filled by the `builtins.getEnv` primop, not the accessor.
 */
struct FileAccessTrace
{
    struct State
    {
        /// `readFile`, `readLink`, `maybeLstat` (hit *and* miss): paths
        /// whose existence, type or content was observed.
        std::set<std::string> files;
        /// `readDirectory`: dirs whose listing was observed.
        std::set<std::string> dirs;
        /// `dumpPath` roots: optional recursive-watch hint instead of the
        /// per-file `files`/`dirs` entries the dump also emits. Includes
        /// `filterSource` roots, so this over-invalidates filtered trees.
        std::set<std::string> trees;
        /// `builtins.getEnv` name → observed value (`""` if unset).
        std::map<std::string, std::string> env;
    };

    Sync<State> state;
};

/**
 * Wrap `next` so every access is recorded into `trace` before being
 * forwarded. Intended to be the outermost layer of `EvalState::rootFS`.
 */
ref<SourceAccessor> makeTracingSourceAccessor(ref<SourceAccessor> next, ref<FileAccessTrace> trace);

} // namespace nix
