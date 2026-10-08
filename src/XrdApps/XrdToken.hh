#ifndef XRDAPPS_TOKEN_HH
#define XRDAPPS_TOKEN_HH

#include <cstdint>
#include <string>
#include <vector>

namespace XrdApps {

struct TokenOptions {
    std::string type;
    std::string url;
    std::string issuer;
    std::string scope;
    std::string client_id;
    std::string device_endpoint;
    std::string token_endpoint;
    std::uint64_t validity{60};
    std::uint64_t timeout{0};
    bool write{false};
    std::vector<std::string> activities;
};

bool ParseTokenOptions(int argc, char **argv, TokenOptions &options,
                       std::string &error);

} // namespace XrdApps

#endif
