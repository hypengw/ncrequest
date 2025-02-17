#pragma once
#include <filesystem>
#include "ncrequest/type.h"

namespace ncrequest
{
class SessionShare {
public:
    class Private;
    SessionShare();
    ~SessionShare();

    auto handle() const -> voidp;
    void load(const std::filesystem::path& p);
    void save(const std::filesystem::path& p) const;

private:
    rc<Private>           d_ptr;
    inline Private*       d_func() { return d_ptr.get(); }
    inline const Private* d_func() const { return d_ptr.get(); }
};
} // namespace ncrequest