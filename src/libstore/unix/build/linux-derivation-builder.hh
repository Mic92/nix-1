#pragma once
///@file

#ifdef __linux__

#  include "nix/store/build/unix-derivation-builder.hh"
#  include "nix/util/file-descriptor.hh"
#  include "nix/util/processes.hh"
#  include <memory>
#  include <optional>
#  include <map>

namespace nix {

class UserLock;
class AutoDelete;

struct LinuxDerivationBuilder : DerivationBuilderImpl
{
    using DerivationBuilderImpl::DerivationBuilderImpl;

    void enterChroot() override;
};

struct ChrootLinuxDerivationBuilder : LinuxDerivationBuilder
{
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
     * The root of the chroot environment.
     */
    Path chrootRootDir;

    /**
     * RAII object to delete the chroot directory.
     */
    std::shared_ptr<AutoDelete> autoDelChroot;

    PathsInChroot pathsInChroot;

    /**
     * The cgroup of the builder, if any.
     */
    std::optional<Path> cgroup;

    using LinuxDerivationBuilder::LinuxDerivationBuilder;

    void deleteTmpDir(bool force) override;
    uid_t sandboxUid();
    gid_t sandboxGid();
    bool needsHashRewrite() override;
    std::unique_ptr<UserLock> getBuildUser() override;
    void setBuildTmpDir() override;
    Path tmpDirInSandbox() override;
    void prepareUser() override;
    void prepareSandbox() override;
    Strings getPreBuildHookArgs() override;
    Path realPathInSandbox(const Path & p) override;
    void startChild() override;
    void enterChroot() override;
    void setUser() override;
    std::variant<std::pair<BuildResult::Status, Error>, SingleDrvOutputs> unprepareBuild() override;
    void killSandbox(bool getStats) override;
    void cleanupBuild() override;
    void addDependency(const StorePath & path) override;
};

}

#endif