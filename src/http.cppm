module;
#include <string>
#include <variant>
#include <vector>
#include <optional>

export module ncrequest:http;
export import ncrequest.type;

namespace ncrequest
{

export class URI {
public:
    URI();
    URI(std::string_view);
    ~URI();
    URI(const URI&);
    URI& operator=(const URI&);

    static URI from(std::string_view);

    std::string_view uri;
    std::string_view scheme;
    std::string_view authority;
    std::string_view userinfo;
    std::string_view host;
    std::string_view port;
    std::string_view path;
    std::string_view query;
    std::string_view fragment;

    bool valid() const;

private:
    bool        m_valid;
    std::string m_holder;
};
export struct HttpHeader;
}

export template<>
struct rstd::Impl<rstd::clone::Clone, ncrequest::HttpHeader>
    : rstd::DefImpl<rstd::clone::Clone, ncrequest::HttpHeader> {
    static auto clone(TraitPtr) -> ncrequest::HttpHeader;
};

namespace ncrequest {

struct HttpHeader : public rstd::WithTrait<HttpHeader, rstd::clone::Clone> {
    struct Request {
        std::string method;
        std::string version;
        std::string target;
    };
    struct Status {
        std::string version;
        i32         code;
    };
    struct Field {
        std::string name;
        std::string value;
    };
    using Start = std::variant<Request, Status>;

    rstd::Option<Start> start;
    std::vector<Field>  fields;

    auto has_field(std::string_view) const -> bool;

    static auto parse_header(std::string_view) -> rstd::Option<HttpHeader>;
    static auto parse_start_line(std::string_view) -> rstd::Option<Start>;
    static auto parse_field_line(std::string_view) -> Field;
};

} // namespace ncrequest