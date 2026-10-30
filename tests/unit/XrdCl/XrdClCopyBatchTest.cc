// Copyright (c) 2026 by European Organization for Nuclear Research (CERN)
// SPDX-License-Identifier: LGPL-3.0-or-later

#include "XrdCl/XrdClCopyProcess.hh"
#include <gtest/gtest.h>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <fstream>
#include <mutex>
#include <set>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

using namespace XrdCl;

class CopyBatchTest: public ::testing::Test
{
  protected:
    void SetUp() override
    {
      char pattern[] = "/tmp/xrdcp-batch-unit-XXXXXX";
      char *directory = mkdtemp(pattern);
      ASSERT_NE(directory, nullptr);
      root = directory;
    }

    void TearDown() override
    {
      for(const auto &path: paths) unlink(path.c_str());
      for(const auto &path: directories) rmdir(path.c_str());
      rmdir(root.c_str());
    }

    std::string Path(const std::string &name)
    {
      std::string path = root + '/' + name;
      paths.insert(path);
      return path;
    }

    std::string Input(const std::string &name)
    {
      std::string path = Path(name);
      std::ofstream output(path);
      output << "native copy batch payload\n";
      return path;
    }

    PropertyList Job(const std::string &source, const std::string &target)
    {
      PropertyList properties;
      properties.Set("source", source);
      properties.Set("target", target);
      return properties;
    }

    void Configure(CopyProcess &process, int parallel, bool stop = false)
    {
      PropertyList configuration;
      configuration.Set("jobType", "configuration");
      configuration.Set("parallel", parallel);
      configuration.Set("continueOnPrepareError", true);
      configuration.Set("stopOnFailure", stop);
      ASSERT_TRUE(process.AddJob(configuration, nullptr).IsOK());
    }

    std::string State(const PropertyList &result)
    {
      std::string state;
      EXPECT_TRUE(result.Get("jobState", state));
      return state;
    }

    std::string root;
    std::set<std::string> paths;
    std::vector<std::string> directories;
};

class BatchProgress: public CopyProgressHandler
{
  public:
    enum Policy { Overlap, HoldFirst, HoldSecond };
    explicit BatchProgress(Policy selected): policy(selected) {}

    void BeginJob(uint32_t job, uint32_t, const URL *, const URL *) override
    {
      std::unique_lock<std::mutex> lock(mutex);
      begun.push_back(job);
      maximum = std::max(maximum, ++active);
      if(job == 1) firstStarted = true;
      if(job == 2) secondStarted = true;
      if(active == 2) overlapped = true;
      condition.notify_all();
      if(policy == Overlap && job <= 2)
        Wait(lock, [this] { return overlapped; });
      if(policy == HoldFirst && job == 1)
        Wait(lock, [this] { return secondFinished; });
      if(policy == HoldFirst && job == 2)
        Wait(lock, [this] { return firstStarted; });
      if(policy == HoldSecond && job == 1)
        Wait(lock, [this] { return secondStarted; });
      if(policy == HoldSecond && job == 2)
        Wait(lock, [this] { return firstFinished; });
    }

    void EndJob(uint32_t job, const PropertyList *) override
    {
      std::lock_guard<std::mutex> lock(mutex);
      --active;
      if(job == 1) firstFinished = true;
      if(job == 2) secondFinished = true;
      condition.notify_all();
    }

    template<class Predicate>
    void Wait(std::unique_lock<std::mutex> &lock, Predicate predicate)
    {
      EXPECT_TRUE(condition.wait_for(lock, std::chrono::seconds(5), predicate));
    }

    Policy policy;
    std::mutex mutex;
    std::condition_variable condition;
    std::vector<uint32_t> begun;
    unsigned active = 0, maximum = 0;
    bool firstStarted = false, secondStarted = false;
    bool firstFinished = false, secondFinished = false, overlapped = false;
};

TEST_F(CopyBatchTest, ParallelJobsHaveBoundedOverlap)
{
  CopyProcess process;
  std::vector<PropertyList> results(5);
  for(size_t index = 0; index < results.size(); ++index)
    ASSERT_TRUE(process.AddJob(Job(Input("source" + std::to_string(index)),
                                  Path("target" + std::to_string(index))),
                               &results[index]).IsOK());
  Configure(process, 2);
  ASSERT_TRUE(process.Prepare().IsOK());
  BatchProgress progress(BatchProgress::Overlap);
  EXPECT_TRUE(process.Run(&progress).IsOK());
  EXPECT_TRUE(progress.overlapped);
  EXPECT_EQ(progress.maximum, 2u);
  EXPECT_EQ(progress.active, 0u);
  EXPECT_EQ(progress.begun.size(), results.size());
  for(const auto &result: results) EXPECT_EQ(State(result), "succeeded");
}

TEST_F(CopyBatchTest, ContinueAfterPreparationFailure)
{
  CopyProcess process;
  PropertyList failed, copied;
  ASSERT_TRUE(process.AddJob(Job("", Path("invalid")), &failed).IsOK());
  ASSERT_TRUE(process.AddJob(Job(Input("input"), Path("output")), &copied).IsOK());
  Configure(process, 2);
  ASSERT_TRUE(process.Prepare().IsOK());
  EXPECT_EQ(State(failed), "failed");
  auto status = process.Run(nullptr);
  EXPECT_FALSE(status.IsOK());
  EXPECT_EQ(status.ToStr(), failed.Get<XRootDStatus>("status").ToStr());
  EXPECT_EQ(State(copied), "succeeded");
  EXPECT_EQ(access(Path("output").c_str(), F_OK), 0);
}

TEST_F(CopyBatchTest, PreparationFailureStopsEveryUnstartedJob)
{
  CopyProcess process;
  PropertyList earlier, failed, later;
  ASSERT_TRUE(process.AddJob(Job(Input("input"), Path("earlier")), &earlier).IsOK());
  ASSERT_TRUE(process.AddJob(Job("", Path("invalid")), &failed).IsOK());
  ASSERT_TRUE(process.AddJob(Job(Input("next"), Path("later")), &later).IsOK());
  Configure(process, 2, true);
  ASSERT_TRUE(process.Prepare().IsOK());
  auto status = process.Run(nullptr);
  EXPECT_FALSE(status.IsOK());
  EXPECT_EQ(status.ToStr(), failed.Get<XRootDStatus>("status").ToStr());
  EXPECT_EQ(State(earlier), "skipped");
  EXPECT_EQ(State(failed), "failed");
  EXPECT_EQ(State(later), "skipped");
  EXPECT_NE(access(Path("earlier").c_str(), F_OK), 0);
  EXPECT_NE(access(Path("later").c_str(), F_OK), 0);
}

TEST_F(CopyBatchTest, RuntimeFailureStopsReplacementAndFinishesActiveJob)
{
  CopyProcess process;
  PropertyList failed, active, skipped;
  ASSERT_TRUE(process.AddJob(Job(Path("missing"), Path("failed")), &failed).IsOK());
  ASSERT_TRUE(process.AddJob(Job(Input("input"), Path("active")), &active).IsOK());
  ASSERT_TRUE(process.AddJob(Job(Input("next"), Path("skipped")), &skipped).IsOK());
  Configure(process, 2, true);
  ASSERT_TRUE(process.Prepare().IsOK());
  BatchProgress progress(BatchProgress::HoldSecond);
  auto status = process.Run(&progress);
  EXPECT_FALSE(status.IsOK());
  EXPECT_EQ(State(failed), "failed");
  EXPECT_EQ(State(active), "succeeded");
  EXPECT_EQ(State(skipped), "skipped");
  EXPECT_EQ(access(Path("active").c_str(), F_OK), 0);
  EXPECT_NE(access(Path("skipped").c_str(), F_OK), 0);
}

TEST_F(CopyBatchTest, RuntimeFailuresReturnInputOrder)
{
  CopyProcess process;
  PropertyList first, second;
  ASSERT_TRUE(process.AddJob(Job(Input("input"), Input("existing")), &first).IsOK());
  ASSERT_TRUE(process.AddJob(Job(Path("missing"), Path("second")), &second).IsOK());
  Configure(process, 2);
  ASSERT_TRUE(process.Prepare().IsOK());
  BatchProgress progress(BatchProgress::HoldFirst);
  auto status = process.Run(&progress);
  EXPECT_FALSE(status.IsOK());
  EXPECT_EQ(State(first), "failed");
  EXPECT_EQ(State(second), "failed");
  EXPECT_EQ(status.ToStr(), first.Get<XRootDStatus>("status").ToStr());
  EXPECT_NE(first.Get<XRootDStatus>("status").errNo,
            second.Get<XRootDStatus>("status").errNo);
}

TEST_F(CopyBatchTest, RepeatedPrepareKeepsDirectoryTargetStable)
{
  std::string target = root + "/directory";
  ASSERT_EQ(mkdir(target.c_str(), 0700), 0);
  directories.push_back(target);
  paths.insert(target + "/input");
  CopyProcess process;
  PropertyList result;
  auto job = Job(Input("input"), target);
  job.Set("targetIsDir", true);
  ASSERT_TRUE(process.AddJob(job, &result).IsOK());
  Configure(process, 1);
  ASSERT_TRUE(process.Prepare().IsOK());
  ASSERT_TRUE(process.Prepare().IsOK());
  EXPECT_TRUE(process.Run(nullptr).IsOK());
  EXPECT_EQ(access((target + "/input").c_str(), F_OK), 0);
}

TEST_F(CopyBatchTest, ConfigurationBeforeJobsPreservesInputIds)
{
  CopyProcess process;
  Configure(process, 2);
  PropertyList first, second;
  ASSERT_TRUE(process.AddJob(Job(Input("first"), Path("first-target")), &first).IsOK());
  ASSERT_TRUE(process.AddJob(Job(Input("second"), Path("second-target")), &second).IsOK());
  ASSERT_TRUE(process.Prepare().IsOK());
  BatchProgress progress(BatchProgress::Overlap);
  EXPECT_TRUE(process.Run(&progress).IsOK());
  std::sort(progress.begun.begin(), progress.begun.end());
  EXPECT_EQ(progress.begun, std::vector<uint32_t>({1, 2}));
}
