//------------------------------------------------------------------------------
// Tests of XrdSecEntityXtra::ResetRequestIdentity, which sets the attributes
// of the request identity back to their login values. A token module sets
// "request.name" and "token.subject" for one authorization, and the next
// authorization must not see them. A security protocol can also set them at
// login, for example sss for a forwarded identity, and these values must stay.
//------------------------------------------------------------------------------

#include <gtest/gtest.h>

#include <string>

#include "XrdSec/XrdSecEntity.hh"
#include "XrdSec/XrdSecEntityAttr.hh"
#include "XrdSec/XrdSecEntityXtra.hh"

namespace {

// The value of the attribute, or "(none)"
std::string Get(const XrdSecEntity &entity, const char *key)
{
   std::string value;
   return entity.eaAPI->Get(key, value) ? value : "(none)";
}

TEST(EntityXtraTest, RemovesTheIdentityOfAnEarlierAuthorization)
{
   XrdSecEntity entity("test");
   XrdSecEntityXtra::ResetRequestIdentity(entity);

   entity.eaAPI->Add("request.name", "mapped", true);
   entity.eaAPI->Add("token.subject", "subject", true);
   XrdSecEntityXtra::ResetRequestIdentity(entity);

   EXPECT_EQ(Get(entity, "request.name"), "(none)");
   EXPECT_EQ(Get(entity, "token.subject"), "(none)");
}

TEST(EntityXtraTest, KeepsTheIdentityOfTheLogin)
{
   XrdSecEntity entity("test");
   entity.eaAPI->Add("request.name", "forwarded", true);
   XrdSecEntityXtra::ResetRequestIdentity(entity);
   EXPECT_EQ(Get(entity, "request.name"), "forwarded");

   entity.eaAPI->Add("request.name", "mapped", true);
   entity.eaAPI->Add("token.subject", "subject", true);
   XrdSecEntityXtra::ResetRequestIdentity(entity);

   EXPECT_EQ(Get(entity, "request.name"), "forwarded");
   EXPECT_EQ(Get(entity, "token.subject"), "(none)");
}

TEST(EntityXtraTest, KeepsTheOtherAttributes)
{
   XrdSecEntity entity("test");
   XrdSecEntityXtra::ResetRequestIdentity(entity);

   entity.eaAPI->Add("other", "value", true);
   XrdSecEntityXtra::ResetRequestIdentity(entity);

   EXPECT_EQ(Get(entity, "other"), "value");
}

TEST(EntityXtraTest, ForgetsTheLoginIdentityWhenTheEntityIsReset)
{
   XrdSecEntity entity("test");
   entity.eaAPI->Add("request.name", "forwarded", true);
   XrdSecEntityXtra::ResetRequestIdentity(entity);

   entity.Reset();
   XrdSecEntityXtra::ResetRequestIdentity(entity);
   entity.eaAPI->Add("request.name", "mapped", true);
   XrdSecEntityXtra::ResetRequestIdentity(entity);

   EXPECT_EQ(Get(entity, "request.name"), "(none)");
}

}
