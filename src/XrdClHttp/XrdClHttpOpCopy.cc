/******************************************************************************/
/* Copyright (C) 2025, Pelican Project, Morgridge Institute for Research      */
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

#include "XrdClHttpOps.hh"

#include <string>

using namespace XrdClHttp;
using namespace std::string_literals;

CurlCopyOp::CurlCopyOp(XrdCl::ResponseHandler *handler, const std::string &source_url, const Headers &source_hdrs,
    const std::string &dest_url, const Headers &dest_hdrs, const Headers &connection_hdrs, TpcMode mode, struct timespec timeout, XrdCl::Log *logger,
    CreateConnCalloutType callout) :
        CurlOperation(handler, mode == TpcMode::Pull ? dest_url : source_url, timeout, logger, callout, nullptr)
    {
        m_minimum_rate = 1;
        m_response.SetCallback([this](off_t bytes) {
            if (m_progress_handler)
                m_progress_handler->HandleProgress(static_cast<std::size_t>(bytes));
        });

        // The headers of the endpoint the client contacts go on the request
        // itself. The headers of the remote endpoint are forwarded by that
        // endpoint, thus they need the 'TransferHeader' prefix.
        const Headers &regular_hdrs  = mode == TpcMode::Pull ? dest_hdrs : source_hdrs;
        const Headers &transfer_hdrs = mode == TpcMode::Pull ? source_hdrs : dest_hdrs;

        if (mode == TpcMode::Pull)
            m_headers_list.emplace_back("Source"s, source_url);
        else
            m_headers_list.emplace_back("Destination"s, dest_url);

        std::copy(connection_hdrs.begin(), connection_hdrs.end(), std::back_inserter(m_headers_list));
        std::copy(regular_hdrs.begin(),    regular_hdrs.end(),    std::back_inserter(m_headers_list));

        for (const auto &info : transfer_hdrs) {
            m_headers_list.emplace_back("TransferHeader"s + info.first, info.second);
        }
    }
    
    bool
    CurlCopyOp::Setup(CURL *curl, CurlWorker &worker)
    {
        auto rv = CurlOperation::Setup(curl, worker);
        if (!rv) return false;

        curl_easy_setopt(m_curl.get(), CURLOPT_WRITEFUNCTION, CurlCopyOp::WriteCallback);
        curl_easy_setopt(m_curl.get(), CURLOPT_WRITEDATA, this);
        curl_easy_setopt(m_curl.get(), CURLOPT_CUSTOMREQUEST, "COPY");

        return true;
    }
    
    void
    CurlCopyOp::Success()
    {
        const auto result = m_response.Finish();
        if (!result.IsOK()) {
            Fail(result.code, result.errNo, result.GetErrorMessage());
            return;
        }
        m_sent_success = true;
        SetDone(false);
        if (m_handler == nullptr) {return;}
        auto status = new XrdCl::XRootDStatus();
        auto obj = new XrdCl::AnyObject();
        auto handle = m_handler;
        m_handler = nullptr;
        handle->HandleResponse(status, obj);
    }
    
    void
    CurlCopyOp::Fail(uint16_t errCode, uint32_t errNum, const std::string &msg)
    {
        m_sent_success = false;
        m_failure = msg;
        CurlOperation::Fail(errCode, errNum, msg);
    }

    void
    CurlCopyOp::ReleaseHandle()
    {
        if (m_curl == nullptr) return;
        curl_easy_setopt(m_curl.get(), CURLOPT_WRITEFUNCTION, nullptr);
        curl_easy_setopt(m_curl.get(), CURLOPT_WRITEDATA, nullptr);
        curl_easy_setopt(m_curl.get(), CURLOPT_CUSTOMREQUEST, nullptr);
        curl_easy_setopt(m_curl.get(), CURLOPT_HTTPHEADER, nullptr);
        curl_easy_setopt(m_curl.get(), CURLOPT_XFERINFOFUNCTION, nullptr);
        CurlOperation::ReleaseHandle();
    }

    void
    CurlCopyOp::SetProgressHandler(XrdCl::ProgressHandler *handler) noexcept
    {
        m_progress_handler = handler;
    }

    size_t
    CurlCopyOp::WriteCallback(char *buffer, size_t size, size_t nitems, void *this_ptr)
    {
        auto me = reinterpret_cast<CurlCopyOp*>(this_ptr);
        me->UpdateBytes(size * nitems);
        // Redirect and error bodies are not the TPC control channel. Preserve
        // the HTTP status and let the worker handle those responses.
        const auto status_code = me->m_headers.GetStatusCode();
        if (status_code < 200 || status_code >= 300) return size * nitems;
        if (!me->m_response.Feed(std::string_view(buffer, size * nitems))) {
            const auto &status = me->m_response.Status();
            return me->FailCallback(static_cast<XErrorCode>(status.errNo), status.GetErrorMessage());
        }
        return size * nitems;
    }

