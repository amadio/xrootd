// Exercise the ownership-transferring FilePlugIn::Write overload against the
// loopback server in cli.py. Unlike xrdcp, this path hands the plugin a Buffer.
#include "XrdClHttp/XrdClHttpFactory.hh"
#include "XrdCl/XrdClBuffer.hh"
#include "XrdCl/XrdClMessageUtils.hh"
#include "XrdCl/XrdClPlugInInterface.hh"

#include <cstring>
#include <array>
#include <iostream>
#include <memory>
#include <string>
#include <utility>

int main(int argc, char **argv)
{
    if (argc != 3) return 2;
    XrdClHttp::Factory factory;
    std::array<XrdCl::SyncResponseHandler, 2> writes;
    std::unique_ptr<XrdCl::FilePlugIn> file(factory.CreateFile(argv[1]));
    const bool known_size = std::string(argv[2]) == "known";
    const std::string url = std::string(argv[1])
        + (known_size ? "?oss.asize=131072" : "");
    auto wait = [](XrdCl::XRootDStatus status,
                   XrdCl::SyncResponseHandler &handler) {
        if (status.IsOK()) status = XrdCl::MessageUtils::WaitForStatus(&handler);
        if (!status.IsOK()) std::cerr << status.ToStr() << '\n';
        return status.IsOK();
    };
    XrdCl::SyncResponseHandler open;
    if (!wait(file->Open(url, XrdCl::OpenFlags::Write,
                        XrdCl::Access::None, &open, 5), open)) return 1;
    for (unsigned chunk = 0; chunk < 2; ++chunk) {
        XrdCl::Buffer buffer(65536);
        std::memset(buffer.GetBuffer(), 'a' + chunk, buffer.GetSize());
        auto status = file->Write(chunk * 65536, std::move(buffer),
                                  &writes[chunk], 5);
        if (!status.IsOK()) {
            std::cerr << status.ToStr() << '\n';
            return 1;
        }
    }
    for (auto &write : writes)
        if (!wait({}, write)) return 1;
    XrdCl::SyncResponseHandler close;
    return wait(file->Close(&close, 5), close) ? 0 : 1;
}
