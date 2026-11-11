#ifndef __XRDXROOTDPATHUTILS__
#define __XRDXROOTDPATHUTILS__
/******************************************************************************/
/*                                                                            */
/*                 X r d X r o o t d P a t h U t i l s . h h                  */
/*                                                                            */
/* (c) 2026 by the Board of Trustees of the Leland Stanford, Jr., University  */
/*                                                                            */
/* This file is part of the XRootD software suite.                            */
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
/* You should have received a copy of the GNU Lesser General Public License   */
/* along with XRootD in a file called COPYING.LESSER (LGPL license) and file  */
/* COPYING (GPL license).  If not, see <http://www.gnu.org/licenses/>.        */
/******************************************************************************/

#include <string>

namespace XrdXrootd
{

//------------------------------------------------------------------------------
//! Normalize a path in place, as a native request does: remove the opaque part
//! after the first '?', the duplicate slashes and the "." components. This is
//! for a front end which opens files directly, for example the HTTP
//! third-party-copy handler.
//!
//! @param  path  - The path, which the function changes also when it fails.
//!
//! @return false for a path that is not absolute, that has a ".." component,
//!         or that is outside the exported paths.
//------------------------------------------------------------------------------

bool NormalizePath(std::string &path);
}
#endif
