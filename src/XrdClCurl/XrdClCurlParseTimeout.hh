/****************************************************************
 *
 * Copyright (C) 2024, Pelican Project, Morgridge Institute for Research
 *
 ***************************************************************/

#ifndef XRDCLCURL_PARSETIMEOUT_HH
#define XRDCLCURL_PARSETIMEOUT_HH

#include <string>

#include <time.h>

namespace XrdClCurl {

// Parse a given string as a duration, returning the parsed value as a timespec.
//
// The implementation is based on the Go duration format which is a signed sequence of
// decimal numbers with a unit suffix.
//
// Examples:
// - 30ms
// - 1h5m
// Valid time units are "ns", "us", "ms", "s", "m", "h".  Unlike go, UTF-8 for microsecond is not accepted
//
// If an invalid value is given, false is returned and the errmsg is set.
bool ParseTimeout(const std::string &duration, struct timespec &, std::string &errmsg);

// Given a time value, marshal it to a string (based on the Go duration format)
//
// Result will be of the form XYZsABCms (or 1s500ms for 1.5 seconds).
std::string MarshalDuration(const struct timespec &timeout);

}

#endif // XRDCLCURL_PARSETIMEOUT_HH
