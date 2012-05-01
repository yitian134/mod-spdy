// Copyright 2010 Google Inc. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "mod_spdy/common/spdy_session.h"

#include <list>
#include <string>

#include "base/basictypes.h"
#include "base/logging.h"
#include "base/memory/scoped_ptr.h"
#include "mod_spdy/common/protocol_util.h"
#include "mod_spdy/common/spdy_server_config.h"
#include "mod_spdy/common/spdy_session_io.h"
#include "mod_spdy/common/spdy_stream_task_factory.h"
#include "mod_spdy/common/testing/spdy_frame_matchers.h"
#include "mod_spdy/common/thread_pool.h"
#include "net/instaweb/util/public/function.h"
#include "net/spdy/buffered_spdy_framer.h"
#include "net/spdy/spdy_framer.h"
#include "net/spdy/spdy_protocol.h"
#include "testing/gmock/include/gmock/gmock.h"
#include "testing/gtest/include/gtest/gtest.h"

using mod_spdy::testing::FlagFinIs;
using mod_spdy::testing::IsControlFrameOfType;
using mod_spdy::testing::IsDataFrameWith;
using mod_spdy::testing::IsGoAway;
using testing::_;
using testing::AllOf;
using testing::AtLeast;
using testing::DoAll;
using testing::Eq;
using testing::Invoke;
using testing::InvokeWithoutArgs;
using testing::NotNull;
using testing::Property;
using testing::Return;
using testing::WithArg;

namespace {

void AddRequiredHeaders(net::SpdyHeaderBlock* headers) {
  (*headers)["host"] = "www.example.com";
  (*headers)["method"] = "GET";
  (*headers)["scheme"] = "https";
  (*headers)["url"] = "/foo/index.html";
  (*headers)["version"] = "HTTP/1.1";
}

class MockSpdySessionIO : public mod_spdy::SpdySessionIO {
 public:
  MOCK_METHOD0(IsConnectionAborted, bool());
  MOCK_METHOD2(ProcessAvailableInput,
               ReadStatus(bool, net::BufferedSpdyFramer*));
  MOCK_METHOD1(SendFrameRaw, WriteStatus(const net::SpdyFrame&));
};

class MockSpdyStreamTaskFactory : public mod_spdy::SpdyStreamTaskFactory {
 public:
  MOCK_METHOD1(NewStreamTask, net_instaweb::Function*(mod_spdy::SpdyStream*));
};

class FakeStreamTask : public net_instaweb::Function {
 public:
  virtual ~FakeStreamTask() {}
  static FakeStreamTask* SimpleResponse(mod_spdy::SpdyStream* stream) {
    return new FakeStreamTask(stream);
  }

 protected:
  // net_instaweb::Function methods:
  virtual void Run();
  virtual void Cancel() {}

 private:
  FakeStreamTask(mod_spdy::SpdyStream* stream)
      : stream_(stream) {}

  mod_spdy::SpdyStream* const stream_;

  DISALLOW_COPY_AND_ASSIGN(FakeStreamTask);
};

void FakeStreamTask::Run() {
  if (!stream_->is_server_push()) {
    net::SpdyFrame* raw_frame;
    ASSERT_TRUE(stream_->GetInputFrame(false, &raw_frame));
    scoped_ptr<net::SpdyFrame> frame(raw_frame);
    ASSERT_TRUE(frame != NULL);
    EXPECT_THAT(*frame, IsControlFrameOfType(net::SYN_STREAM));
  }

  net::SpdyHeaderBlock headers;
  headers["status"] = "200";
  headers["version"] = "HTTP/1.1";
  if (stream_->is_server_push()) {
    stream_->SendOutputSynStream(headers, false);
  } else {
    stream_->SendOutputSynReply(headers, false);
  }

  stream_->SendOutputDataFrame("foobar", false);
  stream_->SendOutputDataFrame("quux", true);
}

// An executor that runs all tasks in the same thread, either immediately when
// they are added or when it is told to run them.
class InlineExecutor : public mod_spdy::Executor {
 public:
  InlineExecutor() : run_on_add_(false), stopped_(false) {}
  virtual ~InlineExecutor() { Stop(); }

  virtual void AddTask(net_instaweb::Function* task,
                       net::SpdyPriority priority) {
    if (stopped_) {
      task->CallCancel();
    } else if (run_on_add_) {
      task->CallRun();
    } else {
      tasks_.push_back(task);
    }
  }
  virtual void Stop() {
    stopped_ = true;
    while (!tasks_.empty()) {
      tasks_.front()->CallCancel();
      tasks_.pop_front();
    }
  }
  void RunOne() {
    if (!tasks_.empty()) {
      tasks_.front()->CallRun();
      tasks_.pop_front();
    }
  }
  void RunAll() {
    while (!tasks_.empty()) {
      RunOne();
    }
  }
  void set_run_on_add(bool run) { run_on_add_ = run; }
  bool stopped() const { return stopped_; }

 private:
  std::list<net_instaweb::Function*> tasks_;
  bool run_on_add_;
  bool stopped_;

  DISALLOW_COPY_AND_ASSIGN(InlineExecutor);
};

// Base class for SpdySession tests.
class SpdySessionTestBase : public testing::TestWithParam<int> {
 public:
  SpdySessionTestBase() : framer_(GetParam()) {
    ON_CALL(session_io_, IsConnectionAborted()).WillByDefault(Return(false));
    ON_CALL(session_io_, ProcessAvailableInput(_, NotNull()))
        .WillByDefault(Invoke(this, &SpdySessionTestBase::ReadNextInputChunk));
    ON_CALL(session_io_, SendFrameRaw(_))
        .WillByDefault(Return(mod_spdy::SpdySessionIO::WRITE_SUCCESS));
  }

  // Use as gMock action for ProcessAvailableInput:
  //   Invoke(this, &SpdySessionTest::ReadNextInputChunk)
  mod_spdy::SpdySessionIO::ReadStatus ReadNextInputChunk(
      bool block, net::BufferedSpdyFramer* framer) {
    if (input_queue_.empty()) {
      return mod_spdy::SpdySessionIO::READ_CONNECTION_CLOSED;
    }
    const std::string chunk = input_queue_.front();
    input_queue_.pop_front();
    framer->ProcessInput(chunk.data(), chunk.size());
    return (framer->HasError() ? mod_spdy::SpdySessionIO::READ_ERROR :
            mod_spdy::SpdySessionIO::READ_SUCCESS);
  }

 protected:
  // Push some random garbage bytes into the input queue.
  void PushGarbageData() {
    input_queue_.push_back("\x88\x5f\x92\x02\xf8\x92\x12\xd1"
                           "\x82\xdc\x1a\x40\xbb\xb2\x9d\x13");
  }

  // Push a frame into the input queue.
  void PushFrame(const net::SpdyFrame& frame) {
    input_queue_.push_back(mod_spdy::FrameData(frame).as_string());
  }

  // Push a PING frame into the input queue.
  void PushPingFrame(uint32 id) {
    scoped_ptr<net::SpdyPingControlFrame> frame(framer_.CreatePingFrame(id));
    PushFrame(*frame);
  }

  // Push a SETTINGS frame into the input queue.
  void PushSettingsFrame(uint32 init_window_size) {
    net::SettingsMap settings;
    settings[net::SETTINGS_INITIAL_WINDOW_SIZE] = std::make_pair(
        net::SETTINGS_FLAG_NONE, init_window_size);
    scoped_ptr<net::SpdySettingsControlFrame> frame(
        framer_.CreateSettings(settings));
    PushFrame(*frame);
  }

  // Push a valid SYN_STREAM frame into the input queue.
  void PushSynStreamFrame(net::SpdyStreamId stream_id,
                          net::SpdyPriority priority,
                          net::SpdyControlFlags flags) {
    net::SpdyHeaderBlock headers;
    AddRequiredHeaders(&headers);
    scoped_ptr<net::SpdySynStreamControlFrame> frame(framer_.CreateSynStream(
        stream_id, 0, priority, 0, flags,
        true,  // true = use compression
        &headers));
    PushFrame(*frame);
  }

  net::SpdyFramer framer_;
  mod_spdy::SpdyServerConfig config_;
  MockSpdySessionIO session_io_;
  MockSpdyStreamTaskFactory task_factory_;
  std::list<std::string> input_queue_;
};

// Class for most SpdySession tests; this uses an InlineExecutor, so that test
// behavior is very predictable.
class SpdySessionTest : public SpdySessionTestBase {
 public:
  SpdySessionTest()
      : session_(framer_.protocol_version(), &config_, &session_io_,
                 &task_factory_, &executor_) {}

  // Use as gMock action for SendFrameRaw:
  //   Invoke(this, &SpdySessionTest::ProcessOutputWithFlowControl)
  mod_spdy::SpdySessionIO::WriteStatus ProcessOutputWithFlowControl(
      const net::SpdyFrame& frame) {
    // For SPDY v3 and above, send back a WINDOW_UPDATE frame saying we
    // consumed the data frame.
    if (session_.spdy_version() >= 3 && !frame.is_control_frame()) {
      const net::SpdyDataFrame& data_frame =
          *static_cast<const net::SpdyDataFrame*>(&frame);
      scoped_ptr<net::SpdyWindowUpdateControlFrame> window_update_frame(
          framer_.CreateWindowUpdate(data_frame.stream_id(),
                                     data_frame.length()));
      PushFrame(*window_update_frame);
    }
    return mod_spdy::SpdySessionIO::WRITE_SUCCESS;
  }

 protected:
  InlineExecutor executor_;
  mod_spdy::SpdySession session_;
};

// Test that if the connection is already closed, we stop immediately.
TEST_P(SpdySessionTest, ConnectionAlreadyClosed) {
  testing::InSequence seq;
  EXPECT_CALL(session_io_, SendFrameRaw(IsControlFrameOfType(net::SETTINGS)))
      .WillOnce(Return(mod_spdy::SpdySessionIO::WRITE_CONNECTION_CLOSED));

  session_.Run();
  EXPECT_TRUE(executor_.stopped());
}

// Test that when the connection is aborted, we stop.
TEST_P(SpdySessionTest, ImmediateConnectionAbort) {
  testing::InSequence seq;
  EXPECT_CALL(session_io_, SendFrameRaw(IsControlFrameOfType(net::SETTINGS)));
  EXPECT_CALL(session_io_, IsConnectionAborted()).WillOnce(Return(true));

  session_.Run();
  EXPECT_TRUE(executor_.stopped());
}

// Test responding to a PING frame from the client (followed by the connection
// closing, so that we can exit the Run loop).
TEST_P(SpdySessionTest, SinglePing) {
  PushPingFrame(1);

  testing::InSequence seq;
  EXPECT_CALL(session_io_, SendFrameRaw(IsControlFrameOfType(net::SETTINGS)));
  EXPECT_CALL(session_io_, IsConnectionAborted());
  EXPECT_CALL(session_io_, ProcessAvailableInput(Eq(true), NotNull()));
  EXPECT_CALL(session_io_, SendFrameRaw(IsControlFrameOfType(net::PING)));
  EXPECT_CALL(session_io_, IsConnectionAborted());
  EXPECT_CALL(session_io_, ProcessAvailableInput(Eq(true), NotNull()));
  EXPECT_CALL(session_io_, SendFrameRaw(IsGoAway(net::GOAWAY_OK)));

  session_.Run();
  EXPECT_TRUE(executor_.stopped());
}

// Test handling a single stream request.
TEST_P(SpdySessionTest, SingleStream) {
  executor_.set_run_on_add(true);
  const net::SpdyStreamId stream_id = 1;
  const net::SpdyPriority priority = 2;
  PushSynStreamFrame(stream_id, priority, net::CONTROL_FLAG_FIN);

  testing::InSequence seq;
  EXPECT_CALL(session_io_, SendFrameRaw(IsControlFrameOfType(net::SETTINGS)));
  EXPECT_CALL(session_io_, IsConnectionAborted());
  EXPECT_CALL(session_io_, ProcessAvailableInput(Eq(true), NotNull()));
  EXPECT_CALL(task_factory_, NewStreamTask(
      AllOf(Property(&mod_spdy::SpdyStream::stream_id, Eq(stream_id)),
            Property(&mod_spdy::SpdyStream::associated_stream_id, Eq(0)),
            Property(&mod_spdy::SpdyStream::priority, Eq(priority)))))
      .WillOnce(WithArg<0>(Invoke(FakeStreamTask::SimpleResponse)));
  EXPECT_CALL(session_io_, SendFrameRaw(
      AllOf(IsControlFrameOfType(net::SYN_REPLY), FlagFinIs(false))));
  EXPECT_CALL(session_io_, SendFrameRaw(
      AllOf(IsDataFrameWith("foobar"), FlagFinIs(false))));
  EXPECT_CALL(session_io_, SendFrameRaw(
      AllOf(IsDataFrameWith("quux"), FlagFinIs(true))));
  EXPECT_CALL(session_io_, IsConnectionAborted());
  EXPECT_CALL(session_io_, ProcessAvailableInput(Eq(true), NotNull()));
  EXPECT_CALL(session_io_, SendFrameRaw(IsGoAway(net::GOAWAY_OK)));

  session_.Run();
  EXPECT_TRUE(executor_.stopped());
}

// Test that if SendFrameRaw fails, we immediately stop trying to send data and
// shut down the session.
TEST_P(SpdySessionTest, ShutDownSessionIfSendFrameRawFails) {
  executor_.set_run_on_add(true);
  PushSynStreamFrame(1, 2, net::CONTROL_FLAG_FIN);

  testing::InSequence seq;
  // We start out the same way as in the SingleStream test above.
  EXPECT_CALL(session_io_, SendFrameRaw(IsControlFrameOfType(net::SETTINGS)));
  EXPECT_CALL(session_io_, IsConnectionAborted());
  EXPECT_CALL(session_io_, ProcessAvailableInput(_, _));
  EXPECT_CALL(task_factory_, NewStreamTask(_))
      .WillOnce(WithArg<0>(Invoke(FakeStreamTask::SimpleResponse)));
  EXPECT_CALL(session_io_, SendFrameRaw(
      AllOf(IsControlFrameOfType(net::SYN_REPLY), FlagFinIs(false))));
  // At this point, the connection is closed by the client.
  EXPECT_CALL(session_io_, SendFrameRaw(
      AllOf(IsDataFrameWith("foobar"), FlagFinIs(false))))
      .WillOnce(Return(mod_spdy::SpdySessionIO::WRITE_CONNECTION_CLOSED));
  // Even though we have another frame to send at this point (already in the
  // output queue), we immediately stop sending data and exit the session.

  session_.Run();
  EXPECT_TRUE(executor_.stopped());
}

// Test that when the client sends us garbage data, we send a GOAWAY frame and
// then quit.
TEST_P(SpdySessionTest, SendGoawayInResponseToGarbage) {
  PushGarbageData();

  testing::InSequence seq;
  EXPECT_CALL(session_io_, SendFrameRaw(IsControlFrameOfType(net::SETTINGS)));
  EXPECT_CALL(session_io_, IsConnectionAborted());
  EXPECT_CALL(session_io_, ProcessAvailableInput(Eq(true), NotNull()));
  EXPECT_CALL(session_io_, SendFrameRaw(IsGoAway(net::GOAWAY_PROTOCOL_ERROR)));

  session_.Run();
  EXPECT_TRUE(executor_.stopped());
}

// Test that when the client sends us a SYN_STREAM with a corrupted header
// block, we send a GOAWAY frame and then quit.
TEST_P(SpdySessionTest, SendGoawayForBadSynStreamCompression) {
  net::SpdyHeaderBlock headers;
  headers["foobar"] = "Foo is to bar as bar is to baz.";
  scoped_ptr<net::SpdyFrame> frame(framer_.CreateSynStream(
      1, 0, SPDY_PRIORITY_HIGHEST, 0, net::CONTROL_FLAG_FIN,
      false,  // false = no compression
      &headers));
  PushFrame(*frame);

  testing::InSequence seq;
  EXPECT_CALL(session_io_, SendFrameRaw(IsControlFrameOfType(net::SETTINGS)));
  EXPECT_CALL(session_io_, IsConnectionAborted());
  EXPECT_CALL(session_io_, ProcessAvailableInput(Eq(true), NotNull()));
  EXPECT_CALL(session_io_, SendFrameRaw(IsGoAway(net::GOAWAY_PROTOCOL_ERROR)));

  session_.Run();
  EXPECT_TRUE(executor_.stopped());
}

// TODO(mdsteele): At the moment, SpdyFramer DCHECKs that the stream ID is
// nonzero when decoding, so this test would crash in debug builds.  Once this
// has been corrected in the Chromium code, we can remove this #ifdef.
#ifdef NDEBUG
// Test that when the client sends us a SYN_STREAM with a stream ID of 0, we
// send a GOAWAY frame and then quit.
TEST_P(SpdySessionTest, SendGoawayForSynStreamIdZero) {
  net::SpdyHeaderBlock headers;
  AddRequiredHeaders(&headers);
  // SpdyFramer DCHECKS that the stream_id isn't zero, so just create the frame
  // with a stream_id of 1, and then set the stream_id on the next line.
  scoped_ptr<net::SpdySynStreamControlFrame> frame(framer_.CreateSynStream(
      1, 0, SPDY_PRIORITY_HIGHEST, 0, net::CONTROL_FLAG_FIN, true, &headers));
  frame->set_stream_id(0);
  PushFrame(*frame);

  testing::InSequence seq;
  EXPECT_CALL(session_io_, SendFrameRaw(IsControlFrameOfType(net::SETTINGS)));
  EXPECT_CALL(session_io_, IsConnectionAborted());
  EXPECT_CALL(session_io_, ProcessAvailableInput(Eq(true), NotNull()));
  EXPECT_CALL(session_io_, SendFrameRaw(IsGoAway(net::GOAWAY_PROTOCOL_ERROR)));

  session_.Run();
  EXPECT_TRUE(executor_.stopped());
}
#endif

// Test that when the client sends us a SYN_STREAM with invalid flags, we
// send a GOAWAY frame and then quit.
TEST_P(SpdySessionTest, SendGoawayForSynStreamWithInvalidFlags) {
  net::SpdyHeaderBlock headers;
  AddRequiredHeaders(&headers);
  // SpdyFramer DCHECKS that the flags are valid, so just create the frame
  // with no flags, and then set the flags on the next line.
  scoped_ptr<net::SpdySynStreamControlFrame> frame(framer_.CreateSynStream(
      1, 0, SPDY_PRIORITY_HIGHEST, 0, net::CONTROL_FLAG_NONE, true, &headers));
  frame->set_flags(0x47);
  PushFrame(*frame);

  testing::InSequence seq;
  EXPECT_CALL(session_io_, SendFrameRaw(IsControlFrameOfType(net::SETTINGS)));
  EXPECT_CALL(session_io_, IsConnectionAborted());
  EXPECT_CALL(session_io_, ProcessAvailableInput(Eq(true), NotNull()));
  EXPECT_CALL(session_io_, SendFrameRaw(IsGoAway(net::GOAWAY_PROTOCOL_ERROR)));

  session_.Run();
  EXPECT_TRUE(executor_.stopped());
}

// Test that when the client sends us two SYN_STREAMs with the same ID, we send
// a GOAWAY frame (but still finish out the good stream before quitting).
TEST_P(SpdySessionTest, SendGoawayForDuplicateStreamId) {
  executor_.set_run_on_add(false);
  const net::SpdyStreamId stream_id = 1;
  const net::SpdyPriority priority = 2;
  PushSynStreamFrame(stream_id, priority, net::CONTROL_FLAG_FIN);
  PushSynStreamFrame(stream_id, priority, net::CONTROL_FLAG_FIN);

  testing::InSequence seq;
  EXPECT_CALL(session_io_, SendFrameRaw(IsControlFrameOfType(net::SETTINGS)));
  EXPECT_CALL(session_io_, IsConnectionAborted());
  // Get the first SYN_STREAM; it looks good, so create a new task (but because
  // we set executor_.set_run_on_add(false) above, it doesn't execute yet).
  EXPECT_CALL(session_io_, ProcessAvailableInput(Eq(true), NotNull()));
  EXPECT_CALL(task_factory_, NewStreamTask(
      AllOf(Property(&mod_spdy::SpdyStream::stream_id, Eq(stream_id)),
            Property(&mod_spdy::SpdyStream::associated_stream_id, Eq(0)),
            Property(&mod_spdy::SpdyStream::priority, Eq(priority)))))
      .WillOnce(WithArg<0>(Invoke(FakeStreamTask::SimpleResponse)));
  EXPECT_CALL(session_io_, IsConnectionAborted());
  // There's an active stream out, so ProcessAvailableInput should have false
  // for the first argument (false = nonblocking read).  Here we get the second
  // SYN_STREAM with the same stream ID, so we should send GOAWAY.
  EXPECT_CALL(session_io_, ProcessAvailableInput(Eq(false), NotNull()));
  EXPECT_CALL(session_io_, SendFrameRaw(IsGoAway(net::GOAWAY_PROTOCOL_ERROR)));
  // At this point, tell the executor to run the task.
  EXPECT_CALL(session_io_, IsConnectionAborted())
      .WillOnce(DoAll(InvokeWithoutArgs(&executor_, &InlineExecutor::RunAll),
                      Return(false)));
  // The stream is no longer active, but there are pending frames to send, so
  // we shouldn't block on input.
  EXPECT_CALL(session_io_, ProcessAvailableInput(Eq(false), NotNull()));
  // Now we should send the output.
  EXPECT_CALL(session_io_, SendFrameRaw(
      AllOf(IsControlFrameOfType(net::SYN_REPLY), FlagFinIs(false))));
  EXPECT_CALL(session_io_, SendFrameRaw(
      AllOf(IsDataFrameWith("foobar"), FlagFinIs(false))));
  EXPECT_CALL(session_io_, SendFrameRaw(
      AllOf(IsDataFrameWith("quux"), FlagFinIs(true))));
  // Finally, there is no more input to read and no more output to send, so we
  // quit.
  EXPECT_CALL(session_io_, IsConnectionAborted());

  session_.Run();
  EXPECT_TRUE(executor_.stopped());
}

// Run each test over both SPDY v2 and SPDY v3.
INSTANTIATE_TEST_CASE_P(Spdy2And3, SpdySessionTest, testing::Values(2, 3));

// Create a type alias so that we can instantiate some of our
// SpdySessionTest-based tests using a different set of parameters.
typedef SpdySessionTest SpdySessionNoFlowControlTest;

// Test that we send GOAWAY if the client tries to send
// SETTINGS_INITIAL_WINDOW_SIZE over SPDY v2.
TEST_P(SpdySessionNoFlowControlTest, SendGoawayForInitialWindowSize) {
  net::SettingsMap settings;
  settings[net::SETTINGS_INITIAL_WINDOW_SIZE] =
      std::make_pair(net::SETTINGS_FLAG_NONE, 4000);
  scoped_ptr<net::SpdyFrame> frame(framer_.CreateSettings(settings));
  PushFrame(*frame);

  testing::InSequence seq;
  EXPECT_CALL(session_io_, SendFrameRaw(IsControlFrameOfType(net::SETTINGS)));
  EXPECT_CALL(session_io_, IsConnectionAborted());
  EXPECT_CALL(session_io_, ProcessAvailableInput(Eq(true), NotNull()));
  EXPECT_CALL(session_io_, SendFrameRaw(IsGoAway(net::GOAWAY_PROTOCOL_ERROR)));

  session_.Run();
  EXPECT_TRUE(executor_.stopped());
}

// Only run no-flow-control tests for SPDY v2.
INSTANTIATE_TEST_CASE_P(Spdy2, SpdySessionNoFlowControlTest,
                        testing::Values(2));

// Test class for flow-control tests.  This uses a ThreadPool Executor, so that
// we can test concurrency behavior.
class SpdySessionFlowControlTest : public SpdySessionTestBase {
 public:
  SpdySessionFlowControlTest()
      : thread_pool_(1, 1) {
    ON_CALL(session_io_, SendFrameRaw(_)).WillByDefault(Invoke(
        this, &SpdySessionFlowControlTest::ProcessOutputWithFlowControl));
  }

  void SetUp() {
    ASSERT_TRUE(thread_pool_.Start());
    executor_.reset(thread_pool_.NewExecutor());
    session_.reset(new mod_spdy::SpdySession(
        framer_.protocol_version(), &config_, &session_io_, &task_factory_,
        executor_.get()));
  }

  // Use as gMock action for SendFrameRaw:
  //   Invoke(this, &SpdySessionTest::ProcessOutputWithFlowControl)
  mod_spdy::SpdySessionIO::WriteStatus ProcessOutputWithFlowControl(
      const net::SpdyFrame& frame) {
    // For SPDY v3 and above, send back a WINDOW_UPDATE frame saying we
    // consumed the data frame.
    if (session_->spdy_version() >= 3 && !frame.is_control_frame()) {
      const net::SpdyDataFrame& data_frame =
          *static_cast<const net::SpdyDataFrame*>(&frame);
      scoped_ptr<net::SpdyWindowUpdateControlFrame> window_update_frame(
          framer_.CreateWindowUpdate(data_frame.stream_id(),
                                     data_frame.length()));
      PushFrame(*window_update_frame);
    }
    return mod_spdy::SpdySessionIO::WRITE_SUCCESS;
  }

 protected:
  mod_spdy::ThreadPool thread_pool_;
  scoped_ptr<mod_spdy::Executor> executor_;
  scoped_ptr<mod_spdy::SpdySession> session_;
};

TEST_P(SpdySessionFlowControlTest, SingleStreamWithFlowControl) {
  // Start by setting the initial window size to very small (three bytes).
  PushSettingsFrame(3);
  // Then send a SYN_STREAM.
  const net::SpdyStreamId stream_id = 1;
  const net::SpdyPriority priority = 2;
  PushSynStreamFrame(stream_id, priority, net::CONTROL_FLAG_FIN);

  // We'll have to go through the loop at least seven times (once for each of
  // six frames (SETTINGS, SYN_STREAM, and four WINDOW_UDPATEs), and once to
  // determine that the connection is closed).
  EXPECT_CALL(session_io_, IsConnectionAborted()).Times(AtLeast(7));
  EXPECT_CALL(session_io_, ProcessAvailableInput(_, NotNull()))
      .Times(AtLeast(7));

  // The rest of these will have to happen in a fixed order.
  testing::Sequence s1;
  EXPECT_CALL(session_io_, SendFrameRaw(IsControlFrameOfType(net::SETTINGS)))
      .InSequence(s1);
  EXPECT_CALL(task_factory_, NewStreamTask(
      AllOf(Property(&mod_spdy::SpdyStream::stream_id, Eq(stream_id)),
            Property(&mod_spdy::SpdyStream::associated_stream_id, Eq(0)),
            Property(&mod_spdy::SpdyStream::priority, Eq(priority)))))
      .InSequence(s1)
      .WillOnce(WithArg<0>(Invoke(FakeStreamTask::SimpleResponse)));
  EXPECT_CALL(session_io_, SendFrameRaw(
      AllOf(IsControlFrameOfType(net::SYN_REPLY), FlagFinIs(false))))
      .InSequence(s1);
  // Since the window size is just three bytes, we can only send three bytes at
  // a time.
  EXPECT_CALL(session_io_, SendFrameRaw(
      AllOf(IsDataFrameWith("foo"), FlagFinIs(false)))).InSequence(s1);
  EXPECT_CALL(session_io_, SendFrameRaw(
      AllOf(IsDataFrameWith("bar"), FlagFinIs(false)))).InSequence(s1);
  EXPECT_CALL(session_io_, SendFrameRaw(
      AllOf(IsDataFrameWith("quu"), FlagFinIs(false)))).InSequence(s1);
  EXPECT_CALL(session_io_, SendFrameRaw(
      AllOf(IsDataFrameWith("x"), FlagFinIs(true)))).InSequence(s1);
  EXPECT_CALL(session_io_, SendFrameRaw(IsGoAway(net::GOAWAY_OK)));

  session_->Run();
}

// Test that we send GOAWAY if the client tries to send
// SETTINGS_INITIAL_WINDOW_SIZE with a value of 0.
TEST_P(SpdySessionFlowControlTest, SendGoawayForTooSmallInitialWindowSize) {
  net::SettingsMap settings;
  settings[net::SETTINGS_INITIAL_WINDOW_SIZE] =
      std::make_pair(net::SETTINGS_FLAG_NONE, 0);
  scoped_ptr<net::SpdyFrame> frame(framer_.CreateSettings(settings));
  PushFrame(*frame);

  testing::InSequence seq;
  EXPECT_CALL(session_io_, SendFrameRaw(IsControlFrameOfType(net::SETTINGS)));
  EXPECT_CALL(session_io_, IsConnectionAborted());
  EXPECT_CALL(session_io_, ProcessAvailableInput(Eq(true), NotNull()));
  EXPECT_CALL(session_io_, SendFrameRaw(IsGoAway(net::GOAWAY_PROTOCOL_ERROR)));

  session_->Run();
}

// Test that we send GOAWAY if the client tries to send
// SETTINGS_INITIAL_WINDOW_SIZE with a value of 0x80000000.
TEST_P(SpdySessionFlowControlTest, SendGoawayForTooLargeInitialWindowSize) {
  net::SettingsMap settings;
  settings[net::SETTINGS_INITIAL_WINDOW_SIZE] =
      std::make_pair(net::SETTINGS_FLAG_NONE, 0x80000000);
  scoped_ptr<net::SpdyFrame> frame(framer_.CreateSettings(settings));
  PushFrame(*frame);

  testing::InSequence seq;
  EXPECT_CALL(session_io_, SendFrameRaw(IsControlFrameOfType(net::SETTINGS)));
  EXPECT_CALL(session_io_, IsConnectionAborted());
  EXPECT_CALL(session_io_, ProcessAvailableInput(Eq(true), NotNull()));
  EXPECT_CALL(session_io_, SendFrameRaw(IsGoAway(net::GOAWAY_PROTOCOL_ERROR)));

  session_->Run();
}

// Only run flow control tests for SPDY v3.
INSTANTIATE_TEST_CASE_P(Spdy3, SpdySessionFlowControlTest, testing::Values(3));

}  // namespace
