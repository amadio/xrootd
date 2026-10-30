//------------------------------------------------------------------------------
// Copyright (c) 2026 by European Organization for Nuclear Research (CERN)
//------------------------------------------------------------------------------
// This file is part of the XRootD software suite.
//
// XRootD is free software: you can redistribute it and/or modify
// it under the terms of the GNU Lesser General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// XRootD is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU Lesser General Public License
// along with XRootD.  If not, see <http://www.gnu.org/licenses/>.
//
// In applying this licence, CERN does not waive the privileges and immunities
// granted to it by virtue of its status as an Intergovernmental Organization
// or submit itself to any jurisdiction.
//------------------------------------------------------------------------------

#ifndef __XRD_CL_COPY_CHECKSUM_HH__
#define __XRD_CL_COPY_CHECKSUM_HH__

#include "XrdCl/XrdClPropertyList.hh"

namespace XrdCl
{
  //----------------------------------------------------------------------------
  //! Validate explicit checksum syntax and supported job combinations without
  //! reading a source or opening a destination.
  //----------------------------------------------------------------------------
  XRootDStatus ValidateCopyChecksum( const PropertyList &properties );

  //----------------------------------------------------------------------------
  //! Prepare explicit source/target checksum verification before starting a
  //! transfer. Jobs without checkSumVerify retain their legacy checksum policy.
  //! This private helper must run before the destination is opened.
  //----------------------------------------------------------------------------
  XRootDStatus PrepareCopyChecksum( PropertyList &properties,
                                   PropertyList &results );

  //----------------------------------------------------------------------------
  //! Finish explicit checksum verification after a successful transfer. Failed
  //! transfers retain their original status and never trigger target removal.
  //----------------------------------------------------------------------------
  XRootDStatus FinishCopyChecksum( PropertyList       &properties,
                                  PropertyList       &results,
                                  const XRootDStatus &transferStatus );
}

#endif // __XRD_CL_COPY_CHECKSUM_HH__
