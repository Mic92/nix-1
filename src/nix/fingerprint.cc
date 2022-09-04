#include "command.hh"
#include "shared.hh"
#include "store-api.hh"
#include "sync.hh"
#include "thread-pool.hh"
#include "references.hh"
#include <cstdio>

using namespace nix;

struct CmdFingerprint : StorePathsCommand
{
    bool noContents = false;
    bool noTrust = false;
    Strings substituterUris;
    size_t sigsNeeded = 0;

    CmdFingerprint()
    {
    }

    std::string description() override
    {
        return "fingerprint the integrity of store paths";
    }

    std::string doc() override
    {
        return std::string("");
    }

    void run(ref<Store> store, StorePaths && storePaths) override
    {
        for (auto & storePath : storePaths) {
            auto storePath2 = store->printStorePath(storePath);
            auto info = store->queryPathInfo(store->parseStorePath(storePath2));
            fprintf(stderr, "%s\n", info->fingerprint(*store).c_str());
        }

        throw Exit(0);
    }
};

static auto rCmdFingerprint = registerCommand2<CmdFingerprint>({"store", "fingerprint"});
