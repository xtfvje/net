// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "net/url_request/url_request_file_job.h"

#include <memory>

#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/logging.h"
#include "base/run_loop.h"
#include "base/strings/stringprintf.h"
#include "base/threading/sequenced_worker_pool.h"
#include "base/threading/thread_task_runner_handle.h"
#include "net/base/filename_util.h"
#include "net/url_request/url_request.h"
#include "net/url_request/url_request_test_util.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace net {

namespace {

// A URLRequestFileJob for testing values passed to OnSeekComplete and
// OnReadComplete.
class TestURLRequestFileJob : public URLRequestFileJob {
 public:
  // |seek_position| will be set to the value passed in to OnSeekComplete.
  // |observed_content| will be set to the concatenated data from all calls to
  // OnReadComplete.
  TestURLRequestFileJob(URLRequest* request,
                        NetworkDelegate* network_delegate,
                        const base::FilePath& file_path,
                        const scoped_refptr<base::TaskRunner>& file_task_runner,
                        int64_t* seek_position,
                        std::string* observed_content)
      : URLRequestFileJob(request,
                          network_delegate,
                          file_path,
                          file_task_runner),
        seek_position_(seek_position),
        observed_content_(observed_content) {
    *seek_position_ = 0;
    observed_content_->clear();
  }

  ~TestURLRequestFileJob() override {}

 protected:
  void OnSeekComplete(int64_t result) override {
    ASSERT_EQ(*seek_position_, 0);
    *seek_position_ = result;
  }

  void OnReadComplete(IOBuffer* buf, int result) override {
    observed_content_->append(std::string(buf->data(), result));
  }

  int64_t* const seek_position_;
  std::string* const observed_content_;
};

// A URLRequestJobFactory that will return TestURLRequestFileJob instances for
// file:// scheme URLs.  Can only be used to create a single job.
class TestJobFactory : public URLRequestJobFactory {
 public:
  TestJobFactory(const base::FilePath& path,
                 int64_t* seek_position,
                 std::string* observed_content)
      : path_(path),
        seek_position_(seek_position),
        observed_content_(observed_content) {
    CHECK(seek_position_);
    CHECK(observed_content_);
  }

  ~TestJobFactory() override {}

  URLRequestJob* MaybeCreateJobWithProtocolHandler(
      const std::string& scheme,
      URLRequest* request,
      NetworkDelegate* network_delegate) const override {
    CHECK(seek_position_);
    CHECK(observed_content_);
    URLRequestJob* job = new TestURLRequestFileJob(
        request, network_delegate, path_, base::ThreadTaskRunnerHandle::Get(),
        seek_position_, observed_content_);
    seek_position_ = nullptr;
    observed_content_ = nullptr;
    return job;
  }

  URLRequestJob* MaybeInterceptRedirect(URLRequest* request,
                                        NetworkDelegate* network_delegate,
                                        const GURL& location) const override {
    return nullptr;
  }

  URLRequestJob* MaybeInterceptResponse(
      URLRequest* request,
      NetworkDelegate* network_delegate) const override {
    return nullptr;
  }

  bool IsHandledProtocol(const std::string& scheme) const override {
    return scheme == "file";
  }

  bool IsHandledURL(const GURL& url) const override {
    return IsHandledProtocol(url.scheme());
  }

  bool IsSafeRedirectTarget(const GURL& location) const override {
    return false;
  }

 private:
  const base::FilePath path_;

  // These are mutable because MaybeCreateJobWithProtocolHandler is const.
  mutable int64_t* seek_position_;
  mutable std::string* observed_content_;
};

// Helper function to create a file in |directory| filled with
// |content|. Returns true on succes and fills in |path| with the full path to
// the file.
bool CreateTempFileWithContent(const std::string& content,
                               const base::ScopedTempDir& directory,
                               base::FilePath* path) {
  if (!directory.IsValid())
    return false;

  if (!base::CreateTemporaryFileInDir(directory.path(), path))
    return false;

  return base::WriteFile(*path, content.c_str(), content.length());
}

// A simple holder for start/end used in http range requests.
struct Range {
  int start;
  int end;

  Range() {
    start = 0;
    end = 0;
  }

  Range(int start, int end) {
    this->start = start;
    this->end = end;
  }
};

// A superclass for tests of the OnSeekComplete / OnReadComplete functions of
// URLRequestFileJob.
class URLRequestFileJobEventsTest : public testing::Test {
 public:
  URLRequestFileJobEventsTest();

 protected:
  // This creates a file with |content| as the contents, and then creates and
  // runs a URLRequestFileJobWithCallbacks job to get the contents out of it,
  // and makes sure that the callbacks observed the correct bytes. If a Range
  // is provided, this function will add the appropriate Range http header to
  // the request and verify that only the bytes in that range (inclusive) were
  // observed.
  void RunRequest(const std::string& content, const Range* range);

  TestURLRequestContext context_;
  TestDelegate delegate_;
};

URLRequestFileJobEventsTest::URLRequestFileJobEventsTest() {}

void URLRequestFileJobEventsTest::RunRequest(const std::string& content,
                                             const Range* range) {
  base::ScopedTempDir directory;
  ASSERT_TRUE(directory.CreateUniqueTempDir());
  base::FilePath path;
  ASSERT_TRUE(CreateTempFileWithContent(content, directory, &path));

  {
    int64_t seek_position;
    std::string observed_content;
    TestJobFactory factory(path, &seek_position, &observed_content);
    context_.set_job_factory(&factory);

    std::unique_ptr<URLRequest> request(context_.CreateRequest(
        FilePathToFileURL(path), DEFAULT_PRIORITY, &delegate_));
    if (range) {
      ASSERT_GE(range->start, 0);
      ASSERT_GE(range->end, 0);
      ASSERT_LE(range->start, range->end);
      ASSERT_LT(static_cast<unsigned int>(range->end), content.length());
      std::string range_value =
          base::StringPrintf("bytes=%d-%d", range->start, range->end);
      request->SetExtraRequestHeaderByName(HttpRequestHeaders::kRange,
                                           range_value, true /*overwrite*/);
    }
    request->Start();

    base::RunLoop().Run();

    EXPECT_FALSE(delegate_.request_failed());
    int expected_length =
        range ? (range->end - range->start + 1) : content.length();
    EXPECT_EQ(delegate_.bytes_received(), expected_length);

    std::string expected_content;
    if (range) {
      expected_content.insert(0, content, range->start, expected_length);
    } else {
      expected_content = content;
    }
    EXPECT_TRUE(delegate_.data_received() == expected_content);

    EXPECT_EQ(seek_position, range ? range->start : 0);
    EXPECT_EQ(expected_content, observed_content);
  }

  base::RunLoop().RunUntilIdle();
}

// Helper function to make a character array filled with |size| bytes of
// test content.
std::string MakeContentOfSize(int size) {
  EXPECT_GE(size, 0);
  std::string result;
  result.reserve(size);
  for (int i = 0; i < size; i++) {
    result.append(1, static_cast<char>(i % 256));
  }
  return result;
}

TEST_F(URLRequestFileJobEventsTest, TinyFile) {
  RunRequest(std::string("hello world"), NULL);
}

TEST_F(URLRequestFileJobEventsTest, SmallFile) {
  RunRequest(MakeContentOfSize(17 * 1024), NULL);
}

TEST_F(URLRequestFileJobEventsTest, BigFile) {
  RunRequest(MakeContentOfSize(3 * 1024 * 1024), NULL);
}

TEST_F(URLRequestFileJobEventsTest, Range) {
  // Use a 15KB content file and read a range chosen somewhat arbitrarily but
  // not aligned on any likely page boundaries.
  int size = 15 * 1024;
  Range range(1701, (6 * 1024) + 3);
  RunRequest(MakeContentOfSize(size), &range);
}

}  // namespace

}  // namespace net
