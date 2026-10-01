export module ncrequest:tls;
export import :error;

using namespace rstd::prelude;
using rstd::path::PathBuf;

namespace ncrequest
{

export struct TlsClientIdentity {
    PathBuf certificate;
    PathBuf private_key;

    auto clone() const -> TlsClientIdentity { return { certificate.clone(), private_key.clone() }; }
};

export struct TlsOptions {
    bool                      verify_peer { true };
    bool                      verify_hostname { true };
    Option<PathBuf>           ca_bundle;
    Option<TlsClientIdentity> client_identity;

    auto clone() const -> TlsOptions {
        auto out            = TlsOptions {};
        out.verify_peer     = verify_peer;
        out.verify_hostname = verify_hostname;
        if (ca_bundle.is_some()) out.ca_bundle = Some(ca_bundle->clone());
        if (client_identity.is_some()) out.client_identity = Some(client_identity->clone());
        return out;
    }

    auto validate() const -> Result<empty> {
        auto valid_path = [](const PathBuf& path) {
            if (path.is_empty()) return false;
            for (auto byte : path.as_path().as_os_str().as_encoded_bytes())
                if (byte == u8()) return false;
            return true;
        };
        if (ca_bundle.is_some() && ! valid_path(*ca_bundle))
            return Err(Error::InvalidState("TLS CA bundle path is empty or contains NUL"));
        if (client_identity.is_some() && (! valid_path(client_identity->certificate) ||
                                          ! valid_path(client_identity->private_key)))
            return Err(Error::InvalidState(
                "TLS client identity requires certificate and private key paths without NUL"));
        return Ok(empty {});
    }
};

} // namespace ncrequest
