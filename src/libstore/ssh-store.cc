#include "ssh-store-config.hh"
#include "compression.hh"
#include "store-api.hh"
#include "local-fs-store.hh"
#include "remote-store.hh"
#include "remote-store-connection.hh"
#include "remote-fs-accessor.hh"
#include "archive.hh"
#include "worker-protocol.hh"
#include "pool.hh"
#include "ssh.hh"

namespace nix {

struct SSHStoreConfig : virtual RemoteStoreConfig, virtual CommonSSHStoreConfig
{
    using RemoteStoreConfig::RemoteStoreConfig;
    using CommonSSHStoreConfig::CommonSSHStoreConfig;

    const Setting<Path> remoteProgram{(StoreConfig*) this, "nix-daemon", "remote-program",
        "Path to the `nix-daemon` executable on the remote machine."};

    const Setting<std::string> compression{(StoreConfig*) this, "none", "compression",
        "Compression algorithm to use for data transfer."};

    const std::string name() override { return "Experimental SSH Store"; }

    std::string doc() override
    {
        return
          #include "ssh-store.md"
          ;
    }
};

class SSHStore : public virtual SSHStoreConfig, public virtual RemoteStore
{
public:

    SSHStore(const std::string & scheme, const std::string & host, const Params & params)
        : StoreConfig(params)
        , RemoteStoreConfig(params)
        , CommonSSHStoreConfig(params)
        , SSHStoreConfig(params)
        , Store(params)
        , RemoteStore(params)
        , host(host)
        , master(
            host,
            sshKey,
            sshPublicHostKey,
            // Use SSH master only if using more than 1 connection.
            connections->capacity() > 1,
            compress)
    {
    }

    static std::set<std::string> uriSchemes() { return {"ssh-ng"}; }

    std::string getUri() override
    {
        return *uriSchemes().begin() + "://" + host;
    }

    // FIXME extend daemon protocol, move implementation to RemoteStore
    std::optional<std::string> getBuildLogExact(const StorePath & path) override
    { unsupported("getBuildLogExact"); }

protected:

    struct Connection : RemoteStore::Connection
    {
        std::unique_ptr<SSHMaster::Connection> sshConn;

        void closeWrite() override
        {
            sshConn->in.close();
        }
    };



    ref<RemoteStore::Connection> openConnection() override;

    std::string host;

    SSHMaster master;

    void setOptions(RemoteStore::Connection & conn) override
    {
        /* TODO Add a way to explicitly ask for some options to be
           forwarded. One option: A way to query the daemon for its
           settings, and then a series of params to SSHStore like
           forward-cores or forward-overridden-cores that only
           override the requested settings.
        */
    };
};

ref<RemoteStore::Connection> SSHStore::openConnection()
{
    std::string command = remoteProgram + " --stdio";
    if (remoteStore.get() != "")
        command += " --store " + shellEscape(remoteStore.get());

    if (compression != "none")
        command += " --compression " + shellEscape(compression.get());

    auto sshConn = master.startCommand(command);
    auto toFdSink = std::make_shared<FdSink>(sshConn->in.get());
    auto to = makeCompressionSink(compression, *toFdSink).get_ptr();
    auto fromFdSource = std::make_shared<FdSource>(sshConn->out.get());
    auto from = std::shared_ptr<DecompressionSource>(nix::makeDecompressionSource(compression, *fromFdSource));

    auto conn = make_ref<Connection>();
    conn->sshConn = std::move(sshConn);
    conn->to = std::move(to);
    conn->from = std::move(from);
    conn->toFdSink = std::move(toFdSink);
    conn->fromFdSource = std::move(fromFdSource);
    return conn;
}

static RegisterStoreImplementation<SSHStore, SSHStoreConfig> regSSHStore;

} // namespace nix
