/***************************************************************
 *
 * Copyright (C) 2025, Pelican Project, Morgridge Institute for Research
 *
 ***************************************************************/

#include "XrdClCurlOps.hh"

#include <XrdCl/XrdClFileSystem.hh>
#include <XrdCl/XrdClLog.hh>

using namespace XrdClCurl;

void CurlQueryOp::Success()
{
    SetDone(false);
    m_logger->Debug(kLogXrdClCurl, "CurlQueryOp::Success");

    if (m_queryCode == XrdCl::QueryCode::XAttr) {
        XrdCl::Buffer *qInfo = new XrdCl::Buffer();
        qInfo->FromString(m_headers.GetETag());
        auto obj = new XrdCl::AnyObject();
        obj->Set(qInfo);

        m_handler->HandleResponse(new XrdCl::XRootDStatus(), obj);
        m_handler = nullptr;
    }
    else {
        m_logger->Error(kLogXrdClCurl, "Invalid information query type code");
        Fail(XrdCl::errInvalidArgs, XrdCl::errErrorResponse, "Unsupported query code");
    }
}
