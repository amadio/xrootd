/******************************************************************************/
/*                                                                            */
/*                      X r d M o n F i l t e r . c c                         */
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

#include <algorithm>
#include <cctype>
#include <cstring>
#include <set>

#include <fnmatch.h>

#include "XrdApps/XrdMonCollect/XrdMonFilter.hh"

using json = nlohmann::json;

namespace
{
/******************************************************************************/
/*                        l o c a l   h e l p e r s                           */
/******************************************************************************/

const char* const kAttrPfx = "attributes.";
const char* const kResPfx  = "resource.";

std::string trim(const std::string& s)
{
   auto b = s.find_first_not_of(" \t\r\n");
   if (b == std::string::npos) return std::string();
   auto e = s.find_last_not_of(" \t\r\n");
   return s.substr(b, e - b + 1);
}

std::string lower(std::string s)
{
   std::transform(s.begin(), s.end(), s.begin(),
                  [](unsigned char c){return (char)std::tolower(c);});
   return s;
}

// Split an OR-list on ',' or '\n' (a repeated INI key arrives newline-joined),
// skipping separators nested inside a bracket expression or a brace quantifier
// so an ERE alternative such as ~x{2,4} or ~[a-z,] survives as one pattern.
//
std::vector<std::string> splitAlts(const std::string& v)
{
   std::vector<std::string> out;
   std::string cur;
   int  brack = 0, brace = 0;
   bool esc   = false;

   for (char c : v)
       {if (esc)                   {cur += c; esc = false;    continue;}
        if (c == '\\')             {cur += c; esc = true;     continue;}
        if (c == '[')              {cur += c; brack++;        continue;}
        if (c == ']' && brack > 0) {cur += c; brack--;        continue;}
        if (c == '{')              {cur += c; brace++;        continue;}
        if (c == '}' && brace > 0) {cur += c; brace--;        continue;}
        if ((c == ',' || c == '\n') && !brack && !brace)
           {std::string a = trim(cur);
            if (!a.empty()) out.push_back(a);
            cur.clear();
            continue;
           }
        cur += c;
       }
   std::string a = trim(cur);
   if (!a.empty()) out.push_back(a);
   return out;
}

// True when `s` looks like an INI key assignment. inih is built with
// INI_ALLOW_MULTILINE, so a line starting with whitespace is appended to the
// previous key's value instead of becoming a key of its own. A section header
// clears that state, so in a git-config-style indented rule the *first* key
// parses correctly and every later one is silently swallowed into the first
// key's value. Catching the swallowed text here turns a rule that quietly
// matches nothing into a start-up error naming the problem.
//
bool looksLikeKey(const std::string& s)
{
   std::size_t i = 0;
   if (i >= s.size() || !(std::isalpha((unsigned char)s[i]) || s[i] == '_'))
      return false;
   while (i < s.size() && (std::isalnum((unsigned char)s[i]) ||
                           s[i] == '_' || s[i] == '.' || s[i] == '-')) i++;
   while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) i++;
   return i < s.size() && (s[i] == '=' || s[i] == ':');
}
}

/******************************************************************************/
/*                            S h o r t c u t s                               */
/******************************************************************************/

const std::vector<std::pair<const char*, const char*>>&
XrdMonFilter::Shortcuts()
{
// Short names for the document fields worth filtering on, so a rule reads
// `user = daemon` rather than `attributes.user.name = daemon`. Anything not
// listed here stays reachable through a raw attributes./resource. path.
//
   static const std::vector<std::pair<const char*, const char*>> tbl =
   {
      // identity
      {"user",       "attributes.user.name"},
      {"userid",     "attributes.user.id"},
      {"role",       "attributes.user.roles"},          // string array
      {"vo",         "attributes.wlcg.vo"},
      {"groups",     "attributes.wlcg.groups"},
      {"authprot",   "attributes.xrootd.auth.method"},
      {"proto",      "attributes.network.protocol.name"},
      {"scheme",     "attributes.url.scheme"},
      // client
      {"client",     "attributes.client.address"},
      {"clientip",   "attributes.network.peer.address"},
      {"clientsite", "attributes.xrootd.client.site"},
      // application
      {"app",        "attributes.user_agent.name"},
      {"appver",     "attributes.user_agent.version"},
      {"appinfo",    "attributes.user_agent.original"},
      {"appid",      "attributes.xrootd.app"},
      {"experiment", "attributes.scitags.experiment"},
      {"activity",   "attributes.scitags.activity"},
      // file
      {"path",       "attributes.file.path"},
      {"dir",        "attributes.file.directory"},
      {"filename",   "attributes.file.name"},
      {"ext",        "attributes.file.extension"},
      {"dataset",    "attributes.xrootd.dataset"},
      // event
      {"event",      "attributes.event.name"},
      {"op",         "attributes.xrootd.operation.name"},
      {"state",      "attributes.xrootd.operation.state"},
      {"error",      "attributes.error.type"},
      {"provider",   "attributes.xrootd.gstream.provider"},
      {"target",     "attributes.xrootd.redirect.target.address"},
      {"session",    "attributes.session.id"},
      {"severity",   "severityText"},
      // server
      {"server",     "resource.server.address"},
      {"site",       "resource.xrootd.server.site"},
      {"instance",   "resource.service.instance.id"},
      {"program",    "resource.xrootd.server.program"},
      {"version",    "resource.service.version"}
   };
   return tbl;
}

/******************************************************************************/
/*                           R e s o l v e K e y                              */
/******************************************************************************/

bool XrdMonFilter::ResolveKey(const std::string& key, Where& w,
                              std::string& jkey)
{
   for (const auto& sc : Shortcuts())
       if (key == sc.first)
          {const char* path = sc.second;
           if (!strncmp(path, kAttrPfx, strlen(kAttrPfx)))
              {w = Where::Attributes; jkey = path + strlen(kAttrPfx);}
           else if (!strncmp(path, kResPfx, strlen(kResPfx)))
              {w = Where::Resource;   jkey = path + strlen(kResPfx);}
           else {w = Where::Top; jkey = path;}
           return true;
          }

// Not a shortcut: accept an explicit path into either of the two OpenTelemetry
// objects, so a field with no shortcut is still reachable.
//
   if (!key.compare(0, strlen(kAttrPfx), kAttrPfx) && key.size() > strlen(kAttrPfx))
      {w = Where::Attributes; jkey = key.substr(strlen(kAttrPfx)); return true;}
   if (!key.compare(0, strlen(kResPfx), kResPfx) && key.size() > strlen(kResPfx))
      {w = Where::Resource;   jkey = key.substr(strlen(kResPfx));  return true;}
   return false;
}

/******************************************************************************/
/*                              A d d R u l e                                 */
/******************************************************************************/

std::size_t XrdMonFilter::AddRule(const std::string& name)
{
   Rule r;
   r.name  = name;
   r.label = name;   // the rule name is the default label
   rules.push_back(std::move(r));
   return rules.size() - 1;
}

/******************************************************************************/
/*                         A d d C o n d i t i o n                            */
/******************************************************************************/

bool XrdMonFilter::AddCondition(std::size_t r, const std::string& key,
                                const std::string& value, std::string& err)
{
   if (r >= rules.size()) {err = "internal error: bad rule index"; return false;}

   Cond c;
   if (!ResolveKey(lower(trim(key)), c.where, c.key))
      {err = "unknown filter key '" + key + "'; use one of:";
       for (const auto& sc : Shortcuts()) {err += ' '; err += sc.first;}
       err += ", or a raw attributes.<key> / resource.<key> path";
       return false;
      }

   std::string v = trim(value);
   if (!v.empty() && v[0] == '!') {c.negate = true; v = trim(v.substr(1));}

   std::vector<std::string> alts = splitAlts(v);
   if (alts.empty())
      {err = "filter key '" + key + "' has an empty value"; return false;}

   for (const std::string& a : alts)
       {if (looksLikeKey(a))
           {err = "'" + a + "' was parsed as part of the value of '" + key
                + "': a filter key must start in column 1 (an indented line is"
                  " appended to the preceding key's value)";
            return false;
           }
        Alt alt;
        if (a[0] == '~')
           {alt.kind = Alt::Regex;
            alt.text = a.substr(1);
            alt.re.reset(new regex_t());
            if (regcomp(alt.re.get(), alt.text.c_str(), REG_EXTENDED|REG_NOSUB))
               {alt.re.reset();   // regcomp failed: nothing to free
                err = "'" + alt.text + "' (from '" + key + "') is not a valid"
                      " POSIX extended regular expression";
                return false;
               }
           }
        else if (a.find_first_of("*?") != std::string::npos)
           {alt.kind = Alt::Glob;  alt.text = a;}
        else
           {alt.kind = Alt::Exact; alt.text = a;}
        c.alts.push_back(std::move(alt));
       }

   rules[r].conds.push_back(std::move(c));
   return true;
}

/******************************************************************************/
/*                            S e t A c t i o n                               */
/******************************************************************************/

bool XrdMonFilter::SetAction(std::size_t r, const std::string& action,
                             std::string& err)
{
   if (r >= rules.size()) {err = "internal error: bad rule index"; return false;}

   std::string a = lower(trim(action));
   if      (a == "tag")  rules[r].action = Action::Tag;
   else if (a == "drop") rules[r].action = Action::Drop;
   else if (a == "keep") rules[r].action = Action::Keep;
   else {err = "unknown action '" + action + "'; use tag, drop or keep";
         return false;}
   return true;
}

/******************************************************************************/
/*                             S e t L a b e l                                */
/******************************************************************************/

void XrdMonFilter::SetLabel(std::size_t r, const std::string& label)
{
   if (r < rules.size()) rules[r].label = trim(label);
}

/******************************************************************************/
/*                              V a l i d a t e                               */
/******************************************************************************/

bool XrdMonFilter::Validate(std::string& err) const
{
   for (const Rule& r : rules)
       {if (r.conds.empty())
           {err = "filter rule '" + r.name + "' has no match conditions; it"
                  " would match every document";
            return false;
           }
        if (r.action == Action::Tag && r.label.empty())
           {err = "filter rule '" + r.name + "' tags with an empty label, so it"
                  " would do nothing";
            return false;
           }
       }
   return true;
}

/******************************************************************************/
/*                                 C o u n t                                  */
/******************************************************************************/

std::size_t XrdMonFilter::Count(Action a) const
{
   std::size_t n = 0;
   for (const Rule& r : rules) if (r.action == a) n++;
   return n;
}

/******************************************************************************/
/*                              s c a l a r O f                               */
/******************************************************************************/

// Render one JSON value as the string a pattern is matched against. Containers
// and null have no sensible rendering and never match.
//
bool XrdMonFilter::scalarOf(const json& v, std::string& out)
{
   if (v.is_string())         {out = v.get<std::string>();            return true;}
   if (v.is_boolean())        {out = v.get<bool>() ? "true" : "false"; return true;}
   if (v.is_number_integer()) {out = std::to_string(v.get<long long>()); return true;}
   if (v.is_number_unsigned()){out = std::to_string(v.get<unsigned long long>());
                                                                       return true;}
   if (v.is_number_float())   {out = v.dump();                        return true;}
   return false;
}

/******************************************************************************/
/*                             a l t s M a t c h                              */
/******************************************************************************/

bool XrdMonFilter::altsMatch(const std::vector<Alt>& alts, const std::string& v)
{
   for (const Alt& a : alts)
       switch (a.kind)
          {case Alt::Exact: if (v == a.text)                          return true;
                            break;
           case Alt::Glob:  if (!fnmatch(a.text.c_str(), v.c_str(), 0)) return true;
                            break;
           case Alt::Regex: if (a.re && !regexec(a.re.get(), v.c_str(), 0,
                                                 nullptr, 0))         return true;
                            break;
          }
   return false;
}

/******************************************************************************/
/*                           c o n d M a t c h e s                            */
/******************************************************************************/

bool XrdMonFilter::condMatches(const json& d, const Cond& c)
{
   const json* obj = &d;
   if (c.where != Where::Top)
      {const char* sec = (c.where == Where::Resource) ? "resource" : "attributes";
       auto it = d.find(sec);
       if (it == d.end() || !it->is_object()) return false;
       obj = &(*it);
      }

   auto it = obj->find(c.key);
   if (it == obj->end() || it->is_null()) return false;   // absent never matches

   bool hit = false;
   if (it->is_array())
      {// An array field (user.roles) matches when any of its elements does.
       for (const json& e : *it)
           {std::string s;
            if (scalarOf(e, s) && altsMatch(c.alts, s)) {hit = true; break;}
           }
      }
      else {std::string s;
            if (!scalarOf(*it, s)) return false;   // object: never matches
            hit = altsMatch(c.alts, s);
           }

   return c.negate ? !hit : hit;
}

/******************************************************************************/
/*                                 A p p l y                                  */
/******************************************************************************/

bool XrdMonFilter::Apply(json& d) const
{
   std::set<std::string> labels;   // sorted + de-duplicated for a stable array
   bool anyDrop = false, anyKeep = false;

   for (const Rule& r : rules)
       {bool all = true;
        for (const Cond& c : r.conds)
            if (!condMatches(d, c)) {all = false; break;}
        if (!all) continue;

        if (!r.label.empty()) labels.insert(r.label);
        if      (r.action == Action::Drop) anyDrop = true;
        else if (r.action == Action::Keep) anyKeep = true;
       }

// A keep rule always wins, so the outcome does not depend on the order the
// rules happen to be loaded in. Labels are applied either way, so a document
// rescued by a keep rule still shows which rules matched it.
//
   if (!labels.empty())
      d["attributes"][kLabelKey] = json(labels);

   return !(anyDrop && !anyKeep);
}
