//------------------------------------------------------------------------------
// Tests of the user which XrdThrottleManager::GetUserInfo derives from a
// security entity. The user selects the fair share and the per-user limits of
// open files and connections.
//
// The protocols remove the token attributes "request.name" and "token.subject"
// before each request. A module can also set an attribute to an empty value.
// An empty attribute must count as no attribute: otherwise all the users of
// one organization share one user "<vorg>:", and a mapped name no longer
// selects the user.
//------------------------------------------------------------------------------

#include "XrdThrottle/XrdThrottleManager.hh"
#include "XrdOuc/XrdOucTrace.hh"
#include "XrdSec/XrdSecEntity.hh"
#include "XrdSec/XrdSecEntityAttr.hh"
#include "XrdSys/XrdSysError.hh"
#include "XrdSys/XrdSysLogger.hh"

#include <gtest/gtest.h>

#include <string>
#include <unistd.h>

class ManagerUserInfoTest : public ::testing::Test {
protected:
    // Init() starts threads, which GetUserInfo does not need
    ManagerUserInfoTest()
        : m_logger(STDERR_FILENO, 0), m_log(&m_logger, "ThrottleTest"),
          m_trace(&m_log), m_manager(&m_log, &m_trace) {}

    std::string User(const XrdSecEntity &entity) {
        return std::get<0>(m_manager.GetUserInfo(&entity));
    }

    XrdSysLogger m_logger;
    XrdSysError m_log;
    XrdOucTrace m_trace;
    XrdThrottleManager m_manager;
};

TEST_F(ManagerUserInfoTest, NameIsTheUserWithoutTokenAttributes) {
    XrdSecEntity entity;
    entity.name = const_cast<char *>("alice");
    entity.vorg = const_cast<char *>("cms");

    EXPECT_EQ(User(entity), "alice");
}

TEST_F(ManagerUserInfoTest, SubjectWithOrganizationIsTheUser) {
    XrdSecEntity entity;
    entity.name = const_cast<char *>("alice");
    entity.vorg = const_cast<char *>("cms");
    entity.eaAPI->Add("token.subject", "subject1", true);

    EXPECT_EQ(User(entity), "cms:subject1");
}

TEST_F(ManagerUserInfoTest, EmptySubjectDoesNotMakeTheOrganizationTheUser) {
    XrdSecEntity entity;
    entity.name = const_cast<char *>("alice");
    entity.vorg = const_cast<char *>("cms");
    entity.eaAPI->Add("token.subject", "", true);

    EXPECT_EQ(User(entity), "alice");
}

TEST_F(ManagerUserInfoTest, EmptySubjectDoesNotHideTheMappedName) {
    XrdSecEntity entity;
    entity.name = const_cast<char *>("alice");
    entity.eaAPI->Add("token.subject", "", true);
    entity.eaAPI->Add("request.name", "mapped", true);

    EXPECT_EQ(User(entity), "mapped");
}

TEST_F(ManagerUserInfoTest, EmptyAttributesGiveTheName) {
    XrdSecEntity entity;
    entity.name = const_cast<char *>("alice");
    entity.eaAPI->Add("token.subject", "", true);
    entity.eaAPI->Add("request.name", "", true);

    EXPECT_EQ(User(entity), "alice");
}
