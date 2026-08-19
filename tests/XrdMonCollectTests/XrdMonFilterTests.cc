//------------------------------------------------------------------------------
// Unit tests for the xrdmoncollect document filter. These drive XrdMonFilter
// against hand-built documents; the decoder-level wiring (emitDoc, dropped
// spans, untouched metrics) is covered in XrdMonCollectTests.cc.
//------------------------------------------------------------------------------

#include <string>

#include "XrdApps/XrdMonCollect/XrdMonFilter.hh"
#include "XrdOuc/XrdOucJson.hh"

#include <gtest/gtest.h>

using json = nlohmann::json;

namespace
{
// A transfer-shaped document with the fields the shortcut table names.
json doc()
{
   return json{
      {"severityText", "INFO"},
      {"resource", {{"server.address", "fst01.cern.ch"},
                    {"server.port",    1095},
                    {"xrootd.server.site", "CERN-PROD"}}},
      {"attributes", {{"event.name",          "xrootd.read"},
                      {"user.name",           "daemon"},
                      {"user.roles",          json::array({"production"})},
                      {"xrootd.auth.method",  "sss"},
                      {"file.path",           "/eos/atlas/proc/tmp.root"},
                      {"user_agent.name",     "eoscp"},
                      {"xrootd.error.code",   3011},
                      {"xrootd.transfer.open_seen", true},
                      {"xrootd.session.recent_files", json::array({json::object()})}}}};
}

// Build a one-rule filter; returns false when the condition is rejected.
bool rule1(XrdMonFilter& f, const char* key, const char* val,
           std::string& err, const char* action = "tag")
{
   std::size_t r = f.AddRule("r");
   if (!f.AddCondition(r, key, val, err)) return false;
   return f.SetAction(r, action, err);
}

// True when a single-condition tag rule matches `d`.
bool matches(const char* key, const char* val, json d = doc())
{
   XrdMonFilter f;
   std::string err;
   EXPECT_TRUE(rule1(f, key, val, err)) << err;
   f.Apply(d);
   return d["attributes"].contains(XrdMonFilter::kLabelKey);
}
}

/******************************************************************************/
/*                        p a t t e r n   s y n t a x                         */
/******************************************************************************/

TEST(XrdMonFilter, ExactMatchIsWholeValue)
{
   EXPECT_TRUE (matches("user", "daemon"));
   EXPECT_FALSE(matches("user", "daem"));      // no accidental substring match
   EXPECT_FALSE(matches("user", "mydaemon"));
   EXPECT_FALSE(matches("user", "Daemon"));    // case-sensitive
}

TEST(XrdMonFilter, AlternativesAreOred)
{
   EXPECT_TRUE(matches("user", "root, daemon"));
   EXPECT_TRUE(matches("user", "  root ,  daemon  "));   // whitespace tolerant
   EXPECT_TRUE(matches("user", "root\ndaemon"));         // repeated INI key
   EXPECT_FALSE(matches("user", "root, nobody"));
}

TEST(XrdMonFilter, GlobCrossesSlashes)
{
   EXPECT_TRUE (matches("path", "/eos/*/proc/*"));
   EXPECT_TRUE (matches("path", "*/proc/*"));
   EXPECT_TRUE (matches("user", "da?mon"));
   EXPECT_FALSE(matches("path", "/eos/*/data/*"));
}

TEST(XrdMonFilter, RegexIsUnanchoredUnlessAnchored)
{
   EXPECT_TRUE (matches("user", "~^daemon$"));
   EXPECT_TRUE (matches("user", "~aemo"));            // unanchored
   EXPECT_FALSE(matches("user", "~^[0-9]+$"));        // "daemon" is not numeric

   // The EOS case this feature exists for: a bare numeric uid.
   json d = doc(); d["attributes"]["user.name"] = "12345";
   EXPECT_TRUE(matches("user", "~^[0-9]+$", d));
   d["attributes"]["user.name"] = "atlas001";
   EXPECT_FALSE(matches("user", "~^[0-9]+$", d));
}

TEST(XrdMonFilter, RegexCommasSurviveListSplitting)
{
   // A brace quantifier and a bracket class both contain a comma that must not
   // be mistaken for an alternative separator.
   json d = doc(); d["attributes"]["user.name"] = "aaa";
   EXPECT_TRUE(matches("user", "~^a{2,4}$", d));
   d["attributes"]["user.name"] = "a,b";
   EXPECT_TRUE(matches("user", "~^[a-z,]+$", d));
}

TEST(XrdMonFilter, NegationInvertsAPresentField)
{
   EXPECT_FALSE(matches("user", "!daemon"));
   EXPECT_TRUE (matches("user", "!root"));
   EXPECT_TRUE (matches("user", "!root, nobody"));   // negates the whole list
   EXPECT_FALSE(matches("user", "!root, daemon"));
}

/******************************************************************************/
/*                       f i e l d   r e s o l u t i o n                      */
/******************************************************************************/

TEST(XrdMonFilter, AbsentFieldNeverMatchesEitherPolarity)
{
   // A server-identity document carries no user at all: an identity rule must
   // not fire on it, and neither must its negation.
   json ident{{"attributes", {{"event.name", "xrootd.server_ident"}}},
              {"resource",   {{"server.address", "fst01.cern.ch"}}}};
   EXPECT_FALSE(matches("user", "daemon", ident));
   EXPECT_FALSE(matches("user", "!daemon", ident));
   EXPECT_FALSE(matches("user", "*", ident));
   // ... while a field it does carry still matches.
   EXPECT_TRUE(matches("server", "fst01.cern.ch", ident));
}

TEST(XrdMonFilter, NullFieldNeverMatches)
{
   json d = doc(); d["attributes"]["user.name"] = nullptr;
   EXPECT_FALSE(matches("user", "*", d));
   EXPECT_FALSE(matches("user", "!daemon", d));
}

TEST(XrdMonFilter, ArrayMatchesAnyElement)
{
   EXPECT_TRUE (matches("role", "production"));
   EXPECT_FALSE(matches("role", "pilot"));

   json d = doc();
   d["attributes"]["user.roles"] = json::array({"pilot", "production"});
   EXPECT_TRUE(matches("role", "production", d));
   d["attributes"]["user.roles"] = json::array();
   EXPECT_FALSE(matches("role", "*", d));
}

TEST(XrdMonFilter, NumbersAndBooleansAreStringified)
{
   EXPECT_TRUE(matches("attributes.xrootd.error.code", "3011"));
   EXPECT_TRUE(matches("attributes.xrootd.transfer.open_seen", "true"));
   EXPECT_TRUE(matches("resource.server.port", "1095"));
   EXPECT_TRUE(matches("resource.server.port", "~^10"));
   EXPECT_FALSE(matches("attributes.xrootd.error.code", "3012"));
}

TEST(XrdMonFilter, ObjectsAndObjectArraysNeverMatch)
{
   EXPECT_FALSE(matches("attributes.xrootd.session.recent_files", "*"));
   json d = doc();
   d["attributes"]["nested"] = json{{"a", 1}};
   EXPECT_FALSE(matches("attributes.nested", "*", d));
}

TEST(XrdMonFilter, ResourceAndTopLevelKeys)
{
   EXPECT_TRUE(matches("server", "fst01.cern.ch"));
   EXPECT_TRUE(matches("site", "CERN-PROD"));
   EXPECT_TRUE(matches("severity", "INFO"));        // top-level severityText
   EXPECT_TRUE(matches("resource.xrootd.server.site", "CERN-PROD"));
   EXPECT_TRUE(matches("attributes.user.name", "daemon"));
   // A resource key must not be found under attributes, or vice versa.
   EXPECT_FALSE(matches("attributes.server.address", "fst01.cern.ch"));
}

TEST(XrdMonFilter, ConditionsAreAnded)
{
   XrdMonFilter f;
   std::string err;
   std::size_t r = f.AddRule("r");
   ASSERT_TRUE(f.AddCondition(r, "user", "daemon", err)) << err;
   ASSERT_TRUE(f.AddCondition(r, "authprot", "gsi", err)) << err;   // no match
   json d = doc();
   f.Apply(d);
   EXPECT_FALSE(d["attributes"].contains(XrdMonFilter::kLabelKey));

   XrdMonFilter g;
   std::size_t q = g.AddRule("r");
   ASSERT_TRUE(g.AddCondition(q, "user", "daemon", err)) << err;
   ASSERT_TRUE(g.AddCondition(q, "authprot", "sss", err)) << err;
   json e = doc();
   g.Apply(e);
   EXPECT_TRUE(e["attributes"].contains(XrdMonFilter::kLabelKey));
}

/******************************************************************************/
/*                       l a b e l s   a n d   a c t i o n s                  */
/******************************************************************************/

TEST(XrdMonFilter, LabelDefaultsToRuleNameAndIsOverridable)
{
   XrdMonFilter f;
   std::string err;
   std::size_t a = f.AddRule("eos-internal");
   ASSERT_TRUE(f.AddCondition(a, "user", "daemon", err)) << err;

   json d = doc();
   EXPECT_TRUE(f.Apply(d));
   EXPECT_EQ(d["attributes"][XrdMonFilter::kLabelKey],
             json::array({"eos-internal"}));

   XrdMonFilter g;
   std::size_t b = g.AddRule("eos-internal");
   ASSERT_TRUE(g.AddCondition(b, "user", "daemon", err)) << err;
   g.SetLabel(b, "internal");
   json e = doc();
   EXPECT_TRUE(g.Apply(e));
   EXPECT_EQ(e["attributes"][XrdMonFilter::kLabelKey], json::array({"internal"}));
}

TEST(XrdMonFilter, LabelsAreUnionedSortedAndDeduplicated)
{
   XrdMonFilter f;
   std::string err;
   std::size_t a = f.AddRule("zebra");
   ASSERT_TRUE(f.AddCondition(a, "user", "daemon", err)) << err;
   std::size_t b = f.AddRule("alpha");
   ASSERT_TRUE(f.AddCondition(b, "authprot", "sss", err)) << err;
   std::size_t c = f.AddRule("dup");           // same label as "alpha"
   ASSERT_TRUE(f.AddCondition(c, "app", "eoscp", err)) << err;
   f.SetLabel(c, "alpha");

   json d = doc();
   EXPECT_TRUE(f.Apply(d));
   EXPECT_EQ(d["attributes"][XrdMonFilter::kLabelKey],
             json::array({"alpha", "zebra"}));
}

TEST(XrdMonFilter, DropSuppressesTheDocument)
{
   XrdMonFilter f;
   std::string err;
   ASSERT_TRUE(rule1(f, "authprot", "sss", err, "drop")) << err;
   json d = doc();
   EXPECT_FALSE(f.Apply(d));
   // A dropped document is still labelled, so a debug sink shows the reason.
   EXPECT_TRUE(d["attributes"].contains(XrdMonFilter::kLabelKey));
}

TEST(XrdMonFilter, NonMatchingDropLeavesTheDocumentAlone)
{
   XrdMonFilter f;
   std::string err;
   ASSERT_TRUE(rule1(f, "authprot", "gsi", err, "drop")) << err;
   json d = doc();
   EXPECT_TRUE(f.Apply(d));
   EXPECT_FALSE(d["attributes"].contains(XrdMonFilter::kLabelKey));
}

TEST(XrdMonFilter, KeepBeatsDropRegardlessOfOrder)
{
   std::string err;
   for (int order = 0; order < 2; order++)
       {XrdMonFilter f;
        auto addDrop = [&]{std::size_t r = f.AddRule("drop-sss");
                           EXPECT_TRUE(f.AddCondition(r, "authprot", "sss", err));
                           EXPECT_TRUE(f.SetAction(r, "drop", err));};
        auto addKeep = [&]{std::size_t r = f.AddRule("keep-prod");
                           EXPECT_TRUE(f.AddCondition(r, "role", "production", err));
                           EXPECT_TRUE(f.SetAction(r, "keep", err));};
        if (order) {addDrop(); addKeep();} else {addKeep(); addDrop();}

        json d = doc();
        EXPECT_TRUE(f.Apply(d)) << "keep must win in order " << order;
        EXPECT_EQ(d["attributes"][XrdMonFilter::kLabelKey],
                  json::array({"drop-sss", "keep-prod"}));
       }
}

TEST(XrdMonFilter, DropBeatsTag)
{
   XrdMonFilter f;
   std::string err;
   std::size_t a = f.AddRule("tag-it");
   ASSERT_TRUE(f.AddCondition(a, "user", "daemon", err)) << err;
   std::size_t b = f.AddRule("drop-it");
   ASSERT_TRUE(f.AddCondition(b, "authprot", "sss", err)) << err;
   ASSERT_TRUE(f.SetAction(b, "drop", err)) << err;

   json d = doc();
   EXPECT_FALSE(f.Apply(d));
}

TEST(XrdMonFilter, EmptyFilterKeepsEverythingUntouched)
{
   XrdMonFilter f;
   json d = doc();
   EXPECT_TRUE(f.Empty());
   EXPECT_TRUE(f.Apply(d));
   EXPECT_FALSE(d["attributes"].contains(XrdMonFilter::kLabelKey));
}

TEST(XrdMonFilter, CountsRulesByAction)
{
   XrdMonFilter f;
   std::string err;
   for (const char* a : {"tag", "drop", "drop", "keep"})
       {std::size_t r = f.AddRule(a);
        ASSERT_TRUE(f.AddCondition(r, "user", "daemon", err)) << err;
        ASSERT_TRUE(f.SetAction(r, a, err)) << err;
       }
   EXPECT_EQ(f.Size(), 4u);
   EXPECT_EQ(f.Count(XrdMonFilter::Action::Tag),  1u);
   EXPECT_EQ(f.Count(XrdMonFilter::Action::Drop), 2u);
   EXPECT_EQ(f.Count(XrdMonFilter::Action::Keep), 1u);
}

/******************************************************************************/
/*                            v a l i d a t i o n                             */
/******************************************************************************/

TEST(XrdMonFilter, RejectsUnknownKeyAndNamesTheAlternatives)
{
   XrdMonFilter f;
   std::string err;
   std::size_t r = f.AddRule("r");
   EXPECT_FALSE(f.AddCondition(r, "usr", "alice", err));
   EXPECT_NE(err.find("usr"), std::string::npos);
   EXPECT_NE(err.find("user"), std::string::npos);   // suggests the real names
}

TEST(XrdMonFilter, RejectsRawPathOutsideResourceOrAttributes)
{
   XrdMonFilter f;
   std::string err;
   std::size_t r = f.AddRule("r");
   EXPECT_FALSE(f.AddCondition(r, "foo.bar", "x", err));
   EXPECT_FALSE(f.AddCondition(r, "attributes.", "x", err));
   EXPECT_TRUE (f.AddCondition(r, "attributes.a.b", "x", err)) << err;
}

TEST(XrdMonFilter, RejectsUncompilableRegex)
{
   XrdMonFilter f;
   std::string err;
   std::size_t r = f.AddRule("r");
   EXPECT_FALSE(f.AddCondition(r, "user", "~([", err));
   EXPECT_NE(err.find("regular expression"), std::string::npos);
}

TEST(XrdMonFilter, RejectsEmptyValue)
{
   XrdMonFilter f;
   std::string err;
   std::size_t r = f.AddRule("r");
   EXPECT_FALSE(f.AddCondition(r, "user", "", err));
   EXPECT_FALSE(f.AddCondition(r, "user", "  ", err));
   EXPECT_FALSE(f.AddCondition(r, "user", " , ", err));
}

TEST(XrdMonFilter, RejectsMisindentedKeySwallowedIntoAValue)
{
   // inih appends an indented line to the preceding key's value, so a
   // git-config-style rule whose keys are indented silently loses every key
   // after the first. Catch the swallowed text rather than match nothing.
   XrdMonFilter f;
   std::string err;
   std::size_t r = f.AddRule("r");
   EXPECT_FALSE(f.AddCondition(r, "user", "daemon\nauthprot = sss", err));
   EXPECT_NE(err.find("column 1"), std::string::npos);
   // A value that merely contains '=' is fine.
   EXPECT_TRUE(f.AddCondition(r, "appinfo", "*eos.app=fuse*", err)) << err;
}

TEST(XrdMonFilter, RejectsBadAction)
{
   XrdMonFilter f;
   std::string err;
   std::size_t r = f.AddRule("r");
   EXPECT_FALSE(f.SetAction(r, "nope", err));
   EXPECT_TRUE(f.SetAction(r, "TAG", err)) << err;    // case-insensitive
   EXPECT_TRUE(f.SetAction(r, " drop ", err)) << err;
}

TEST(XrdMonFilter, ValidateRejectsRuleWithoutConditions)
{
   XrdMonFilter f;
   std::string err;
   f.AddRule("catch-all");
   EXPECT_FALSE(f.Validate(err));
   EXPECT_NE(err.find("catch-all"), std::string::npos);
}

TEST(XrdMonFilter, ValidateRejectsTagRuleWithEmptyLabel)
{
   XrdMonFilter f;
   std::string err;
   std::size_t r = f.AddRule("r");
   ASSERT_TRUE(f.AddCondition(r, "user", "daemon", err)) << err;
   f.SetLabel(r, "");
   EXPECT_FALSE(f.Validate(err));

   // An unlabelled drop rule is legitimate: suppress without annotating.
   XrdMonFilter g;
   std::size_t q = g.AddRule("r");
   ASSERT_TRUE(g.AddCondition(q, "user", "daemon", err)) << err;
   ASSERT_TRUE(g.SetAction(q, "drop", err)) << err;
   g.SetLabel(q, "");
   EXPECT_TRUE(g.Validate(err)) << err;
   json d = doc();
   EXPECT_FALSE(g.Apply(d));
   EXPECT_FALSE(d["attributes"].contains(XrdMonFilter::kLabelKey));
}

TEST(XrdMonFilter, ShortcutTableResolves)
{
   XrdMonFilter::Where w;
   std::string k;
   ASSERT_TRUE(XrdMonFilter::ResolveKey("user", w, k));
   EXPECT_EQ(k, "user.name");
   EXPECT_TRUE(w == XrdMonFilter::Where::Attributes);
   ASSERT_TRUE(XrdMonFilter::ResolveKey("server", w, k));
   EXPECT_EQ(k, "server.address");
   EXPECT_TRUE(w == XrdMonFilter::Where::Resource);
   ASSERT_TRUE(XrdMonFilter::ResolveKey("severity", w, k));
   EXPECT_EQ(k, "severityText");
   EXPECT_TRUE(w == XrdMonFilter::Where::Top);
   EXPECT_FALSE(XrdMonFilter::ResolveKey("nosuchkey", w, k));
   // Every shortcut must resolve, so the table cannot drift out of sync.
   for (const auto& sc : XrdMonFilter::Shortcuts())
       EXPECT_TRUE(XrdMonFilter::ResolveKey(sc.first, w, k)) << sc.first;
}
