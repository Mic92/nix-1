#pragma once
///@file

#include "nix/store/s3-compat-binary-cache-store.hh"

namespace nix {

struct GCSBinaryCacheStoreConfig : S3CompatBinaryCacheStoreConfig
{
    GCSBinaryCacheStoreConfig(const Params & params)
        : StoreConfig(params, FilePathType::Unix)
        , S3CompatBinaryCacheStoreConfig(params)
    {
    }

    GCSBinaryCacheStoreConfig(ParsedURL cacheUri, const Params & params);

    GCSBinaryCacheStoreConfig(std::string_view bucketName, const Params & params);

    Setting<std::string> scheme{
        this,
        "https",
        "scheme",
        R"(
          The scheme used for GCS requests, `https` (default) or `http`. This
          option allows you to disable HTTPS for emulators that don't support
          it.

          > **Note**
          >
          > HTTPS should be used if the cache might contain sensitive
          > information.
        )"};

    Setting<std::string> endpoint{
        this,
        "",
        "endpoint",
        R"(
          The GCS endpoint to use. When empty (default), uses
          `storage.googleapis.com`. For GCS-compatible emulators, set this to
          your service's endpoint.
        )"};

    Setting<std::string> userProject{
        this,
        "",
        "user-project",
        R"(
          The Google Cloud project to bill for access to a requester-pays
          bucket. Sent as the `x-goog-user-project` header.
        )"};

    Setting<std::optional<std::string>> storageClass{
        this,
        std::nullopt,
        "storage-class",
        R"(
          The GCS storage class to use for uploaded objects. When not set
          (default), uses the bucket's default storage class. Valid values
          include `STANDARD`, `NEARLINE`, `COLDLINE`, and `ARCHIVE`.

          See the GCS documentation for details:
          https://cloud.google.com/storage/docs/storage-classes
        )"};

    /**
     * Settings that must be carried in the `gs://` URL query so that
     * `FileTransferRequest::setupForGCS()` can recover them per request.
     */
    const std::set<const AbstractSetting *> gcsUriSettings = {&scheme, &endpoint, &userProject};

    static const std::string name()
    {
        return "GCS Binary Cache Store";
    }

    static StringSet uriSchemes();

    static std::string doc();

    std::string getHumanReadableURI() const override;

    ref<Store> openStore() const override;
};

} // namespace nix
