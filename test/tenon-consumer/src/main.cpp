import ncrequest.curl;

int main() { return ncrequest::curl_init().is_ok() ? 0 : 1; }
