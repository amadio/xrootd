/******************************************************************************/
/* Copyright (C) 2026, XRootD Collaboration                                  */
/*                                                                            */
/* This file is part of the XrdClHttp client plugin for XRootD.               */
/*                                                                            */
/* XRootD is free software: you can redistribute it and/or modify it under    */
/* the terms of the GNU Lesser General Public License as published by the     */
/* Free Software Foundation, either version 3 of the License, or (at your     */
/* option) any later version.                                                 */
/*                                                                            */
/* XRootD is distributed in the hope that it will be useful, but WITHOUT      */
/* ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or      */
/* FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public       */
/* License for more details.                                                  */
/*                                                                            */
/* The copyright holder's institutional names and contributor's names may not */
/* be used to endorse or promote products derived from this software without  */
/* specific prior written permission of the institution or contributor.       */
/******************************************************************************/

#include "XrdClHttpCopyResponse.hh"
#include "XrdClHttpUtil.hh"
#include <charconv>

using namespace XrdClHttp;

bool CopyResponse::Feed(std::string_view data)
{
    // Bound memory even if a peer never terminates a control-channel line.
    constexpr size_t max_line = 64 * 1024;
    while (!data.empty() && m_status.IsOK()) {
        const auto newline = data.find('\n');
        const auto length = newline == data.npos ? data.size() : newline;
        if (length > max_line - m_line.size()) {
            m_status = XrdCl::XRootDStatus(XrdCl::stError, XrdCl::errErrorResponse,
                kXR_IOError, "HTTP COPY control-channel line exceeds 64 KiB");
            return false;
        }
        m_line.append(data.data(), length);
        if (newline == data.npos) break;
        HandleLine(m_line);
        m_line.clear();
        data.remove_prefix(length + 1);
    }
    return m_status.IsOK();
}

XrdCl::XRootDStatus CopyResponse::Finish()
{
    if (m_status.IsOK() && !m_line.empty()) HandleLine(m_line);
    m_line.clear();
    if (m_status.IsOK() && !m_success) {
        m_status = XrdCl::XRootDStatus(XrdCl::stError, XrdCl::errErrorResponse,
            kXR_IOError, "HTTP COPY ended without a terminal success marker; transfer status is unknown");
    }
    return m_status;
}

void CopyResponse::HandleLine(std::string_view line)
{
    line = trim_view(line);
    if (line == "Perf Marker") {
        m_in_marker = true;
        m_bytemark = -1;
        return;
    }
    if (line == "End") {
        if (m_in_marker && m_bytemark >= 0 && m_callback) m_callback(m_bytemark);
        m_in_marker = false;
        return;
    }
    const auto colon = line.find(':');
    if (colon == line.npos) return;
    const auto key = line.substr(0, colon);
    const auto value = trim_view(line.substr(colon + 1));
    if (key == "success") {
        m_success = true;
    } else if (key == "failure" || key == "failed" || key == "aborted") {
        m_status = XrdCl::XRootDStatus(XrdCl::stError, XrdCl::errErrorResponse,
            key == "aborted" ? kXR_Cancelled : kXR_IOError,
            "HTTP COPY " + std::string(key) + ": " + std::string(value));
    } else if (key == "Stripe Bytes Transferred" && m_in_marker) {
        off_t bytes = -1;
        const auto result = std::from_chars(value.data(), value.data() + value.size(), bytes);
        m_bytemark = result.ec == std::errc() && result.ptr == value.data() + value.size()
            && bytes >= 0 ? bytes : -1;
    }
}
