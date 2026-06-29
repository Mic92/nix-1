#include "nix/store/gcs-binary-cache-store.hh"
#include "nix/store/store-registration.hh"

#include <cassert>

namespace nix {

class GCSBinaryCacheStore : public virtual S3CompatBinaryCacheStore
{
    void anchor() override;

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

    void prepareRequest(FileTransferRequest & req) const override
    {
        req.setupForGCS();
    }

    void addUploadHeaders(Headers & headers) const override
    {
        if (auto storageClass = gcsConfig->storageClass.get())
            headers.emplace_back("x-goog-storage-class", *storageClass);
    }

private:
    ref<GCSBinaryCacheStoreConfig> gcsConfig;
};

void GCSBinaryCacheStore::anchor() {}

StringSet GCSBinaryCacheStoreConfig::uriSchemes()
{
    return {"gs"};
}

GCSBinaryCacheStoreConfig::GCSBinaryCacheStoreConfig(ParsedURL cacheUri_, const Params & params)
    : StoreConfig(params, FilePathType::Unix)
    , S3CompatBinaryCacheStoreConfig(std::move(cacheUri_), params)
{
    assert(cacheUri.query.empty());
    assert(cacheUri.scheme == "gs");

    copyUriParams(params, gcsUriSettings);
    validateMultipartSettings();
}

GCSBinaryCacheStoreConfig::GCSBinaryCacheStoreConfig(std::string_view bucketName, const Params & params)
    : GCSBinaryCacheStoreConfig(
          ParsedURL{.scheme = "gs", .authority = ParsedURL::Authority{.host = std::string(bucketName)}}, params)
{
}

std::string GCSBinaryCacheStoreConfig::getHumanReadableURI() const
{
    return renderHumanReadableUri(gcsUriSettings);
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
