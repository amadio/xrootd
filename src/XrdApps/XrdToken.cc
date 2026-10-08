#include "XrdApps/XrdToken.hh"

#include "XrdCl/XrdClBuffer.hh"
#include "XrdCl/XrdClFileSystem.hh"
#include "XrdCl/XrdClURL.hh"
#include <XrdOuc/XrdOucJson.hh>

#include <cstdlib>
#include <algorithm>
#include <chrono>
#include <iostream>
#include <limits>
#include <memory>
#include <sstream>
#include <thread>

namespace XrdApps {

bool ParseTokenOptions(int argc, char **argv, TokenOptions &options,
                       std::string &error)
{
    options = TokenOptions{};
    error.clear();
    if (argc < 2) {
        error = "Choose macaroon, oauth, oauth-macaroon, id, or oauth-device";
        return false;
    }
    options.type = argv[1];
    if (options.type != "macaroon" && options.type != "oauth" &&
        options.type != "oauth-macaroon" && options.type != "id" &&
        options.type != "oauth-device") {
        error = "Unknown token operation";
        return false;
    }
    bool end_options = false;
    bool validity_set = false;
    for (int i = 2; i < argc; ++i) {
        const std::string arg = argv[i];
        if (!end_options && arg == "--") {
            end_options = true;
            continue;
        }
        if (!end_options && (arg == "--issuer" || arg == "--scope" ||
                             arg == "--client-id" || arg == "--validity" ||
                             arg == "--timeout" ||
                             arg == "--activity" ||
                             arg == "--device-endpoint" ||
                             arg == "--token-endpoint")) {
            if (++i == argc) {
                error = arg + " requires a value";
                return false;
            }
            const std::string value = argv[i];
            if (arg == "--issuer") options.issuer = value;
            else if (arg == "--scope") options.scope = value;
            else if (arg == "--client-id") options.client_id = value;
            else if (arg == "--device-endpoint")
                options.device_endpoint = value;
            else if (arg == "--token-endpoint")
                options.token_endpoint = value;
            else if (arg == "--activity") options.activities.push_back(value);
            else {
                if (arg == "--validity") validity_set = true;
                const char *name = arg == "--timeout" ? "Timeout" : "Validity";
                if (value.empty()) {
                    error = std::string(name) + " must be a nonnegative integer";
                    return false;
                }
                std::uint64_t number = 0;
                for (unsigned char digit : value) {
                    if (digit < '0' || digit > '9' ||
                        number > (std::numeric_limits<std::uint64_t>::max() -
                                    (digit - '0')) / 10) {
                        error = std::string(name) + " must be a nonnegative integer";
                        return false;
                    }
                    number = number * 10 + (digit - '0');
                }
                if (arg == "--timeout") {
                    if (number > 3600) {
                        error = "Timeout must be at most 3600 seconds";
                        return false;
                    }
                    options.timeout = number;
                } else options.validity = number;
            }
        } else if (!end_options && arg == "--write") {
            options.write = true;
        } else if (!end_options && !arg.empty() && arg[0] == '-') {
            error = "Unknown option: " + arg;
            return false;
        } else if (options.url.empty()) {
            options.url = arg;
        } else {
            error = "Only one storage URL is accepted; use --activity for each activity";
            return false;
        }
    }

    if (options.type == "oauth" || options.type == "id" ||
        options.type == "oauth-device") {
        if (options.issuer.empty() || options.scope.empty() ||
            options.client_id.empty()) {
            error = "This operation requires --issuer, --scope, and --client-id";
            return false;
        }
        if (!options.url.empty() || options.write ||
            !options.activities.empty() || validity_set) {
            error = "This operation does not take storage or macaroon options";
            return false;
        }
    } else if (options.url.empty()) {
        error = "A storage URL is required";
        return false;
    }
    if (options.type == "macaroon" && !options.issuer.empty()) {
        error = "Direct macaroon requests do not take --issuer";
        return false;
    }
    if (options.type == "macaroon" &&
        (!options.scope.empty() || !options.client_id.empty())) {
        error = "Direct macaroon requests do not take OAuth options";
        return false;
    }
    if (options.type == "oauth-macaroon" && options.issuer.empty()) {
        error = "oauth-macaroon requires --issuer";
        return false;
    }
    if (options.type == "oauth-macaroon" && !options.scope.empty()) {
        error = "oauth-macaroon derives scope from the storage path";
        return false;
    }
    if (options.type == "id") {
        std::istringstream scopes(options.scope);
        std::string scope;
        bool has_openid = false;
        while (scopes >> scope) has_openid |= scope == "openid";
        if (!has_openid) {
            error = "ID token requests require the openid scope";
            return false;
        }
    }
    if (options.device_endpoint.empty() != options.token_endpoint.empty()) {
        error = "--device-endpoint and --token-endpoint must be supplied together";
        return false;
    }
    if ((!options.device_endpoint.empty() || !options.token_endpoint.empty()) &&
        options.type != "id" && options.type != "oauth-device") {
        error = "Explicit device endpoints require id or oauth-device";
        return false;
    }
    return true;
}

} // namespace XrdApps

namespace {

void Usage()
{
    std::cerr << "Usage:\n"
              << "  xrdtoken macaroon [--write] [--validity MIN]"
                 " [--activity NAME] URL\n"
              << "  xrdtoken oauth --issuer URL --scope SCOPE --client-id ID\n"
              << "  xrdtoken id --issuer URL --scope 'openid ...' --client-id ID\n"
              << "  xrdtoken oauth-device --issuer URL --scope SCOPE"
                 " --client-id ID\n"
              << "    [--device-endpoint URL --token-endpoint URL]\n"
              << "  xrdtoken oauth-macaroon --issuer URL [--client-id ID]"
                 " [--write] [--validity MIN] [--activity NAME] URL\n"
              << "Client credentials can use mTLS or XRD_TOKEN_CLIENT_SECRET.\n";
    std::cerr << "Use --timeout SEC to bound each token request workflow.\n";
}

bool QueryToken(const XrdCl::URL &url, const nlohmann::json &request,
                std::string &response, std::uint64_t timeout)
{
    XrdCl::FileSystem filesystem(url);
    XrdCl::Buffer *raw_response = nullptr;
    XrdCl::Buffer query;
    query.FromString(request.dump());
    const auto status = filesystem.Query(XrdCl::QueryCode::Visa,
                                         query, raw_response,
                                         static_cast<time_t>(timeout));
    std::unique_ptr<XrdCl::Buffer> buffer(raw_response);
    if (!status.IsOK() || !buffer || buffer->ToString().empty()) {
        std::cerr << "xrdtoken: token request failed";
        if (!status.IsOK()) std::cerr << ": " << status.ToStr();
        std::cerr << '\n';
        return false;
    }
    response = buffer->ToString();
    return true;
}

bool RunDeviceFlow(const XrdCl::URL &url, nlohmann::json request,
                   bool id_token, std::uint64_t timeout)
{
    request["type"] = "device-start";
    std::string raw;
    if (!QueryToken(url, request, raw, timeout)) return false;
    auto device = nlohmann::json::parse(raw, nullptr, false);
    if (device.is_discarded() || !device.is_object() ||
        !device.contains("verification_uri") ||
        !device["verification_uri"].is_string() ||
        !device.contains("user_code") || !device["user_code"].is_string() ||
        !device.contains("device_code") || !device["device_code"].is_string() ||
        !device.contains("token_endpoint") ||
        !device["token_endpoint"].is_string() ||
        !device.contains("expires_in") ||
        !device["expires_in"].is_number_unsigned()) {
        std::cerr << "xrdtoken: invalid device authorization response\n";
        return false;
    }
    const auto expiry_seconds = device["expires_in"].get<std::uint64_t>();
    if (expiry_seconds == 0 || expiry_seconds > 3600) {
        std::cerr << "xrdtoken: invalid device authorization lifetime\n";
        return false;
    }
    std::uint64_t interval = 5;
    if (device.contains("interval") && device["interval"].is_number_unsigned())
        interval = device["interval"].get<std::uint64_t>();
    if (interval == 0 || interval > 60) interval = 5;
    std::cerr << "Open " << device["verification_uri"].get<std::string>()
              << " and enter code " << device["user_code"].get<std::string>()
              << '\n';

    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(expiry_seconds);
    request["type"] = "device-poll";
    request["device_code"] = device["device_code"];
    request["token_endpoint"] = device["token_endpoint"];
    const char *token_key = id_token ? "id_token" : "access_token";
    while (std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::seconds(interval));
        if (std::chrono::steady_clock::now() >= deadline) break;
        if (!QueryToken(url, request, raw, timeout)) return false;
        auto result = nlohmann::json::parse(raw, nullptr, false);
        if (result.is_discarded() || !result.is_object()) break;
        auto token = result.find(token_key);
        if (token != result.end() && token->is_string() &&
            !token->get_ref<const std::string &>().empty()) {
            std::cout << token->get<std::string>() << '\n';
            return true;
        }
        const auto error_entry = result.find("error");
        const auto oauth_error = error_entry != result.end() &&
                                 error_entry->is_string() ?
                                 error_entry->get<std::string>() : std::string{};
        if (oauth_error == "authorization_pending") continue;
        if (oauth_error == "slow_down") {
            interval = std::min<std::uint64_t>(interval + 5, 60);
            continue;
        }
        std::cerr << "xrdtoken: device authorization failed\n";
        return false;
    }
    std::cerr << "xrdtoken: device authorization expired\n";
    return false;
}

} // namespace

int main(int argc, char **argv)
{
    if (argc == 2 && std::string(argv[1]) == "--help") {
        Usage();
        return 0;
    }
    XrdApps::TokenOptions options;
    std::string error;
    if (!XrdApps::ParseTokenOptions(argc, argv, options, error)) {
        std::cerr << "xrdtoken: " << error << '\n';
        Usage();
        return 2;
    }

    const std::string source = (options.type == "oauth" ||
                                options.type == "id" ||
                                options.type == "oauth-device") ?
        options.issuer : options.url;
    XrdCl::URL url(source);
    if (!url.IsValid() || (url.GetProtocol() != "https" &&
                           url.GetProtocol() != "davs")) {
        std::cerr << "xrdtoken: HTTPS or DAVS URL required\n";
        return 2;
    }
    std::string path = "/";
    if (options.type == "macaroon" || options.type == "oauth-macaroon") {
        const auto authority_start = options.url.find("://") + 3;
        const auto path_start = options.url.find_first_of("/?#", authority_start);
        if (path_start != std::string::npos &&
            options.url[path_start] != '#') {
            if (options.url[path_start] != '/') path += options.url.substr(path_start);
            else path = options.url.substr(path_start);
            const auto fragment = path.find('#');
            if (fragment != std::string::npos) path.erase(fragment);
        }
    }
    url.SetPath("/");
    url.SetParams(XrdCl::URL::ParamsMap{});

    nlohmann::json request = {
        {"type", options.type}, {"path", path.empty() ? "/" : path},
        {"validity", options.validity}, {"write", options.write},
        {"activities", options.activities}
    };
    if (!options.issuer.empty()) request["issuer"] = options.issuer;
    if (!options.scope.empty()) request["scope"] = options.scope;
    if (!options.client_id.empty()) request["client_id"] = options.client_id;
    if (!options.device_endpoint.empty()) {
        request["device_endpoint"] = options.device_endpoint;
        request["token_endpoint"] = options.token_endpoint;
    }
    if (options.type != "macaroon") {
        const char *secret = std::getenv("XRD_TOKEN_CLIENT_SECRET");
        if (secret != nullptr && *secret != '\0') request["client_secret"] = secret;
    }

    if (options.type == "id" || options.type == "oauth-device") {
        return RunDeviceFlow(url, request, options.type == "id",
                             options.timeout) ? 0 : 1;
    }
    std::string response;
    if (!QueryToken(url, request, response, options.timeout)) return 1;
    std::cout << response << '\n';
    return 0;
}
