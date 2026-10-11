export module ncrequest:cache_mode;
export namespace ncrequest
{
enum class CacheMode
{
    Default,
    Validate,
    Bypass,
    OnlyIfCached
};
}
