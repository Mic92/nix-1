#include "uds-remote-store.hh"
#include "worker-protocol.hh"
#include "compression.hh"

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

#include <cstring>


namespace nix {

std::string UDSRemoteStoreConfig::doc()
{
    return
        #include "uds-remote-store.md"
        ;
}


UDSRemoteStore::UDSRemoteStore(const Params & params)
    : StoreConfig(params)
    , LocalFSStoreConfig(params)
    , RemoteStoreConfig(params)
    , UDSRemoteStoreConfig(params)
    , Store(params)
    , LocalFSStore(params)
    , RemoteStore(params)
{
}


UDSRemoteStore::UDSRemoteStore(
    const std::string scheme,
    std::string socket_path,
    const Params & params)
    : UDSRemoteStore(params)
{
    path.emplace(socket_path);
}


std::string UDSRemoteStore::getUri()
{
    if (path) {
        return std::string("unix://") + *path;
    } else {
        return "daemon";
    }
}


void UDSRemoteStore::Connection::closeWrite()
{
    shutdown(fd.get(), SHUT_WR);
}


ref<RemoteStore::Connection> UDSRemoteStore::openConnection()
{
    auto conn = make_ref<Connection>();

    /* Connect to a daemon that does the privileged work for us. */
    conn->fd = createUnixDomainSocket();

    nix::connect(conn->fd.get(), path ? *path : settings.nixDaemonSocketFile);

    conn->fromFdSource = make_ref<FdSource>(conn->fd.get());
    conn->toFdSink = make_ref<FdSink>(conn->fd.get());

    conn->to = makeCompressionSink("none", *conn->toFdSink).get_ptr();
    conn->from = std::shared_ptr<DecompressionSource>(nix::makeDecompressionSource("none", *conn->fromFdSource));

    conn->startTime = std::chrono::steady_clock::now();

    return conn;
}


void UDSRemoteStore::addIndirectRoot(const Path & path)
{
    auto conn(getConnection());
    *conn->to << WorkerProto::Op::AddIndirectRoot << path;
    conn.processStderr();
    readInt(*conn->from);
}


static RegisterStoreImplementation<UDSRemoteStore, UDSRemoteStoreConfig> regUDSRemoteStore;

}
