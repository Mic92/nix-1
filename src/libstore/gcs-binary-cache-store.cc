#include "nix/store/gcs-binary-cache-store.hh"
#include "nix/store/gcp-creds.hh"
#include "nix/store/gcs-url.hh"
#include "nix/store/store-registration.hh"

#include <cassert>

namespace nix {

class GCSBinaryCacheStore : public virtual S3CompatBinaryCacheStore
{

public:
    GCSBinaryCacheStore(ref<GCSBinaryCacheStoreConfig> config)
        : Store{*config}
        , BinaryCacheStore{*config}
        , HttpBinaryCacheStore{config}
        , S3CompatBinaryCacheStore{config}
        , gcsConfig{config}
    {
    }

protected:
    std::string_view backendName() const override
    {
        return "GCS";
    }

    /**
     * Produce an `http(s)://<endpoint>/<bucket>/<path>` request with the
     * bearer token attached. This is where the store's configured endpoint is
     * applied; a `gs://` URL cannot carry one (see `ParsedGCSURL`).
     */
    FileTransferRequest makeRequest(std::string_view path) override
    {
        auto req = HttpBinaryCacheStore::makeRequest(path);
        req.uri =
            ParsedGCSURL::parse(req.uri.parsed()).toHttpsUrl(gcsConfig->scheme.get(), gcsConfig->resolvedEndpoint);

        if (auto up = gcsConfig->userProject.get(); !up.empty())
            req.headers.emplace_back("x-goog-user-project", up);

#if NIX_WITH_GCS_AUTH
        if (auto creds = getGcpCredentialsProvider()->maybeGetCredentials())
            req.headers.emplace_back("Authorization", "Bearer " + creds->accessToken);
#endif
        return req;
    }

    void prepareRequest(FileTransferRequest &) const override
    {
        /* makeRequest() already rewrote the URL and attached credentials;
           nothing left for the multipart path to do. */
    }

    void addUploadHeaders(Headers & headers) const override
    {
        if (auto storageClass = gcsConfig->storageClass.get())
            headers.emplace_back("x-goog-storage-class", *storageClass);
    }

private:
    ref<GCSBinaryCacheStoreConfig> gcsConfig;
};

StringSet GCSBinaryCacheStoreConfig::uriSchemes()
{
    return {"gs"};
}

GCSBinaryCacheStoreConfig::GCSBinaryCacheStoreConfig(ParsedURL cacheUri_, const Params & params)
    : StoreConfig(params)
    , S3CompatBinaryCacheStoreConfig(std::move(cacheUri_), params)
{
    assert(cacheUri.query.empty());
    assert(cacheUri.scheme == "gs");

    if (auto ep = endpoint.get(); !ep.empty())
        resolvedEndpoint = std::visit([](auto v) -> ParsedGCSURL::Endpoint { return v; }, parseUrlOrAuthority(ep));

    validateMultipartSettings();
}

GCSBinaryCacheStoreConfig::GCSBinaryCacheStoreConfig(std::string_view bucketName, const Params & params)
    : GCSBinaryCacheStoreConfig(
          ParsedURL{.scheme = "gs", .authority = ParsedURL::Authority{.host = std::string(bucketName)}}, params)
{
}

std::string GCSBinaryCacheStoreConfig::getHumanReadableURI() const
{
    return renderHumanReadableUri({&scheme, &endpoint, &userProject});
}

std::string GCSBinaryCacheStoreConfig::doc()
{
    return
#include "gcs-binary-cache-store.md"
        ;
}

ref<Store> GCSBinaryCacheStoreConfig::openStore() const
{
    auto sharedThis = std::const_pointer_cast<GCSBinaryCacheStoreConfig>(
        std::static_pointer_cast<const GCSBinaryCacheStoreConfig>(shared_from_this()));
    return make_ref<GCSBinaryCacheStore>(ref{sharedThis});
}

static RegisterStoreImplementation<GCSBinaryCacheStoreConfig> registerGCSBinaryCacheStore;

} // namespace nix
