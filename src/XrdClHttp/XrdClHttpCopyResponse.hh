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

#ifndef XRDCLHTTP_COPY_RESPONSE_HH
#define XRDCLHTTP_COPY_RESPONSE_HH

#include <XrdCl/XrdClXRootDResponses.hh>
#include <functional>
#include <string>
#include <string_view>
#include <sys/types.h>
#include <utility>

namespace XrdClHttp {

// Incremental parser for the HTTP TPC control channel, after transfer decoding.
class CopyResponse {
public:
    bool Feed(std::string_view data);
    XrdCl::XRootDStatus Finish();
    void SetCallback(std::function<void(off_t)> callback) { m_callback = std::move(callback); }
    const XrdCl::XRootDStatus &Status() const { return m_status; }

private:
    void HandleLine(std::string_view line);
    std::string m_line;
    std::function<void(off_t)> m_callback;
    XrdCl::XRootDStatus m_status;
    off_t m_bytemark{-1};
    bool m_in_marker{false};
    bool m_success{false};
};

} // namespace XrdClHttp
#endif
