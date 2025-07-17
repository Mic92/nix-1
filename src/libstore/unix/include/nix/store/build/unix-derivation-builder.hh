#pragma once
///@file

#include "nix/store/build/derivation-builder.hh"
#include "nix/store/build/derivation-building-misc.hh"
#include "nix/store/user-lock.hh"
#include "nix/util/file-descriptor.hh"
#include <thread>
#include <memory>
#include <map>

namespace nix {

// Forward declarations
class AutoDelete;

/**
 * Paths that are mounted into the chroot
 */
struct ChrootPath
{
    Path source;
    bool optional;
    ChrootPath(Path source = "", bool optional = false)
        : source(source)
        , optional(optional)
    {
    }
};
typedef std::map<Path, ChrootPath> PathsInChroot;

/**
 * This class represents the state for building locally on Unix systems.
 *
 * @todo Ideally, it would not be a class, but a single function.
 * However, besides the main entry point, there are a few more methods
 * which are externally called, and need to be gotten rid of. There are
 * also some virtual methods (either directly here or inherited from
 * `DerivationBuilderCallbacks`, a stop-gap) that represent outgoing
 * rather than incoming call edges that either should be removed, or
 * become (higher order) function parameters.
 */
class DerivationBuilderImpl : public DerivationBuilder, public DerivationBuilderParams
{
protected:

    Store & store;

    std::unique_ptr<DerivationBuilderCallbacks> miscMethods;

public:

    DerivationBuilderImpl(
        Store & store, std::unique_ptr<DerivationBuilderCallbacks> miscMethods, DerivationBuilderParams params);

protected:

    /**
     * User selected for running the builder.
     */
    std::unique_ptr<UserLock> buildUser;

    /**
     * The temporary directory used for the build.
     */
    Path tmpDir;

    /**
     * The top-level temporary directory. `tmpDir` is either equal to
     * or a child of this directory.
     */
    Path topTmpDir;

    /**
     * The file descriptor of the temporary directory.
     */
    AutoCloseFD tmpDirFd;

    /**
     * The sort of derivation we are building.
     *
     * Just a cached value, computed from `drv`.
     */
    const DerivationType derivationType;

    typedef StringMap Environment;
    Environment env;

    /**
     * Hash rewriting.
     */
    StringMap inputRewrites, outputRewrites;
    typedef std::map<StorePath, StorePath> RedirectedOutputs;
    RedirectedOutputs redirectedOutputs;

    /**
     * The output paths used during the build.
     *
     * - Input-addressed derivations or fixed content-addressed outputs are
     *   sometimes built when some of their outputs already exist, and can not
     *   be hidden via sandboxing. We use temporary locations instead and
     *   rewrite after the build. Otherwise the regular predetermined paths are
     *   put here.
     *
     * - Floating content-addressing derivations do not know their final build
     *   output paths until the outputs are hashed, so random locations are
     *   used, and then renamed. The randomness helps guard against hidden
     *   self-references.
     */
    OutputPathMap scratchOutputs;

    const static Path homeDir;

    /**
     * The recursive Nix daemon socket.
     */
    AutoCloseFD daemonSocket;

    /**
     * The daemon main thread.
     */
    std::thread daemonThread;

    /**
     * The daemon worker threads.
     */
    std::vector<std::thread> daemonWorkerThreads;

    const StorePathSet & originalPaths() override;
    bool isAllowed(const StorePath & path) override;
    bool isAllowed(const DrvOutput & id) override;
    bool isAllowed(const DerivedPath & req);

    friend struct RestrictedStore;

    /**
     * Whether we need to perform hash rewriting if there are valid output paths.
     */
    virtual bool needsHashRewrite();

public:

    bool prepareBuild() override;
    void startBuilder() override;
    std::variant<std::pair<BuildResult::Status, Error>, SingleDrvOutputs> unprepareBuild() override;
    void stopDaemon() override;
    void deleteTmpDir(bool force) override;
    void killSandbox(bool getStats) override;

protected:

    /**
     * Acquire a build user lock. Return nullptr if no lock is available.
     */
    virtual std::unique_ptr<UserLock> getBuildUser();

    /**
     * Return the paths that should be made available in the sandbox.
     * This includes:
     *
     * * The paths specified by the `sandbox-paths` setting, and their closure in the Nix store.
     * * The contents of the `__impureHostDeps` derivation attribute, if the sandbox is in relaxed mode.
     * * The paths returned by the `pre-build-hook`.
     * * The paths in the input closure of the derivation.
     */
    PathsInChroot getPathsInSandbox();

    virtual void setBuildTmpDir();

    /**
     * Return the path of the temporary directory in the sandbox.
     */
    virtual Path tmpDirInSandbox();

    /**
     * Ensure that there are no processes running that conflict with
     * `buildUser`.
     */
    virtual void prepareUser();

    /**
     * Called by prepareBuild() to do any setup in the parent to
     * prepare for a sandboxed build.
     */
    virtual void prepareSandbox();

    virtual Strings getPreBuildHookArgs();

    virtual Path realPathInSandbox(const Path & p);

    /**
     * Open the slave side of the pseudoterminal and use it as stderr.
     */
    void openSlave();

    /**
     * Called by prepareBuild() to start the child process for the
     * build. Must set `pid`. The child must call runChild().
     */
    virtual void startChild();

    /**
     * Actually configure the sandbox. Meant to be called in the
     * child process by the platform-specific runChild() before
     * actually executing the builder.
     */
    virtual void setUser();

    /**
     * Fork a child process, make it do things, and wait for the child to finish.
     */
    virtual void execBuilder(const Strings & args, const Strings & envStrs);

    virtual void enterChroot();

    /**
     * Initialise the sandbox, if any.
     */
    void runChild();

    /**
     * Add a dependency to the build. This does not (yet) affect the
     * RestrictionContext's allowed *paths* in any way. Rather, the point is to
     * be able to tell the user what dependencies were actually used.
     *
     * However, it does add to the allowed *drv outputs* so that
     * `addDependency(path to output foo)` enables subsequent
     * `requestDrvOutput(drv output foo)`.
     */
    virtual void addDependency(const StorePath & path) override;

    /**
     * Start the daemonThread. This is driven by a separate step
     * because we need to complete its socket handshake before
     * installing a cgroup or root directory that might interfere
     * with its ability to load dynamic libraries.
     */
    void startDaemon();

    void stopDaemonWorkerThread();

    void stopDaemonThread();

    /**
     * Run the builder, using a pseudoterminal to receive its output
     * and stderr. */
    void runBuilderRunRemoteWorker();

    /**
     * Run the builder, hijacking the process's stdout/stdin to
     * be how the parent talks to us. */
    void runBuilderInPTY();

    /**
     * Update the builder's environment to match our settings.
     */
    void updateEnvironment();

    /**
     * Clean up the build environment after the build.
     */
    virtual void cleanupBuild();

    /**
     * Fill in the environment for the builder.
     */
    void initEnv();

    /**
     * Process messages send by the sandbox initialization.
     */
    void processSandboxSetupMessages();

    /**
     * Write a JSON file containing the derivation attributes.
     */
    void writeStructuredAttrs();

    /**
     * Make a file owned by the builder.
     *
     * SAFETY: this function is prone to TOCTOU as it receives a path and not a descriptor.
     * It's only safe to call in a child of a directory only visible to the owner.
     */
    void chownToBuilder(const Path & path);

    /**
     * Make a file owned by the builder addressed by its file descriptor.
     */
    void chownToBuilder(int fd, const Path & path);

    /**
     * Create a file in `tmpDir` owned by the builder.
     */
    void writeBuilderFile(const std::string & name, std::string_view contents);

    /**
     * Check that the derivation outputs all exist and register them
     * as valid.
     */
    SingleDrvOutputs registerOutputs();

    /**
     * Check that an output meets the requirements specified by the
     * 'outputChecks' attribute (or the legacy
     * '{allowed,disallowed}{References,Requisites}' attributes).
     */
    void checkOutputs(const std::map<std::string, ValidPathInfo> & outputs);

    bool decideWhetherDiskFull();

    /**
     * Create alternative path calculated from but distinct from the
     * input, so we can avoid overwriting outputs (or other store paths)
     * that already exist.
     */
    StorePath makeFallbackPath(const StorePath & path);

    /**
     * Make a path to another based on the output name along with the
     * derivation hash.
     *
     * @todo Add option to randomize, so we can audit whether our
     * rewrites caught everything
     */
    StorePath makeFallbackPath(OutputNameView outputName);

    /**
     * Create a new build log file for this builder and return a write-only file
     * descriptor to it.
     */
    Path openLogFile();

    /**
     * Wrappers around the corresponding miscMethods.
     */
    void closeLogFile();

    void noteHashMismatch();

    void noteCheckMismatch();

    void markContentsGood(const StorePath & path);

    /**
     * Send the current exception to the parent in the format expected by
     * `processSandboxSetupMessages()`.
     */
    static void handleChildException(bool sendException);

protected:
    StorePathSet addedPaths;
    DrvOutputs addedDrvOutputs;

    /**
     * RAII object to delete the chroot directory.
     */
    std::shared_ptr<AutoDelete> autoDelChroot;
};

}
