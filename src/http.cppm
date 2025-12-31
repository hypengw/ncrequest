export module ncrequest:http;
export import ncrequest.type;

namespace ncrequest
{

export class URI {
public:
    URI();
    URI(rstd::cppstd::string_view);
    ~URI();
    URI(const URI&);
    URI& operator=(const URI&);

    static URI from(rstd::cppstd::string_view);

    rstd::cppstd::string_view uri;
    rstd::cppstd::string_view scheme;
    rstd::cppstd::string_view authority;
    rstd::cppstd::string_view userinfo;
    rstd::cppstd::string_view host;
    rstd::cppstd::string_view port;
    rstd::cppstd::string_view path;
    rstd::cppstd::string_view query;
    rstd::cppstd::string_view fragment;

    bool valid() const;

private:
    bool                 m_valid;
    rstd::cppstd::string m_holder;
};

export struct HttpHeader {
    struct Request {
        rstd::cppstd::string method;
        rstd::cppstd::string version;
        rstd::cppstd::string target;
    };
    struct Status {
        rstd::cppstd::string version;
        i32                  code;
    };
    struct Field {
        rstd::cppstd::string name;
        rstd::cppstd::string value;
    };
    using Start = rstd::cppstd::variant<Request, Status>;

    rstd::Option<Start>         start;
    rstd::cppstd::vector<Field> fields;

    auto has_field(rstd::cppstd::string_view) const -> bool;

    static auto parse_header(rstd::cppstd::string_view) -> rstd::Option<HttpHeader>;
    static auto parse_start_line(rstd::cppstd::string_view) -> rstd::Option<Start>;
    static auto parse_field_line(rstd::cppstd::string_view) -> Field;

    // trait
    auto clone() const -> ncrequest::HttpHeader;
    void clone_from(ncrequest::HttpHeader&);
};

} // namespace ncrequest

export template<>
struct rstd::Impl<rstd::clone::Clone, ncrequest::HttpHeader>
    : rstd::ImplInClass<rstd::clone::Clone, ncrequest::HttpHeader> {};