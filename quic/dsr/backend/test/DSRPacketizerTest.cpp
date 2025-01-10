/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <folly/portability/GTest.h>
#include <quic/common/events/FollyQuicEventBase.h>
#include <quic/common/test/TestUtils.h>
#include <quic/common/testutil/MockAsyncUDPSocket.h>
#include <quic/common/udpsocket/QuicAsyncUDPSocket.h>
#include <quic/dsr/backend/DSRPacketizer.h>
#include <quic/dsr/backend/test/TestUtils.h>
#include <quic/dsr/frontend/WriteFunctions.h>
#include <quic/dsr/test/TestCommon.h>

using namespace testing;

namespace {
fizz::TrafficKey getFizzTestKey() {
  fizz::TrafficKey testKey;
  auto quicKey = quic::test::getQuicTestKey();
  testKey.key = std::move(quicKey.key);
  testKey.iv = std::move(quicKey.iv);
  return testKey;
}
} // namespace

namespace quic::test {

class DSRPacketizerTest : public DSRCommonTestFixture {};

TEST_F(DSRPacketizerTest, BuildCipher) {
  CipherBuilder cipherBuilder;
  auto cipherPair = cipherBuilder.buildCiphers(
      getFizzTestKey(),
      fizz::CipherSuite::TLS_AES_128_GCM_SHA256,
      packetProtectionKey_->clone());
  EXPECT_NE(cipherPair.aead, nullptr);
  EXPECT_NE(cipherPair.headerCipher, nullptr);
}

class DSRPacketizerSingleWriteTest : public Test {
 protected:
  void SetUp() override {
    aead = test::createNoOpAead();
    headerCipher = test::createNoOpHeaderCipher();
    qEvb_ = std::make_shared<FollyQuicEventBase>(&evb);
  }

  folly::EventBase evb;
  std::shared_ptr<FollyQuicEventBase> qEvb_;
  folly::SocketAddress peerAddress{"127.0.0.1", 1234};
  std::unique_ptr<Aead> aead;
  std::unique_ptr<PacketNumberCipher> headerCipher;
};

TEST_F(DSRPacketizerSingleWriteTest, SingleWrite) {
  auto testBatchWriter = new test::TestPacketBatchWriter(16);
  auto batchWriter = BatchWriterPtr(testBatchWriter);
  auto socket =
      std::make_unique<NiceMock<quic::test::MockAsyncUDPSocket>>(qEvb_);
  PacketNum packetNum = 20;
  PacketNum largestAckedByPeer = 0;
  StreamId streamId = 0;
  size_t offset = 0;
  size_t length = 100;
  bool eof = false;
  auto dcid = test::getTestConnectionId();
  BufAccessor accessor{16 * kDefaultMaxUDPPayload};
  UdpSocketPacketGroupWriter packetGroupWriter(
      *socket, peerAddress, std::move(batchWriter));
  auto ret = packetGroupWriter.writeSingleQuicPacket(
      accessor,
      dcid,
      packetNum,
      largestAckedByPeer,
      *aead,
      *headerCipher,
      streamId,
      offset,
      length,
      eof,
      test::buildRandomInputData(5000));
  EXPECT_TRUE(ret);
  // This sucks. But i can't think of a better way to verify we do not
  // write a stream frame length into the packet.
  EXPECT_EQ(
      testBatchWriter->getBufSize(),
      1 /* short header initial byte */ + 1 /* packet num */ +
          dcid.size() /* dcid */ + 1 /* stream frame initial byte */ +
          1 /* stream id */ + length /* actual data */ +
          aead->getCipherOverhead());
  packetGroupWriter.getIOBufQuicBatch().flush();
  EXPECT_EQ(1, packetGroupWriter.getIOBufQuicBatch().getPktSent());
}

TEST_F(DSRPacketizerSingleWriteTest, NotEnoughData) {
  auto batchWriter = BatchWriterPtr(new test::TestPacketBatchWriter(16));
  auto socket =
      std::make_unique<NiceMock<quic::test::MockAsyncUDPSocket>>(qEvb_);
  UdpSocketPacketGroupWriter packetGroupWriter(
      *socket, peerAddress, std::move(batchWriter));
  PacketNum packetNum = 20;
  PacketNum largestAckedByPeer = 0;
  StreamId streamId = 0;
  size_t offset = 0;
  size_t length = 100;
  bool eof = false;
  BufAccessor accessor{16 * kDefaultMaxUDPPayload};
  auto ret = packetGroupWriter.writeSingleQuicPacket(
      accessor,
      test::getTestConnectionId(),
      packetNum,
      largestAckedByPeer,
      *aead,
      *headerCipher,
      streamId,
      offset,
      length,
      eof,
      folly::IOBuf::copyBuffer("Clif"));
  EXPECT_FALSE(ret);
  packetGroupWriter.getIOBufQuicBatch().flush();
  EXPECT_EQ(0, packetGroupWriter.getIOBufQuicBatch().getPktSent());
}

class DSRMultiWriteTest : public DSRCommonTestFixture {
  void SetUp() override {
    qEvb_ = std::make_shared<FollyQuicEventBase>(&evb_);
  }

 protected:
  FizzCryptoFactory factory_;
  folly::EventBase evb_;
  std::shared_ptr<FollyQuicEventBase> qEvb_;
};

TEST_F(DSRMultiWriteTest, TwoRequestsWithLoss) {
  prepareFlowControlAndStreamLimit();
  auto streamId = prepareOneStream(1000);
  auto stream = conn_.streamManager->findStream(streamId);
  // Pretend we sent the non DSR data
  stream->ackedIntervals.insert(0, stream->writeBuffer.chainLength() - 1);
  stream->currentWriteOffset = stream->writeBuffer.chainLength();
  stream->writeBuffer.move();
  ChainedByteRangeHead(std::move(stream->pendingWrites));
  conn_.streamManager->updateWritableStreams(*stream);
  auto bufMetaStartingOffset = stream->writeBufMeta.offset;
  // Move part of the BufMetas to lossBufMetas
  auto split = stream->writeBufMeta.split(500);
  stream->lossBufMetas.push_back(split);
  size_t packetLimit = 10;
  EXPECT_EQ(
      2,
      writePacketizationRequest(
          conn_, getTestConnectionId(), packetLimit, *aead_));
  EXPECT_EQ(2, countInstructions(streamId));
  EXPECT_EQ(2, conn_.outstandings.packets.size());
  auto& packet1 = conn_.outstandings.packets.front().packet;
  auto& packet2 = conn_.outstandings.packets.back().packet;
  EXPECT_EQ(1, packet1.frames.size());
  WriteStreamFrame expectedFirstFrame(
      streamId, bufMetaStartingOffset, 500, false, true, std::nullopt, 0);
  WriteStreamFrame expectedSecondFrame(
      streamId, 500 + bufMetaStartingOffset, 500, true, true, std::nullopt, 1);
  EXPECT_EQ(expectedFirstFrame, *packet1.frames[0].asWriteStreamFrame());
  EXPECT_EQ(expectedSecondFrame, *packet2.frames[0].asWriteStreamFrame());

  std::vector<Buf> sentData;
  auto sock = std::make_unique<NiceMock<quic::test::MockAsyncUDPSocket>>(qEvb_);
  EXPECT_CALL(*sock, writeGSO(conn_.peerAddress, _, _, _))
      .WillRepeatedly(Invoke([&](const folly::SocketAddress&,
                                 const struct iovec* vec,
                                 size_t iovec_len,
                                 QuicAsyncUDPSocket::WriteOptions) {
        sentData.push_back(copyChain(folly::IOBuf::wrapIov(vec, iovec_len)));
        return getTotalIovecLen(vec, iovec_len);
      }));
  EXPECT_CALL(*sock, write(conn_.peerAddress, _, _))
      .WillRepeatedly(Invoke([&](const folly::SocketAddress&,
                                 const struct iovec* vec,
                                 size_t iovec_len) {
        sentData.push_back(copyChain(folly::IOBuf::wrapIov(vec, iovec_len)));
        return getTotalIovecLen(vec, iovec_len);
      }));
  auto& instruction = pendingInstructions_.front();
  CipherBuilder builder;
  auto cipherPair = builder.buildCiphers(
      getFizzTestKey(),
      fizz::CipherSuite::TLS_AES_128_GCM_SHA256,
      packetProtectionKey_->clone());

  RequestGroup requests{
      instruction.dcid,
      instruction.scid,
      instruction.clientAddress,
      &cipherPair,
      {}};

  for (const auto& i : pendingInstructions_) {
    requests.requests.push_back(sendInstructionToPacketizationRequest(i));
  }

  UdpSocketPacketGroupWriter packetGroupWriter(*sock, requests.clientAddress);
  auto result = packetGroupWriter.writePacketsGroup(
      requests, [](const PacketizationRequest& req) {
        return buildRandomInputData(req.len);
      });
  EXPECT_EQ(2, result.packetsSent);
  EXPECT_EQ(2, sentData.size());
  EXPECT_GT(sentData[0]->computeChainDataLength(), 500);
  EXPECT_GT(sentData[1]->computeChainDataLength(), 500);
}

} // namespace quic::test
