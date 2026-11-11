//------------------------------------------------------------------------------
// Tests of the server side of the unix security provider.
//
// An incomplete first authentication message must not become a successful
// login. For a credential of a few bytes, the providers switched the identity
// to "host" and returned zero, which the native server
// (XrdXrootdProtocol::do_Auth) treats as a completed login. The gsi, krb5 and
// pwd providers had the same branch. The end-to-end tests in
// tests/end-to-end/sec check it for gsi and krb5; pwd has no test server yet.
//
// The size of the message bounds the parsing, so a message needs no
// terminator at its end. The tests check that the messages of the clients of
// XRootD, JAliEn and dCache still log in.
//------------------------------------------------------------------------------

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "XrdNet/XrdNetAddrInfo.hh"
#include "XrdOuc/XrdOucErrInfo.hh"
#include "XrdSec/XrdSecInterface.hh"

// The provider exposes only this C factory; the class is private to its source.
extern "C" XrdSecProtocol *XrdSecProtocolunixObject(char mode,
                                                    const char *hostname,
                                                    XrdNetAddrInfo &endPoint,
                                                    const char *parms,
                                                    XrdOucErrInfo *erp);

namespace {

struct Login {
   int rc;            // zero for a completed login
   std::string prot;  // the protocol of the identity
   std::string name;  // the user name of the identity
   std::string grps;  // the groups of the identity
};

// Run the server side of the provider on the message.
Login Authenticate(const std::string &message)
{
   XrdNetAddrInfo endPoint;
   XrdSecProtocol *prot =
      XrdSecProtocolunixObject('s', "localhost", endPoint, "", nullptr);
   EXPECT_NE(prot, nullptr);
   if (!prot) return {1, "", "", ""};

   // Set the fields rather than pass the buffer to the constructor, which
   // would take ownership and free it.
   std::vector<char> buffer(message.begin(), message.end());
   XrdSecCredentials cred;
   cred.buffer = buffer.data();
   cred.size = static_cast<int>(buffer.size());
   XrdSecParameters *parms = nullptr;
   XrdOucErrInfo einfo;

   Login login;
   login.rc = prot->Authenticate(&cred, &parms, &einfo);
   login.prot.assign(prot->Entity.prot,
                     strnlen(prot->Entity.prot, sizeof(prot->Entity.prot)));
   if (prot->Entity.name) login.name = prot->Entity.name;
   if (prot->Entity.grps) login.grps = prot->Entity.grps;
   prot->Delete();
   return login;
}

// The message of the dCache client (xrootd4j, UnixClientAuthenticationHandler):
// a bucket buffer as for gsi, with the user name in a credentials bucket.
std::string DcacheMessage(const std::string &uname)
{
   std::string m;
   auto int32 = [&m](uint32_t v) {
      for (int shift = 24; shift >= 0; shift -= 8)
         m += static_cast<char>((v >> shift) & 0xff);
   };

   m += "unix"; int32(1001); int32(3001); int32(28 + uname.size());
   m += "unix"; int32(1001); int32(3010); int32(uname.size());
   m += uname;  int32(0);    int32(0);
   return m;
}

TEST(ProtocolunixTest, IncompleteMessageIsNotALogin)
{
   const Login login = Authenticate(std::string("un\0", 3));

   EXPECT_NE(login.rc, 0) << "an incomplete message was accepted as a login";
   EXPECT_NE(login.prot, "host");
}

TEST(ProtocolunixTest, MessageOfOnlyTheNameOfTheProtocolIsNotALogin)
{
   // "unix" is 4 bytes: the protocol id without its terminator.
   const Login login = Authenticate("unix");

   EXPECT_NE(login.rc, 0);
   EXPECT_NE(login.prot, "host");
}

TEST(ProtocolunixTest, MessageWithoutTerminatorLogsIn)
{
   // The buffer of the test ends at the last byte of the message, so a
   // sanitizer finds a read after the identity.
   const Login login = Authenticate(std::string("unix\0alice users", 16));

   EXPECT_EQ(login.rc, 0);
   EXPECT_EQ(login.name, "alice");
   EXPECT_EQ(login.grps, "users");
}

TEST(ProtocolunixTest, MessageWithoutIdentityLogsInWithAnEmptyName)
{
   // "unix\0" is the shortest complete message: the protocol id and its
   // terminator, with an empty identity.
   const Login login = Authenticate(std::string("unix\0", 5));

   EXPECT_EQ(login.rc, 0);
   EXPECT_EQ(login.prot, "unix");
   EXPECT_EQ(login.name, "");
}

TEST(ProtocolunixTest, MessageOfTheXRootDClientLogsIn)
{
   XrdNetAddrInfo endPoint;
   XrdSecProtocol *client =
      XrdSecProtocolunixObject('c', "localhost", endPoint, "", nullptr);
   ASSERT_NE(client, nullptr);
   XrdSecCredentials *cred = client->getCredentials();
   ASSERT_NE(cred, nullptr);
   const std::string message(cred->buffer, cred->size);
   delete cred;
   client->Delete();

   const Login login = Authenticate(message);

   EXPECT_EQ(login.rc, 0);
   EXPECT_EQ(login.prot, "unix");
   EXPECT_FALSE(login.name.empty());
}

// JAliEn (XrootdClientSecurityProtocolUnix) sends "unix\0<user> <group>\0".
TEST(ProtocolunixTest, MessageOfTheJAliEnClientLogsIn)
{
   const Login login = Authenticate(std::string("unix\0alice users\0", 17));

   EXPECT_EQ(login.rc, 0);
   EXPECT_EQ(login.name, "alice");
   EXPECT_EQ(login.grps, "users");
}

// The parser sees no name in the message of dCache, which logs in without one.
TEST(ProtocolunixTest, MessageOfTheDcacheClientLogsIn)
{
   const Login login = Authenticate(DcacheMessage("alice"));

   EXPECT_EQ(login.rc, 0);
   EXPECT_EQ(login.prot, "unix");
}

}
