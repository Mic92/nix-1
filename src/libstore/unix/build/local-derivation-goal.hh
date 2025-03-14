#pragma once
///@file

#include "derivation-goal.hh"
#include "local-store.hh"
#include "processes.hh"

namespace nix {

struct LocalDerivationGoal : public DerivationGoal
{
    LocalStore & getLocalStore();

    /**
     * User selected for running the builder.
     */
    std::unique_ptr<UserLock> buildUser;

    /**
     * The process ID of the builder.
     */
    Pid pid;

    /**
     * The cgroup of the builder, if any.
     */
    std::optional<Path> cgroup;

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
     * The path of the temporary directory in the sandbox.
     */
    Path tmpDirInSandbox;

    /**
     * Master side of the pseudoterminal used for the builder's
     * standard output/error.
     */
    AutoCloseFD builderOut;

    /**
     * Pipe for synchronising updates to the builder namespaces.
     */
    Pipe userNamespaceSync;

    /**
     * The mount namespace and user namespace of the builder, used to add additional
     * paths to the sandbox as a result of recursive Nix calls.
     */
    AutoCloseFD sandboxMountNamespace;
    AutoCloseFD sandboxUserNamespace;

    /**
     * On Linux, whether we're doing the build in its own user
     * namespace.
     */
    bool usingUserNamespace = true;

    /**
     * Whether we're currently doing a chroot build.
     */
    bool useChroot = false;

    /**
     * The root of the chroot environment.
     */
    Path chrootRootDir;

    /**
     * RAII object to delete the chroot directory.
     */
    std::shared_ptr<AutoDelete> autoDelChroot;

    /**
     * Stuff we need to pass to initChild().
     */
    struct ChrootPath {
        Path source;
        bool optional;
        ChrootPath(Path source = "", bool optional = false)
            : source(source), optional(optional)
        { }
    };
    typedef map<Path, ChrootPath> PathsInChroot; // maps target path to source path
    PathsInChroot pathsInChroot;

    typedef map<std::string, std::string> Environment;
    Environment env;

    /**
     * Hash rewriting.
     */
    StringMap inputRewrites, outputRewrites;
    typedef map<StorePath, StorePath> RedirectedOutputs;
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

    uid_t sandboxUid() { return usingUserNamespace ? (!buildUser || buildUser->getUIDCount() == 1 ? 1000 : 0) : buildUser->getUID(); }
    gid_t sandboxGid() { return usingUserNamespace ? (!buildUser || buildUser->getUIDCount() == 1 ? 100  : 0) : buildUser->getGID(); }

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

    /**
     * Paths that were added via recursive Nix calls.
     */
    StorePathSet addedPaths;

    /**
     * Realisations that were added via recursive Nix calls.
     */
    std::set<DrvOutput> addedDrvOutputs;

    /**
     * Recursive Nix calls are only allowed to build or realize paths
     * in the original input closure or added via a recursive Nix call
     * (so e.g. you can't do 'nix-store -r /nix/store/<bla>' where
     * /nix/store/<bla> is some arbitrary path in a binary cache).
     */
    bool isAllowed(const StorePath & path)
    {
        return inputPaths.count(path) || addedPaths.count(path);
    }
    bool isAllowed(const DrvOutput & id)
    {
        return addedDrvOutputs.count(id);
    }

    bool isAllowed(const DerivedPath & req);

    friend struct RestrictedStore;

    using DerivationGoal::DerivationGoal;

    virtual ~LocalDerivationGoal() override;

    /**
     * Whether we need to perform hash rewriting if there are valid output paths.
     */
    bool needsHashRewrite();

    /**
     * The additional states.
     */
    Goal::Co tryLocalBuild() override;

    /**
     * Start building a derivation.
     */
    void startBuilder();

    /**
     * Fill in the environment for the builder.
     */
    void initEnv();

    /**
     * Process messages send by the sandbox initialization.
     */
    void processSandboxSetupMessages();

    /**
     * Setup tmp dir location.
     */
    void initTmpDir();

    /**
     * Write a JSON file containing the derivation attributes.
     */
    void writeStructuredAttrs();

    /**
     * Start an in-process nix daemon thread for recursive-nix.
     */
    void startDaemon();

    /**
     * Stop the in-process nix daemon thread.
     * @see startDaemon
     */
    void stopDaemon();

    /**
     * Add 'path' to the set of paths that may be referenced by the
     * outputs, and make it appear in the sandbox.
     */
    void addDependency(const StorePath & path);

    /**
     * Make a file owned by the builder.
     */
    void chownToBuilder(const Path & path);

    /**
     * Run the builder's process.
     */
    void runChild();

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

    bool isReadDesc(int fd) override;

    /**
     * Delete the temporary directory, if we have one.
     */
    void deleteTmpDir(bool force);

    /**
     * Forcibly kill the child process, if any.
     *
     * Called by destructor, can't be overridden
     */
    void killChild() override final;

    /**
     * Kill any processes running under the build user UID or in the
     * cgroup of the build.
     */
    void killSandbox(bool getStats);

    bool cleanupDecideWhetherDiskFull();

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
};

}
