export module ncrequest:request_control;
export import :error;
using namespace rstd::prelude;
using namespace rstd::literals;
export namespace ncrequest
{
class Session;
class RequestControl {
    friend class Session;
    struct Fields {
        bool                               claimed {}, canceled {};
        Option<lifetchto::TransferControl> transfer;
    };
    rstd::sync::Mutex<Fields> fields_ { Fields {} };
    auto                      claim() -> lifetchto::Result<empty> {
        auto f = fields_.lock().unwrap();
        if (f->claimed)
            return Err(lifetchto::Error::InvalidOptions(
                String::make("request control was already used"_str)));
        f->claimed = true;
        if (f->canceled) return Err(lifetchto::Error::Canceled());
        return Ok(empty {});
    }
    void bind(lifetchto::TransferControl transfer) {
        auto f = fields_.lock().unwrap();
        if (f->canceled) transfer.cancel();
        f->transfer = Some(rstd::move(transfer));
    }

public:
    void cancel() const {
        auto f      = fields_.lock().unwrap();
        f->canceled = true;
        if (f->transfer.is_some()) f->transfer->cancel();
    }
    auto is_canceled() const -> bool { return fields_.lock().unwrap()->canceled; }
};
} // namespace ncrequest
