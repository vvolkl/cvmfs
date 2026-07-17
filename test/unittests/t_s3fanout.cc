/**
 * This file is part of the CernVM File System.
 */

#include <gtest/gtest.h>

#include <cstdio>

#include "network/s3fanout.h"
#include "util/file_backed_buffer.h"

using namespace std;  // NOLINT

TEST(T_S3Fanout, DetectThrottleIndicator) {
  FileBackedBuffer *buf = FileBackedBuffer::Create(1024);
  s3fanout::JobInfo info("", NULL, buf);
  info.throttle_ms = 1;

  s3fanout::S3FanoutManager::DetectThrottleIndicator("", &info);
  EXPECT_EQ(1U, info.throttle_ms);
  s3fanout::S3FanoutManager::DetectThrottleIndicator("retry-after", &info);
  EXPECT_EQ(1U, info.throttle_ms);
  s3fanout::S3FanoutManager::DetectThrottleIndicator("retry-after:", &info);
  EXPECT_EQ(1U, info.throttle_ms);
  s3fanout::S3FanoutManager::DetectThrottleIndicator("x-retry-in:", &info);
  EXPECT_EQ(1U, info.throttle_ms);

  s3fanout::S3FanoutManager::DetectThrottleIndicator("retry-after: 1", &info);
  EXPECT_EQ(1000U, info.throttle_ms);
  s3fanout::S3FanoutManager::DetectThrottleIndicator("retry-after:5", &info);
  EXPECT_EQ(5000U, info.throttle_ms);
  s3fanout::S3FanoutManager::DetectThrottleIndicator("retry-after:42", &info);
  EXPECT_EQ(10000U, info.throttle_ms);
  s3fanout::S3FanoutManager::DetectThrottleIndicator("x-retry-in:2", &info);
  EXPECT_EQ(2000U, info.throttle_ms);
  s3fanout::S3FanoutManager::DetectThrottleIndicator("x-retry-in:0", &info);
  EXPECT_EQ(2000U, info.throttle_ms);

  s3fanout::S3FanoutManager::DetectThrottleIndicator("retry-after:13ms", &info);
  EXPECT_EQ(13U, info.throttle_ms);
  s3fanout::S3FanoutManager::DetectThrottleIndicator("retry-after:27Ms", &info);
  EXPECT_EQ(27U, info.throttle_ms);
  s3fanout::S3FanoutManager::DetectThrottleIndicator("retry-after:12000ms",
                                                     &info);
  EXPECT_EQ(10000U, info.throttle_ms);

  s3fanout::S3FanoutManager::DetectThrottleIndicator("X-Retry-In: 10ms\n",
                                                     &info);
  EXPECT_EQ(10U, info.throttle_ms);
  s3fanout::S3FanoutManager::DetectThrottleIndicator("X-Retry-In: 12ms\r\n",
                                                     &info);
  EXPECT_EQ(12U, info.throttle_ms);
}


TEST(T_S3Fanout, MkPath) {
  EXPECT_EQ("/data/ab/file1", s3fanout::MkPath("bkt", "data/ab/file1", true));
  EXPECT_EQ("/bkt/data/ab/file1",
            s3fanout::MkPath("bkt", "data/ab/file1", false));

  // Empty object key: multi-object delete and bucket creation post to the
  // bucket root.  With DNS buckets that leaves a bare "/", in path style the
  // bucket itself is the path and there is no trailing slash.
  EXPECT_EQ("/", s3fanout::MkPath("bkt", "", true));
  EXPECT_EQ("/bkt", s3fanout::MkPath("bkt", "", false));
}


// The V2 canonical resource must stay in sync with the path that is actually
// requested, otherwise the server computes a different signature.  Signing
// "/bkt?delete" for a request to "/" is what made Ceph RGW reject batch
// deletes with SignatureDoesNotMatch.
TEST(T_S3Fanout, MkV2CanonicalResourceMatchesPath) {
  const char * const keys[] = {"", "data/ab/file1"};
  const bool dns_buckets[] = {true, false};
  for (unsigned i = 0; i < 2; ++i) {
    for (unsigned j = 0; j < 2; ++j) {
      const string path = s3fanout::MkPath("bkt", keys[i], dns_buckets[j]);
      // With DNS buckets the bucket is in the hostname, so the signature has
      // to prepend it; in path style it is already part of the path.
      const string expected = (dns_buckets[j] ? "/bkt" + path : path);
      EXPECT_EQ(expected, s3fanout::MkV2CanonicalResource(
                              "bkt", keys[i], dns_buckets[j], false));
      EXPECT_EQ(expected + "?delete",
                s3fanout::MkV2CanonicalResource("bkt", keys[i], dns_buckets[j],
                                                true));
    }
  }
}


TEST(T_S3Fanout, MkV2CanonicalResource) {
  // The resource must mirror MkUrl().  With DNS buckets the request path is
  // "/" + object_key, so an empty key keeps the trailing slash; in path style
  // the bucket is the path and an empty key has no trailing slash.
  EXPECT_EQ(
      "/bkt/data/ab/file1",
      s3fanout::MkV2CanonicalResource("bkt", "data/ab/file1", true, false));
  EXPECT_EQ(
      "/bkt/data/ab/file1",
      s3fanout::MkV2CanonicalResource("bkt", "data/ab/file1", false, false));

  // Multi-object delete posts to the bucket root with an empty object key.
  // Signing "/bkt?delete" here makes Ceph RGW reject the request with
  // SignatureDoesNotMatch, because it rebuilds the resource from the path "/".
  EXPECT_EQ("/bkt/?delete",
            s3fanout::MkV2CanonicalResource("bkt", "", true, true));
  EXPECT_EQ("/bkt?delete",
            s3fanout::MkV2CanonicalResource("bkt", "", false, true));

  // Bucket creation is the other empty-key request
  EXPECT_EQ("/bkt/", s3fanout::MkV2CanonicalResource("bkt", "", true, false));
  EXPECT_EQ("/bkt", s3fanout::MkV2CanonicalResource("bkt", "", false, false));
}


TEST(T_S3Fanout, ComposeDeleteMultiXmlSingleKey) {
  vector<string> keys;
  keys.push_back("data/ab/file1");
  const string xml = s3fanout::ComposeDeleteMultiXml(keys);

  EXPECT_NE(string::npos, xml.find("<?xml version=\"1.0\""));
  EXPECT_NE(string::npos, xml.find("<Delete>"));
  EXPECT_NE(string::npos, xml.find("<Quiet>true</Quiet>"));
  EXPECT_NE(string::npos, xml.find("<Object><Key>data/ab/file1</Key></Object>"));
  EXPECT_NE(string::npos, xml.find("</Delete>"));
}


TEST(T_S3Fanout, ComposeDeleteMultiXmlMultipleKeys) {
  vector<string> keys;
  keys.push_back("data/ab/file1");
  keys.push_back("data/cd/file2");
  keys.push_back("data/ef/file3");
  const string xml = s3fanout::ComposeDeleteMultiXml(keys);

  EXPECT_NE(string::npos, xml.find("<Object><Key>data/ab/file1</Key></Object>"));
  EXPECT_NE(string::npos, xml.find("<Object><Key>data/cd/file2</Key></Object>"));
  EXPECT_NE(string::npos, xml.find("<Object><Key>data/ef/file3</Key></Object>"));
}


TEST(T_S3Fanout, ComposeDeleteMultiXmlEmptyKeys) {
  vector<string> keys;
  const string xml = s3fanout::ComposeDeleteMultiXml(keys);

  EXPECT_NE(string::npos, xml.find("<Delete>"));
  EXPECT_NE(string::npos, xml.find("<Quiet>true</Quiet>"));
  EXPECT_NE(string::npos, xml.find("</Delete>"));
  EXPECT_EQ(string::npos, xml.find("<Object>"));
}


TEST(T_S3Fanout, ComposeDeleteMultiXmlEscaping) {
  vector<string> keys;
  keys.push_back("repo&name/data/ab/file1");
  keys.push_back("repo<name/data/cd/file2");
  keys.push_back("normal/data/ef/file3");
  const string xml = s3fanout::ComposeDeleteMultiXml(keys);

  EXPECT_NE(string::npos,
            xml.find("<Key>repo&amp;name/data/ab/file1</Key>"));
  EXPECT_NE(string::npos,
            xml.find("<Key>repo&lt;name/data/cd/file2</Key>"));
  EXPECT_NE(string::npos,
            xml.find("<Key>normal/data/ef/file3</Key>"));
  // Raw & and < must not appear unescaped in key values
  EXPECT_EQ(string::npos, xml.find("repo&name"));
  EXPECT_EQ(string::npos, xml.find("repo<name"));
}


TEST(T_S3Fanout, ParseDeleteMultiResponseEmpty) {
  vector<string> error_keys, error_codes, error_messages;
  const unsigned n = s3fanout::ParseDeleteMultiResponse(
      "", &error_keys, &error_codes, &error_messages);

  EXPECT_EQ(0U, n);
  EXPECT_TRUE(error_keys.empty());
}


TEST(T_S3Fanout, ParseDeleteMultiResponseSingleError) {
  const string response =
      "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
      "<DeleteResult>"
      "<Error>"
      "<Key>data/ab/file1</Key>"
      "<Code>AccessDenied</Code>"
      "<Message>Access Denied</Message>"
      "</Error>"
      "</DeleteResult>";

  vector<string> error_keys, error_codes, error_messages;
  const unsigned n = s3fanout::ParseDeleteMultiResponse(
      response, &error_keys, &error_codes, &error_messages);

  EXPECT_EQ(1U, n);
  ASSERT_EQ(1U, error_keys.size());
  EXPECT_EQ("data/ab/file1", error_keys[0]);
  EXPECT_EQ("AccessDenied", error_codes[0]);
  EXPECT_EQ("Access Denied", error_messages[0]);
}


TEST(T_S3Fanout, ParseDeleteMultiResponseMultipleErrors) {
  const string response =
      "<DeleteResult>"
      "<Error><Key>key1</Key><Code>NoSuchKey</Code>"
      "<Message>Not found</Message></Error>"
      "<Error><Key>key2</Key><Code>InternalError</Code>"
      "<Message>Internal</Message></Error>"
      "</DeleteResult>";

  vector<string> error_keys, error_codes, error_messages;
  const unsigned n = s3fanout::ParseDeleteMultiResponse(
      response, &error_keys, &error_codes, &error_messages);

  EXPECT_EQ(2U, n);
  ASSERT_EQ(2U, error_keys.size());
  EXPECT_EQ("key1", error_keys[0]);
  EXPECT_EQ("NoSuchKey", error_codes[0]);
  EXPECT_EQ("key2", error_keys[1]);
  EXPECT_EQ("InternalError", error_codes[1]);
}


TEST(T_S3Fanout, ParseDeleteMultiResponseMalformed) {
  const string response = "<DeleteResult><Error><Key>k1</Key>";

  vector<string> error_keys, error_codes, error_messages;
  const unsigned n = s3fanout::ParseDeleteMultiResponse(
      response, &error_keys, &error_codes, &error_messages);

  EXPECT_EQ(0U, n);
}
