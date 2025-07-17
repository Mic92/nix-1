#pragma once
///@file

#ifdef __APPLE__

#  include "nix/store/build/unix-derivation-builder.hh"

namespace nix {

struct DarwinDerivationBuilder : DerivationBuilderImpl
{
    PathsInChroot pathsInChroot;

    /**
     * Whether full sandboxing is enabled. Note that macOS builds
     * always have *some* sandboxing (see sandbox-minimal.sb).
     */
    bool useSandbox;

    DarwinDerivationBuilder(
        Store & store,
        std::unique_ptr<DerivationBuilderCallbacks> miscMethods,
        DerivationBuilderParams params,
        bool useSandbox);

    void prepareSandbox() override;
    void setUser() override;
    void execBuilder(const Strings & args, const Strings & envStrs) override;
};

}

#endif