export module ncrequest:http;
export import ncrequest.type;
export import cppstd;

namespace ncrequest
{

export class URI {
public:
    URI();
    URI(cppstd::string_view);
    ~URI();
    URI(const URI&);
    URI& operator=(const URI&);

    static URI from(cppstd::string_view);

    cppstd::string_view uri;
    cppstd::string_view scheme;
    cppstd::string_view authority;
    cppstd::string_view userinfo;
    cppstd::string_view host;
    cppstd::string_view port;
    cppstd::string_view path;
    cppstd::string_view query;
    cppstd::string_view fragment;

    bool valid() const;

private:
    bool                 m_valid;
    cppstd::string m_holder;
};

export struct HttpHeader {
    struct Request {
        cppstd::string method;
        cppstd::string version;
        cppstd::string target;
    };
    struct Status {
        cppstd::string version;
        i32                  code;
    };
    struct Field {
        cppstd::string name;
        cppstd::string value;
    };
    using Start = cppstd::variant<Request, Status>;

    rstd::Option<Start>         start;
    cppstd::vector<Field> fields;

    auto has_field(cppstd::string_view) const -> bool;

    static auto parse_header(cppstd::string_view) -> rstd::Option<HttpHeader>;
    static auto parse_start_line(cppstd::string_view) -> rstd::Option<Start>;
    static auto parse_field_line(cppstd::string_view) -> Field;

    // trait
    auto clone() const -> ncrequest::HttpHeader;
    void clone_from(ncrequest::HttpHeader&);
};

} // namespace ncrequest

export template<>
struct rstd::Impl<rstd::clone::Clone, ncrequest::HttpHeader>
    : rstd::LinkClassMethod<rstd::clone::Clone, ncrequest::HttpHeader> {};