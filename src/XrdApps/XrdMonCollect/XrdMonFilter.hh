#ifndef __XRDMONFILTER_HH__
#define __XRDMONFILTER_HH__
/******************************************************************************/
/*                                                                            */
/*                      X r d M o n F i l t e r . h h                         */
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

#include <cstddef>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <regex.h>

#include "XrdOuc/XrdOucJson.hh"

//-----------------------------------------------------------------------------
//! A set of named rules evaluated against each finished monitoring document
//! immediately before it is handed to the output sinks. A rule either drops the
//! document or tags it with a label, so activity that is uninteresting to end
//! users (an EOS instance's own internal traffic, say) can be suppressed at the
//! collector or merely marked for filtering downstream.
//!
//! Rules come from `[filter "<name>"]` sections of the collector's INI
//! configuration; this class is deliberately unaware of the INI parser and is
//! driven through AddRule()/AddCondition()/SetAction()/SetLabel().
//!
//! \verbatim
//! [filter "eos-internal"]
//! user     = daemon, root, ~^[0-9]+$
//! authprot = sss
//! action   = tag
//! label    = internal
//! \endverbatim
//!
//! A rule's keys are ANDed. A value is a comma- (or newline-) separated OR-list
//! whose alternatives are matched against the document value:
//!
//! \li an exact string by default, so `daemon` never matches `mydaemon`;
//! \li a glob when it contains `*` or `?` (fnmatch(3) with no flags, so `*`
//!     crosses `/` and `path = /eos/*/proc/*` works as written);
//! \li a POSIX extended regular expression when it starts with `~`, compiled
//!     once here and matched unanchored, like the collector's --dataset.
//!
//! A leading `!` on the whole value negates the condition.
//!
//! A key naming a field the document does not carry **never** matches, negated
//! or not: that is what keeps `user = daemon` from firing on a server-identity
//! document, which has no user at all. So `user = !daemon` reads "has a user
//! and it is not daemon", not "has no daemon user" — both polarities fail open,
//! which is the safe default for a drop rule.
//!
//! Evaluation is deliberately independent of rule order (the INI parser hands
//! sections back sorted, not in file order): every rule is evaluated, the
//! labels of all matching rules are unioned into the document, and the document
//! is dropped only when some matching rule says `drop` and none says `keep`.
//!
//! Built once at start-up and const afterwards, so the decode thread needs no
//! locking. Matching is done on the serializer thread, never in the UDP receive
//! path.
//-----------------------------------------------------------------------------

class XrdMonFilter
{
public:

using json = nlohmann::json;

//! What a matching rule does to the document.
enum class Action : unsigned char {Tag = 0, Drop, Keep};

//! Which part of the document a condition's key names.
enum class Where : unsigned char {Attributes, Resource, Top};

//! The event attribute carrying the union of every matching rule's label.
static constexpr const char* kLabelKey = "xrootd.filter.labels";

XrdMonFilter() = default;
~XrdMonFilter() = default;

XrdMonFilter(const XrdMonFilter&)            = delete;   // owns compiled regexes
XrdMonFilter& operator=(const XrdMonFilter&) = delete;

//! Start a rule named `name`, which is also its default label. Rule identity
//! is the returned index; uniqueness of names is the caller's business.
//!
//! @return the index to pass to AddCondition/SetAction/SetLabel.
std::size_t AddRule(const std::string& name);

//! Add one ANDed condition to rule `r`. `key` is a shortcut name (see
//! Shortcuts()) or a raw `attributes.<key>` / `resource.<key>` path; `value`
//! is the `[!]alt[,alt...]` OR-list described above.
//!
//! @return false, with `err` set and the rule unchanged, on an unknown key, an
//!         empty value, an alternative that looks like a misindented INI key,
//!         or a `~` expression that does not compile.
bool AddCondition(std::size_t r, const std::string& key,
                  const std::string& value, std::string& err);

//! Set rule `r`'s action from "tag", "drop" or "keep" (case-insensitive).
//! @return false with `err` set for anything else.
bool SetAction(std::size_t r, const std::string& action, std::string& err);

//! Override rule `r`'s label. An empty label tags nothing.
void SetLabel(std::size_t r, const std::string& label);

//! Post-load sanity check, to be called once every rule has been added.
//! @return false with `err` set for a rule with no conditions (it would match
//!         every document) or a `tag` rule that would tag nothing.
bool Validate(std::string& err) const;

bool        Empty() const {return rules.empty();}
std::size_t Size()  const {return rules.size();}

//! How many rules carry action `a` (for the start-up summary).
std::size_t Count(Action a) const;

//! Evaluate every rule against the finished document `d`, adding the union of
//! all matching rules' labels to `d["attributes"][kLabelKey]` as a sorted,
//! de-duplicated string array (omitted when no rule matched).
//!
//! @return false when `d` must not be emitted: some matching rule is `drop`
//!         and no matching rule is `keep`.
bool Apply(json& d) const;

//! The shortcut table as (short name, dotted document path) pairs, in a stable
//! order, for usage and diagnostic text.
static const std::vector<std::pair<const char*, const char*>>& Shortcuts();

//! Resolve a condition key to a document location.
//! @return false when `key` is neither a shortcut nor a raw
//!         `attributes.`/`resource.` path.
static bool ResolveKey(const std::string& key, Where& w, std::string& jkey);

private:

//! One alternative of an OR-list.
struct Alt
{
   enum Kind : unsigned char {Exact, Glob, Regex};

   //! regfree() the compiled expression when the alternative goes away. The
   //! regex_t is heap-held so that growing the enclosing vector cannot move a
   //! live pattern out from under a match in progress.
   struct ReDel {void operator()(regex_t* r) const {if (r) {regfree(r); delete r;}}};

   Kind        kind = Exact;
   std::string text;                     // pattern (for Regex: the source)
   std::unique_ptr<regex_t, ReDel> re;   // Regex only
};

struct Cond
{
   Where            where = Where::Attributes;
   std::string      key;
   bool             negate = false;
   std::vector<Alt> alts;
};

struct Rule
{
   std::string       name;
   std::string       label;
   Action            action = Action::Tag;
   std::vector<Cond> conds;
};

std::vector<Rule> rules;

//! True when `d`'s value at `c` satisfies `c` (negation already applied).
static bool condMatches(const json& d, const Cond& c);

//! True when any alternative matches the scalar `v`.
static bool altsMatch(const std::vector<Alt>& alts, const std::string& v);

//! Render a scalar JSON value for matching. Returns false for a container or
//! null, which never matches.
static bool scalarOf(const json& v, std::string& out);
};
#endif
