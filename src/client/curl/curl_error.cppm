export module ncrequest.curl:error;
export import :curl;
export import rstd.core;

using namespace curl;

namespace std
{
export template<>
struct is_error_code_enum<CURLMcode> : rstd::meta::true_type {};

export template<>
struct is_error_code_enum<CURLcode> : rstd::meta::true_type {};
} // namespace std

namespace ncrequest
{
class CURLMEcategory : public rstd::cppstd::error_category {
public:
    static CURLMEcategory const& instance() {
        static CURLMEcategory instance;
        return instance;
    }

    char const* name() const noexcept override { return "CURLMEcategory"; }

    rstd::cppstd::string message(int code) const override {
        return curl_multi_strerror((CURLMcode)code);
    }
};

const CURLMEcategory theCURLMEcategory {};

class CURLEcategory : public rstd::cppstd::error_category {
public:
    static CURLEcategory const& instance() {
        static CURLEcategory instance;
        return instance;
    }

    char const* name() const noexcept override { return "CURLEcategory"; }

    rstd::cppstd::string message(int code) const override {
        return curl_easy_strerror((CURLcode)code);
    }
};

const CURLEcategory theCURLEcategory {};
} // namespace ncrequest

export inline rstd::cppstd::error_code make_error_code(CURLMcode code) {
    return {
        static_cast<int>(code),
        ncrequest::theCURLMEcategory,
    };
}
export inline rstd::cppstd::error_code make_error_code(CURLcode code) {
    return {
        static_cast<int>(code),
        ncrequest::theCURLEcategory,
    };
}
