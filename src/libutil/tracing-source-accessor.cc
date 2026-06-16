#include "nix/util/tracing-source-accessor.hh"
#include "nix/util/logging.hh"

namespace nix {

namespace {

class TracingSourceAccessor : public SourceAccessor
{
    ref<SourceAccessor> next;
    ref<FileAccessTrace> trace;

    using Bucket = std::set<std::string> FileAccessTrace::State::*;

    void record(Bucket bucket, const CanonPath & path)
    {
        if (auto phys = next->getPhysicalPath(path))
            ((*trace->state.lock()).*bucket).insert(phys->string());
        else
            debug("file-access trace: skipping virtual path '%s'", next->showPath(path));
    }

public:
    TracingSourceAccessor(ref<SourceAccessor> next_, ref<FileAccessTrace> trace_)
        : next(std::move(next_))
        , trace(std::move(trace_))
    {
        displayPrefix.clear();
    }

    void readFile(const CanonPath & path, Sink & sink, fun<void(uint64_t)> sizeCallback) override
    {
        record(&FileAccessTrace::State::files, path);
        next->readFile(path, sink, sizeCallback);
    }

    /* `pathExists`/`lstat` not overridden: base impls route through
       `maybeLstat`. Hit and miss both land in `files` — a watcher on a
       non-existent path fires on create, so the distinction is not
       actionable for the consumer. */

    std::optional<Stat> maybeLstat(const CanonPath & path) override
    {
        record(&FileAccessTrace::State::files, path);
        return next->maybeLstat(path);
    }

    DirEntries readDirectory(const CanonPath & path) override
    {
        record(&FileAccessTrace::State::dirs, path);
        return next->readDirectory(path);
    }

    std::string readLink(const CanonPath & path) override
    {
        record(&FileAccessTrace::State::files, path);
        return next->readLink(path);
    }

    void dumpPath(const CanonPath & path, Sink & sink, PathFilter & filter) override
    {
        /* Record the root as a tree-copy hint. The base-class
           recursion below also emits per-file `files`/`dirs` entries,
           so `trees` is a watch-count optimisation, not required for
           soundness. */
        record(&FileAccessTrace::State::trees, path);
        SourceAccessor::dumpPath(path, sink, filter);
    }

    std::optional<std::filesystem::path> getPhysicalPath(const CanonPath & path) override
    {
        return next->getPhysicalPath(path);
    }

    std::string showPath(const CanonPath & path) override
    {
        return next->showPath(path);
    }

    std::pair<CanonPath, std::optional<std::string>> getFingerprint(const CanonPath & path) override
    {
        return next->getFingerprint(path);
    }

    std::optional<time_t> getLastModified() override
    {
        return next->getLastModified();
    }

    void invalidateCache(const CanonPath & path) override
    {
        next->invalidateCache(path);
    }
};

} // namespace

ref<SourceAccessor> makeTracingSourceAccessor(ref<SourceAccessor> next, ref<FileAccessTrace> trace)
{
    return make_ref<TracingSourceAccessor>(std::move(next), std::move(trace));
}

} // namespace nix
