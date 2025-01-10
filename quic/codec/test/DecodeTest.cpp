/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <quic/codec/Decode.h>

#include <folly/Random.h>
#include <folly/container/Array.h>
#include <folly/io/IOBuf.h>
#include <folly/portability/GTest.h>
#include <quic/codec/QuicReadCodec.h>
#include <quic/codec/Types.h>
#include <quic/common/test/TestUtils.h>
#include <ctime>

using namespace testing;

namespace quic::test {

using UnderlyingFrameType = std::underlying_type<FrameType>::type;

class DecodeTest : public Test {};

ShortHeader makeHeader() {
  PacketNum packetNum = 100;
  return ShortHeader(
      ProtectionType::KeyPhaseZero, getTestConnectionId(), packetNum);
}

// NormalizedAckBlocks are in order needed.
struct NormalizedAckBlock {
  QuicInteger gap; // Gap to previous AckBlock
  QuicInteger blockLen;

  NormalizedAckBlock(QuicInteger gapIn, QuicInteger blockLenIn)
      : gap(gapIn), blockLen(blockLenIn) {}
};

template <class LargestAckedType = uint64_t>
std::unique_ptr<folly::IOBuf> createAckFrame(
    Optional<QuicInteger> largestAcked,
    Optional<QuicInteger> ackDelay = none,
    Optional<QuicInteger> numAdditionalBlocks = none,
    Optional<QuicInteger> firstAckBlockLength = none,
    std::vector<NormalizedAckBlock> ackBlocks = {},
    bool useRealValuesForLargestAcked = false,
    bool useRealValuesForAckDelay = false,
    bool addEcnCounts = false) {
  std::unique_ptr<folly::IOBuf> ackFrame = folly::IOBuf::create(0);
  BufAppender wcursor(ackFrame.get(), 10);
  auto appenderOp = [&](auto val) { wcursor.writeBE(val); };
  if (largestAcked) {
    if (useRealValuesForLargestAcked) {
      wcursor.writeBE<LargestAckedType>(largestAcked->getValue());
    } else {
      largestAcked->encode(appenderOp);
    }
  }
  if (ackDelay) {
    if (useRealValuesForAckDelay) {
      wcursor.writeBE(ackDelay->getValue());
    } else {
      ackDelay->encode(appenderOp);
    }
  }
  if (numAdditionalBlocks) {
    numAdditionalBlocks->encode(appenderOp);
  }
  if (firstAckBlockLength) {
    firstAckBlockLength->encode(appenderOp);
  }
  for (size_t i = 0; i < ackBlocks.size(); ++i) {
    ackBlocks[i].gap.encode(appenderOp);
    ackBlocks[i].blockLen.encode(appenderOp);
  }
  if (addEcnCounts) {
    QuicInteger ect0(1); // ECT-0 count
    QuicInteger ect1(2); // ECT-1 count
    QuicInteger ce(3); // CE count
    ect0.encode(appenderOp);
    ect1.encode(appenderOp);
    ce.encode(appenderOp);
  }
  return ackFrame;
}

std::unique_ptr<folly::IOBuf> createRstStreamFrame(
    StreamId streamId,
    ApplicationErrorCode errorCode,
    uint64_t finalSize,
    folly::Optional<uint64_t> reliableSize = folly::none) {
  std::unique_ptr<folly::IOBuf> rstStreamFrame = folly::IOBuf::create(0);
  BufAppender wcursor(rstStreamFrame.get(), 10);
  auto appenderOp = [&](auto val) { wcursor.writeBE(val); };

  FrameType frameType =
      reliableSize ? FrameType::RST_STREAM_AT : FrameType::RST_STREAM;

  // Write the frame type
  QuicInteger frameTypeQuicInt(static_cast<uint8_t>(frameType));
  frameTypeQuicInt.encode(appenderOp);

  // Write the stream id
  QuicInteger streamIdQuicInt(streamId);
  streamIdQuicInt.encode(appenderOp);

  // Write the error code
  QuicInteger errorCodeQuicInt(static_cast<uint64_t>(errorCode));
  errorCodeQuicInt.encode(appenderOp);

  // Write the final size
  QuicInteger finalSizeQuicInt(finalSize);
  finalSizeQuicInt.encode(appenderOp);

  if (reliableSize) {
    // Write the reliable size
    QuicInteger reliableSizeQuicInt(*reliableSize);
    reliableSizeQuicInt.encode(appenderOp);
  }

  return rstStreamFrame;
}

template <class StreamIdType = StreamId>
std::unique_ptr<folly::IOBuf> createStreamFrame(
    Optional<QuicInteger> streamId,
    Optional<QuicInteger> offset = none,
    Optional<QuicInteger> dataLength = none,
    Buf data = nullptr,
    bool useRealValuesForStreamId = false,
    Optional<QuicInteger> groupId = none) {
  std::unique_ptr<folly::IOBuf> streamFrame = folly::IOBuf::create(0);
  BufAppender wcursor(streamFrame.get(), 10);
  auto appenderOp = [&](auto val) { wcursor.writeBE(val); };
  if (streamId) {
    if (useRealValuesForStreamId) {
      wcursor.writeBE<StreamIdType>(streamId->getValue());
    } else {
      streamId->encode(appenderOp);
    }
  }
  if (groupId) {
    groupId->encode(appenderOp);
  }
  if (offset) {
    offset->encode(appenderOp);
  }
  if (dataLength) {
    dataLength->encode(appenderOp);
  }
  if (data) {
    wcursor.insert(std::move(data));
  }
  return streamFrame;
}

std::unique_ptr<folly::IOBuf> createCryptoFrame(
    Optional<QuicInteger> offset = none,
    Optional<QuicInteger> dataLength = none,
    Buf data = nullptr) {
  std::unique_ptr<folly::IOBuf> cryptoFrame = folly::IOBuf::create(0);
  BufAppender wcursor(cryptoFrame.get(), 10);
  auto appenderOp = [&](auto val) { wcursor.writeBE(val); };
  if (offset) {
    offset->encode(appenderOp);
  }
  if (dataLength) {
    dataLength->encode(appenderOp);
  }
  if (data) {
    wcursor.insert(std::move(data));
  }
  return cryptoFrame;
}

std::unique_ptr<folly::IOBuf> createAckFrequencyFrame(
    Optional<QuicInteger> sequenceNumber,
    Optional<QuicInteger> packetTolerance,
    Optional<QuicInteger> maxAckDelay,
    Optional<QuicInteger> reorderThreshold) {
  QuicInteger intFrameType(static_cast<uint64_t>(FrameType::ACK_FREQUENCY));
  std::unique_ptr<folly::IOBuf> ackFrequencyFrame = folly::IOBuf::create(0);
  BufAppender wcursor(ackFrequencyFrame.get(), 50);
  auto appenderOp = [&](auto val) { wcursor.writeBE(val); };
  if (sequenceNumber) {
    sequenceNumber->encode(appenderOp);
  }
  if (packetTolerance) {
    packetTolerance->encode(appenderOp);
  }
  if (maxAckDelay) {
    maxAckDelay->encode(appenderOp);
  }
  if (reorderThreshold) {
    reorderThreshold->encode(appenderOp);
  }
  return ackFrequencyFrame;
}

TEST_F(DecodeTest, VersionNegotiationPacketDecodeTest) {
  ConnectionId srcCid = getTestConnectionId(0),
               destCid = getTestConnectionId(1);
  std::vector<QuicVersion> versions{
      {static_cast<QuicVersion>(1234),
       static_cast<QuicVersion>(4321),
       static_cast<QuicVersion>(2341),
       static_cast<QuicVersion>(3412),
       static_cast<QuicVersion>(4123)}};
  auto packet =
      VersionNegotiationPacketBuilder(srcCid, destCid, versions).buildPacket();
  auto codec = std::make_unique<QuicReadCodec>(QuicNodeType::Server);
  AckStates ackStates;
  auto packetQueue = bufToQueue(std::move(packet.second));
  auto versionPacket = codec->tryParsingVersionNegotiation(packetQueue);
  ASSERT_TRUE(versionPacket.has_value());
  EXPECT_EQ(versionPacket->destinationConnectionId, destCid);
  EXPECT_EQ(versionPacket->sourceConnectionId, srcCid);
  EXPECT_EQ(versionPacket->versions.size(), versions.size());
  EXPECT_EQ(versionPacket->versions, versions);
}

TEST_F(DecodeTest, DifferentCIDLength) {
  ConnectionId sourceConnectionId = getTestConnectionId();
  ConnectionId destinationConnectionId({1, 2, 3, 4, 5, 6});
  std::vector<QuicVersion> versions{
      {static_cast<QuicVersion>(1234),
       static_cast<QuicVersion>(4321),
       static_cast<QuicVersion>(2341),
       static_cast<QuicVersion>(3412),
       static_cast<QuicVersion>(4123)}};
  auto packet = VersionNegotiationPacketBuilder(
                    sourceConnectionId, destinationConnectionId, versions)
                    .buildPacket();
  auto codec = std::make_unique<QuicReadCodec>(QuicNodeType::Server);
  AckStates ackStates;
  auto packetQueue = bufToQueue(std::move(packet.second));
  auto versionPacket = codec->tryParsingVersionNegotiation(packetQueue);
  ASSERT_TRUE(versionPacket.has_value());
  EXPECT_EQ(versionPacket->sourceConnectionId, sourceConnectionId);
  EXPECT_EQ(versionPacket->destinationConnectionId, destinationConnectionId);
  EXPECT_EQ(versionPacket->versions.size(), versions.size());
  EXPECT_EQ(versionPacket->versions, versions);
}

TEST_F(DecodeTest, VersionNegotiationPacketBadPacketTest) {
  ConnectionId connId = getTestConnectionId();
  QuicVersionType version = static_cast<QuicVersionType>(QuicVersion::MVFST);

  auto buf = folly::IOBuf::create(10);
  folly::io::Appender appender(buf.get(), 10);
  appender.writeBE<uint8_t>(kHeaderFormMask);
  appender.push(connId.data(), connId.size());
  appender.writeBE<QuicVersionType>(
      static_cast<QuicVersionType>(QuicVersion::VERSION_NEGOTIATION));
  appender.push((uint8_t*)&version, sizeof(QuicVersion) - 1);

  auto codec = std::make_unique<QuicReadCodec>(QuicNodeType::Server);
  AckStates ackStates;
  auto packetQueue = bufToQueue(std::move(buf));
  auto packet = codec->parsePacket(packetQueue, ackStates);
  EXPECT_EQ(packet.regularPacket(), nullptr);

  buf = folly::IOBuf::create(0);
  packetQueue = bufToQueue(std::move(buf));
  packet = codec->parsePacket(packetQueue, ackStates);
  // Packet with empty versions
  EXPECT_EQ(packet.regularPacket(), nullptr);
}

TEST_F(DecodeTest, ValidAckFrame) {
  QuicInteger largestAcked(1000);
  QuicInteger ackDelay(100);
  QuicInteger numAdditionalBlocks(1);
  QuicInteger firstAckBlockLength(10);

  std::vector<NormalizedAckBlock> ackBlocks;
  ackBlocks.emplace_back(QuicInteger(10), QuicInteger(10));

  auto result = createAckFrame(
      largestAcked,
      ackDelay,
      numAdditionalBlocks,
      firstAckBlockLength,
      ackBlocks);
  folly::io::Cursor cursor(result.get());
  auto ackFrame = decodeAckFrame(
      cursor,
      makeHeader(),
      CodecParameters(kDefaultAckDelayExponent, QuicVersion::MVFST));
  EXPECT_EQ(ackFrame.ackBlocks.size(), 2);
  EXPECT_EQ(ackFrame.largestAcked, 1000);
  // Since 100 is the encoded value, we use the decoded value.
  EXPECT_EQ(ackFrame.ackDelay.count(), 100 << kDefaultAckDelayExponent);
}

TEST_F(DecodeTest, AckEcnFrame) {
  QuicInteger largestAcked(1000);
  QuicInteger ackDelay(100);
  QuicInteger numAdditionalBlocks(1);
  QuicInteger firstAckBlockLength(10);

  std::vector<NormalizedAckBlock> ackBlocks;
  ackBlocks.emplace_back(QuicInteger(10), QuicInteger(10));

  auto result = createAckFrame(
      largestAcked,
      ackDelay,
      numAdditionalBlocks,
      firstAckBlockLength,
      ackBlocks,
      false, // useRealValuesForLargestAcked
      false, // useRealValuesForAckDelay
      true); // addEcnCounts
  folly::io::Cursor cursor(result.get());
  auto ackFrame = decodeAckFrameWithECN(
      cursor,
      makeHeader(),
      CodecParameters(kDefaultAckDelayExponent, QuicVersion::MVFST));
  EXPECT_EQ(ackFrame.ackBlocks.size(), 2);
  EXPECT_EQ(ackFrame.largestAcked, 1000);
  // Since 100 is the encoded value, we use the decoded value.
  EXPECT_EQ(ackFrame.ackDelay.count(), 100 << kDefaultAckDelayExponent);

  // These values are hardcoded in the createAckFrame function
  EXPECT_EQ(ackFrame.ecnECT0Count, 1);
  EXPECT_EQ(ackFrame.ecnECT1Count, 2);
  EXPECT_EQ(ackFrame.ecnCECount, 3);
}

TEST_F(DecodeTest, AckFrameLargestAckExceedsRange) {
  // An integer larger than the representable range of quic integer.
  QuicInteger largestAcked(std::numeric_limits<uint64_t>::max());
  QuicInteger ackDelay(10);
  QuicInteger numAdditionalBlocks(0);
  QuicInteger firstAckBlockLength(10);
  auto result = createAckFrame(
      largestAcked,
      ackDelay,
      numAdditionalBlocks,
      firstAckBlockLength,
      {},
      true);
  folly::io::Cursor cursor(result.get());
  auto ackFrame = decodeAckFrame(
      cursor,
      makeHeader(),
      CodecParameters(kDefaultAckDelayExponent, QuicVersion::MVFST));
  // it will interpret this as a 8 byte range with the max value.
  EXPECT_EQ(ackFrame.largestAcked, 4611686018427387903);
}

TEST_F(DecodeTest, AckFrameLargestAckInvalid) {
  // An integer larger than the representable range of quic integer.
  QuicInteger largestAcked(std::numeric_limits<uint64_t>::max());
  QuicInteger ackDelay(10);
  QuicInteger numAdditionalBlocks(0);
  QuicInteger firstAckBlockLength(10);
  auto result = createAckFrame<uint8_t>(
      largestAcked,
      ackDelay,
      numAdditionalBlocks,
      firstAckBlockLength,
      {},
      true);
  folly::io::Cursor cursor(result.get());
  EXPECT_THROW(
      decodeAckFrame(
          cursor,
          makeHeader(),
          CodecParameters(kDefaultAckDelayExponent, QuicVersion::MVFST)),
      QuicTransportException);
}

TEST_F(DecodeTest, AckFrameDelayEncodingInvalid) {
  QuicInteger largestAcked(1000);
  // Maximal representable value by quic integer.
  QuicInteger ackDelay(4611686018427387903);
  QuicInteger numAdditionalBlocks(0);
  QuicInteger firstAckBlockLength(10);
  auto result = createAckFrame(
      largestAcked,
      ackDelay,
      numAdditionalBlocks,
      firstAckBlockLength,
      {},
      false,
      true);
  folly::io::Cursor cursor(result.get());
  EXPECT_THROW(
      decodeAckFrame(
          cursor,
          makeHeader(),
          CodecParameters(kDefaultAckDelayExponent, QuicVersion::MVFST)),
      QuicTransportException);
}

TEST_F(DecodeTest, AckFrameDelayExceedsRange) {
  QuicInteger largestAcked(1000);
  // Maximal representable value by quic integer.
  QuicInteger ackDelay(4611686018427387903);
  QuicInteger numAdditionalBlocks(0);
  QuicInteger firstAckBlockLength(10);
  auto result = createAckFrame(
      largestAcked, ackDelay, numAdditionalBlocks, firstAckBlockLength);
  folly::io::Cursor cursor(result.get());
  EXPECT_THROW(
      decodeAckFrame(
          cursor,
          makeHeader(),
          CodecParameters(kDefaultAckDelayExponent, QuicVersion::MVFST)),
      QuicTransportException);
}

TEST_F(DecodeTest, AckFrameAdditionalBlocksUnderflow) {
  QuicInteger largestAcked(1000);
  QuicInteger ackDelay(100);
  QuicInteger numAdditionalBlocks(2);
  QuicInteger firstAckBlockLength(10);

  std::vector<NormalizedAckBlock> ackBlocks;
  ackBlocks.emplace_back(QuicInteger(10), QuicInteger(10));

  auto result = createAckFrame(
      largestAcked,
      ackDelay,
      numAdditionalBlocks,
      firstAckBlockLength,
      ackBlocks);
  folly::io::Cursor cursor(result.get());
  EXPECT_THROW(
      decodeAckFrame(
          cursor,
          makeHeader(),
          CodecParameters(kDefaultAckDelayExponent, QuicVersion::MVFST)),
      QuicTransportException);
}

TEST_F(DecodeTest, AckFrameAdditionalBlocksOverflow) {
  QuicInteger largestAcked(1000);
  QuicInteger ackDelay(100);
  QuicInteger numAdditionalBlocks(2);
  QuicInteger firstAckBlockLength(10);

  std::vector<NormalizedAckBlock> ackBlocks;
  ackBlocks.emplace_back(QuicInteger(10), QuicInteger(10));
  ackBlocks.emplace_back(QuicInteger(10), QuicInteger(10));
  ackBlocks.emplace_back(QuicInteger(10), QuicInteger(10));

  auto result = createAckFrame(
      largestAcked,
      ackDelay,
      numAdditionalBlocks,
      firstAckBlockLength,
      ackBlocks);
  folly::io::Cursor cursor(result.get());
  decodeAckFrame(
      cursor,
      makeHeader(),
      CodecParameters(kDefaultAckDelayExponent, QuicVersion::MVFST));
}

TEST_F(DecodeTest, AckFrameMissingFields) {
  QuicInteger largestAcked(1000);
  QuicInteger ackDelay(100);
  QuicInteger numAdditionalBlocks(2);
  QuicInteger firstAckBlockLength(10);

  std::vector<NormalizedAckBlock> ackBlocks;
  ackBlocks.emplace_back(QuicInteger(10), QuicInteger(10));
  ackBlocks.emplace_back(QuicInteger(10), QuicInteger(10));

  auto result1 = createAckFrame(
      largestAcked, none, numAdditionalBlocks, firstAckBlockLength, ackBlocks);
  folly::io::Cursor cursor1(result1.get());

  EXPECT_THROW(
      decodeAckFrame(
          cursor1,
          makeHeader(),
          CodecParameters(kDefaultAckDelayExponent, QuicVersion::MVFST)),
      QuicTransportException);

  auto result2 = createAckFrame(
      largestAcked, ackDelay, none, firstAckBlockLength, ackBlocks);
  folly::io::Cursor cursor2(result2.get());
  EXPECT_THROW(
      decodeAckFrame(
          cursor2,
          makeHeader(),
          CodecParameters(kDefaultAckDelayExponent, QuicVersion::MVFST)),
      QuicTransportException);

  auto result3 = createAckFrame(
      largestAcked, ackDelay, none, firstAckBlockLength, ackBlocks);
  folly::io::Cursor cursor3(result3.get());
  EXPECT_THROW(
      decodeAckFrame(
          cursor3,
          makeHeader(),
          CodecParameters(kDefaultAckDelayExponent, QuicVersion::MVFST)),
      QuicTransportException);

  auto result4 = createAckFrame(
      largestAcked, ackDelay, numAdditionalBlocks, none, ackBlocks);
  folly::io::Cursor cursor4(result4.get());
  EXPECT_THROW(
      decodeAckFrame(
          cursor4,
          makeHeader(),
          CodecParameters(kDefaultAckDelayExponent, QuicVersion::MVFST)),
      QuicTransportException);

  auto result5 = createAckFrame(
      largestAcked, ackDelay, numAdditionalBlocks, firstAckBlockLength, {});
  folly::io::Cursor cursor5(result5.get());
  EXPECT_THROW(
      decodeAckFrame(
          cursor5,
          makeHeader(),
          CodecParameters(kDefaultAckDelayExponent, QuicVersion::MVFST)),
      QuicTransportException);
}

TEST_F(DecodeTest, AckFrameFirstBlockLengthInvalid) {
  QuicInteger largestAcked(1000);
  QuicInteger ackDelay(100);
  QuicInteger numAdditionalBlocks(0);
  QuicInteger firstAckBlockLength(2000);

  auto result = createAckFrame(
      largestAcked, ackDelay, numAdditionalBlocks, firstAckBlockLength);
  folly::io::Cursor cursor(result.get());
  EXPECT_THROW(
      decodeAckFrame(
          cursor,
          makeHeader(),
          CodecParameters(kDefaultAckDelayExponent, QuicVersion::MVFST)),
      QuicTransportException);
}

TEST_F(DecodeTest, AckFrameBlockLengthInvalid) {
  QuicInteger largestAcked(1000);
  QuicInteger ackDelay(100);
  QuicInteger numAdditionalBlocks(2);
  QuicInteger firstAckBlockLength(10);

  std::vector<NormalizedAckBlock> ackBlocks;
  ackBlocks.emplace_back(QuicInteger(10), QuicInteger(10));
  ackBlocks.emplace_back(QuicInteger(10), QuicInteger(1000));

  auto result = createAckFrame(
      largestAcked,
      ackDelay,
      numAdditionalBlocks,
      firstAckBlockLength,
      ackBlocks);
  folly::io::Cursor cursor(result.get());
  EXPECT_THROW(
      decodeAckFrame(
          cursor,
          makeHeader(),
          CodecParameters(kDefaultAckDelayExponent, QuicVersion::MVFST)),
      QuicTransportException);
}

TEST_F(DecodeTest, AckFrameBlockGapInvalid) {
  QuicInteger largestAcked(1000);
  QuicInteger ackDelay(100);
  QuicInteger numAdditionalBlocks(2);
  QuicInteger firstAckBlockLength(10);

  std::vector<NormalizedAckBlock> ackBlocks;
  ackBlocks.emplace_back(QuicInteger(10), QuicInteger(10));
  ackBlocks.emplace_back(QuicInteger(1000), QuicInteger(0));

  auto result = createAckFrame(
      largestAcked,
      ackDelay,
      numAdditionalBlocks,
      firstAckBlockLength,
      ackBlocks);
  folly::io::Cursor cursor(result.get());
  EXPECT_THROW(
      decodeAckFrame(
          cursor,
          makeHeader(),
          CodecParameters(kDefaultAckDelayExponent, QuicVersion::MVFST)),
      QuicTransportException);
}

TEST_F(DecodeTest, AckFrameBlockLengthZero) {
  QuicInteger largestAcked(1000);
  QuicInteger ackDelay(100);
  QuicInteger numAdditionalBlocks(3);
  QuicInteger firstAckBlockLength(10);

  std::vector<NormalizedAckBlock> ackBlocks;
  ackBlocks.emplace_back(QuicInteger(10), QuicInteger(10));
  ackBlocks.emplace_back(QuicInteger(10), QuicInteger(0));
  ackBlocks.emplace_back(QuicInteger(0), QuicInteger(10));

  auto result = createAckFrame(
      largestAcked,
      ackDelay,
      numAdditionalBlocks,
      firstAckBlockLength,
      ackBlocks);
  folly::io::Cursor cursor(result.get());

  auto readAckFrame = decodeAckFrame(
      cursor,
      makeHeader(),
      CodecParameters(kDefaultAckDelayExponent, QuicVersion::MVFST));
  EXPECT_EQ(readAckFrame.ackBlocks[0].endPacket, 1000);
  EXPECT_EQ(readAckFrame.ackBlocks[0].startPacket, 990);
  EXPECT_EQ(readAckFrame.ackBlocks[1].endPacket, 978);
  EXPECT_EQ(readAckFrame.ackBlocks[1].startPacket, 968);
  EXPECT_EQ(readAckFrame.ackBlocks[2].endPacket, 956);
  EXPECT_EQ(readAckFrame.ackBlocks[2].startPacket, 956);
  EXPECT_EQ(readAckFrame.ackBlocks[3].endPacket, 954);
  EXPECT_EQ(readAckFrame.ackBlocks[3].startPacket, 944);
}

TEST_F(DecodeTest, StreamDecodeSuccess) {
  QuicInteger streamId(10);
  QuicInteger offset(10);
  QuicInteger length(1);
  auto streamType =
      StreamTypeField::Builder().setFin().setOffset().setLength().build();
  auto streamFrame = createStreamFrame(
      streamId, offset, length, folly::IOBuf::copyBuffer("a"));
  BufQueue queue;
  queue.append(streamFrame->clone());
  auto decodedFrame = decodeStreamFrame(queue, streamType);
  EXPECT_EQ(decodedFrame.offset, 10);
  EXPECT_EQ(decodedFrame.data->computeChainDataLength(), 1);
  EXPECT_EQ(decodedFrame.streamId, 10);
  EXPECT_TRUE(decodedFrame.fin);
}

TEST_F(DecodeTest, StreamLengthStreamIdInvalid) {
  QuicInteger streamId(std::numeric_limits<uint64_t>::max());
  auto streamType =
      StreamTypeField::Builder().setFin().setOffset().setLength().build();
  auto streamFrame =
      createStreamFrame<uint8_t>(streamId, none, none, nullptr, true);
  BufQueue queue;
  queue.append(streamFrame->clone());
  EXPECT_THROW(decodeStreamFrame(queue, streamType), QuicTransportException);
}

TEST_F(DecodeTest, StreamOffsetNotPresent) {
  QuicInteger streamId(10);
  QuicInteger length(1);
  auto streamType =
      StreamTypeField::Builder().setFin().setOffset().setLength().build();
  auto streamFrame =
      createStreamFrame(streamId, none, length, folly::IOBuf::copyBuffer("a"));
  BufQueue queue;
  queue.append(streamFrame->clone());
  EXPECT_THROW(decodeStreamFrame(queue, streamType), QuicTransportException);
}

TEST_F(DecodeTest, StreamIncorrectDataLength) {
  QuicInteger streamId(10);
  QuicInteger offset(10);
  QuicInteger length(10);
  auto streamType =
      StreamTypeField::Builder().setFin().setOffset().setLength().build();
  auto streamFrame = createStreamFrame(
      streamId, offset, length, folly::IOBuf::copyBuffer("a"));
  BufQueue queue;
  queue.append(streamFrame->clone());
  EXPECT_THROW(decodeStreamFrame(queue, streamType), QuicTransportException);
}

TEST_F(DecodeTest, StreamNoRemainingData) {
  // assume after parsing the frame type (stream frame), there was no remaining
  // data
  quic::Buf buf = folly::IOBuf::copyBuffer("test");
  BufQueue queue(std::move(buf));
  queue.trimStartAtMost(4);

  const auto streamType =
      StreamTypeField(static_cast<uint8_t>(FrameType::STREAM));
  EXPECT_THROW(decodeStreamFrame(queue, streamType), QuicTransportException);
}

TEST_F(DecodeTest, DatagramNoRemainingData) {
  // assume after parsing the frame type (datagram frame), there was no
  // remaining data
  quic::Buf buf = folly::IOBuf::copyBuffer("test");
  BufQueue queue(std::move(buf));
  queue.trimStartAtMost(4);

  // invalid len
  EXPECT_THROW(decodeDatagramFrame(queue, true), QuicTransportException);
}

std::unique_ptr<folly::IOBuf> CreateMaxStreamsIdFrame(
    unsigned long long maxStreamsId) {
  std::unique_ptr<folly::IOBuf> buf = folly::IOBuf::create(sizeof(QuicInteger));
  BufAppender wcursor(buf.get(), sizeof(QuicInteger));
  auto appenderOp = [&](auto val) { wcursor.writeBE(val); };
  QuicInteger maxStreamsIdVal(maxStreamsId);
  maxStreamsIdVal.encode(appenderOp);
  return buf;
}

// Uni and BiDi have same max limits so uses single 'frame' to check both.
void MaxStreamsIdCheckSuccess(StreamId maxStreamsId) {
  std::unique_ptr<folly::IOBuf> buf = CreateMaxStreamsIdFrame(maxStreamsId);

  folly::io::Cursor cursorBiDi(buf.get());
  MaxStreamsFrame maxStreamsBiDiFrame = decodeBiDiMaxStreamsFrame(cursorBiDi);
  EXPECT_EQ(maxStreamsBiDiFrame.maxStreams, maxStreamsId);

  folly::io::Cursor cursorUni(buf.get());
  MaxStreamsFrame maxStreamsUniFrame = decodeUniMaxStreamsFrame(cursorUni);
  EXPECT_EQ(maxStreamsUniFrame.maxStreams, maxStreamsId);
}

// Uni and BiDi have same max limits so uses single 'frame' to check both.
void MaxStreamsIdCheckInvalid(StreamId maxStreamsId) {
  std::unique_ptr<folly::IOBuf> buf = CreateMaxStreamsIdFrame(maxStreamsId);

  folly::io::Cursor cursorBiDi(buf.get());
  EXPECT_THROW(decodeBiDiMaxStreamsFrame(cursorBiDi), QuicTransportException);

  folly::io::Cursor cursorUni(buf.get());
  EXPECT_THROW(decodeUniMaxStreamsFrame(cursorUni), QuicTransportException);
}

TEST_F(DecodeTest, MaxStreamsIdChecks) {
  MaxStreamsIdCheckSuccess(0);
  MaxStreamsIdCheckSuccess(123);
  MaxStreamsIdCheckSuccess(kMaxMaxStreams);

  MaxStreamsIdCheckInvalid(kMaxMaxStreams + 1);
  MaxStreamsIdCheckInvalid(kMaxMaxStreams + 123);
  MaxStreamsIdCheckInvalid(kMaxStreamId - 1);
}

TEST_F(DecodeTest, CryptoDecodeSuccess) {
  QuicInteger offset(10);
  QuicInteger length(1);
  auto cryptoFrame =
      createCryptoFrame(offset, length, folly::IOBuf::copyBuffer("a"));
  folly::io::Cursor cursor(cryptoFrame.get());
  auto decodedFrame = decodeCryptoFrame(cursor);
  EXPECT_EQ(decodedFrame.offset, 10);
  EXPECT_EQ(decodedFrame.data->computeChainDataLength(), 1);
}

TEST_F(DecodeTest, CryptoOffsetNotPresent) {
  QuicInteger length(1);
  auto cryptoFrame =
      createCryptoFrame(none, length, folly::IOBuf::copyBuffer("a"));
  folly::io::Cursor cursor(cryptoFrame.get());
  EXPECT_THROW(decodeCryptoFrame(cursor), QuicTransportException);
}

TEST_F(DecodeTest, CryptoLengthNotPresent) {
  QuicInteger offset(0);
  auto cryptoFrame = createCryptoFrame(offset, none, nullptr);
  folly::io::Cursor cursor(cryptoFrame.get());
  EXPECT_THROW(decodeCryptoFrame(cursor), QuicTransportException);
}

TEST_F(DecodeTest, CryptoIncorrectDataLength) {
  QuicInteger offset(10);
  QuicInteger length(10);
  auto cryptoFrame =
      createCryptoFrame(offset, length, folly::IOBuf::copyBuffer("a"));
  folly::io::Cursor cursor(cryptoFrame.get());
  EXPECT_THROW(decodeCryptoFrame(cursor), QuicTransportException);
}

TEST_F(DecodeTest, PaddingFrameTest) {
  auto buf = folly::IOBuf::create(sizeof(UnderlyingFrameType));
  buf->append(1);
  memset(buf->writableData(), 0, 1);

  folly::io::Cursor cursor(buf.get());
  decodePaddingFrame(cursor);
}

TEST_F(DecodeTest, PaddingFrameNoBytesTest) {
  auto buf = folly::IOBuf::create(sizeof(UnderlyingFrameType));

  folly::io::Cursor cursor(buf.get());
  decodePaddingFrame(cursor);
}

TEST_F(DecodeTest, DecodeMultiplePaddingInterleavedTest) {
  auto buf = folly::IOBuf::create(20);
  buf->append(10);
  memset(buf->writableData(), 0, 10);
  buf->append(1);
  // something which is not padding
  memset(buf->writableData() + 10, 5, 1);

  folly::io::Cursor cursor(buf.get());
  decodePaddingFrame(cursor);
  // If we encountered an interleaved frame, leave the whole thing
  // as is
  EXPECT_EQ(cursor.totalLength(), 11);
}

TEST_F(DecodeTest, DecodeMultiplePaddingTest) {
  auto buf = folly::IOBuf::create(20);
  buf->append(10);
  memset(buf->writableData(), 0, 10);

  folly::io::Cursor cursor(buf.get());
  decodePaddingFrame(cursor);
  EXPECT_EQ(cursor.totalLength(), 0);
}

std::unique_ptr<folly::IOBuf> createNewTokenFrame(
    Optional<QuicInteger> tokenLength = none,
    Buf token = nullptr) {
  std::unique_ptr<folly::IOBuf> newTokenFrame = folly::IOBuf::create(0);
  BufAppender wcursor(newTokenFrame.get(), 10);
  auto appenderOp = [&](auto val) { wcursor.writeBE(val); };
  if (tokenLength) {
    tokenLength->encode(appenderOp);
  }
  if (token) {
    wcursor.insert(std::move(token));
  }
  return newTokenFrame;
}

TEST_F(DecodeTest, NewTokenDecodeSuccess) {
  QuicInteger length(1);
  auto newTokenFrame =
      createNewTokenFrame(length, folly::IOBuf::copyBuffer("a"));
  folly::io::Cursor cursor(newTokenFrame.get());
  auto decodedFrame = decodeNewTokenFrame(cursor);
  EXPECT_EQ(decodedFrame.token->computeChainDataLength(), 1);
}

TEST_F(DecodeTest, NewTokenLengthNotPresent) {
  auto newTokenFrame = createNewTokenFrame(none, folly::IOBuf::copyBuffer("a"));
  folly::io::Cursor cursor(newTokenFrame.get());
  EXPECT_THROW(decodeNewTokenFrame(cursor), QuicTransportException);
}

TEST_F(DecodeTest, NewTokenIncorrectDataLength) {
  QuicInteger length(10);
  auto newTokenFrame =
      createNewTokenFrame(length, folly::IOBuf::copyBuffer("a"));
  folly::io::Cursor cursor(newTokenFrame.get());
  EXPECT_THROW(decodeNewTokenFrame(cursor), QuicTransportException);
}

TEST_F(DecodeTest, ParsePlaintextNewToken) {
  folly::IPAddress clientIp("127.0.0.1");
  uint64_t timestampInMs =
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count();

  NewToken newToken(clientIp, timestampInMs);
  Buf plaintextNewToken = newToken.getPlaintextToken();

  folly::io::Cursor cursor(plaintextNewToken.get());

  auto parseResult = parsePlaintextRetryOrNewToken(cursor);

  EXPECT_TRUE(parseResult.hasValue());

  EXPECT_EQ(parseResult.value(), timestampInMs);
}

TEST_F(DecodeTest, ParsePlaintextRetryToken) {
  ConnectionId odcid = getTestConnectionId();
  folly::IPAddress clientIp("109.115.3.49");
  uint16_t clientPort = 42069;
  uint64_t timestampInMs =
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count();

  RetryToken retryToken(odcid, clientIp, clientPort, timestampInMs);
  Buf plaintextRetryToken = retryToken.getPlaintextToken();

  folly::io::Cursor cursor(plaintextRetryToken.get());

  /**
   * Now we continue with the parsing logic here.
   */
  auto parseResult = parsePlaintextRetryOrNewToken(cursor);

  EXPECT_TRUE(parseResult.hasValue());

  EXPECT_EQ(parseResult.value(), timestampInMs);
}

TEST_F(DecodeTest, StreamGroupDecodeSuccess) {
  QuicInteger streamId(10);
  QuicInteger groupId(20);
  QuicInteger offset(10);
  QuicInteger length(1);
  auto streamType = StreamTypeField::Builder()
                        .switchToStreamGroups()
                        .setFin()
                        .setOffset()
                        .setLength()
                        .build();

  auto streamFrame = createStreamFrame(
      streamId,
      offset,
      length,
      folly::IOBuf::copyBuffer("a"),
      false /* useRealValuesForStreamId */,
      groupId);
  BufQueue queue;
  queue.append(streamFrame->clone());
  auto decodedFrame =
      decodeStreamFrame(queue, streamType, true /* isGroupFrame */);
  EXPECT_EQ(decodedFrame.offset, 10);
  EXPECT_EQ(decodedFrame.data->computeChainDataLength(), 1);
  EXPECT_EQ(decodedFrame.streamId, 10);
  EXPECT_EQ(*decodedFrame.streamGroupId, 20);
  EXPECT_TRUE(decodedFrame.fin);
}

TEST_F(DecodeTest, AckFrequencyFrameDecodeValid) {
  QuicInteger sequenceNumber(1);
  QuicInteger packetTolerance(100);
  QuicInteger maxAckDelay(100000); // 100 ms
  QuicInteger reorderThreshold(50);
  auto ackFrequencyFrame = createAckFrequencyFrame(
      sequenceNumber, packetTolerance, maxAckDelay, reorderThreshold);
  ASSERT_NE(ackFrequencyFrame, nullptr);

  folly::io::Cursor cursor(ackFrequencyFrame.get());
  auto decodedFrame = decodeAckFrequencyFrame(cursor);
  EXPECT_EQ(decodedFrame.sequenceNumber, 1);
  EXPECT_EQ(decodedFrame.packetTolerance, 100);
  EXPECT_EQ(decodedFrame.updateMaxAckDelay, 100000);
  EXPECT_EQ(decodedFrame.reorderThreshold, 50);
}

TEST_F(DecodeTest, AckFrequencyFrameDecodeInvalidReserved) {
  QuicInteger sequenceNumber(1);
  QuicInteger packetTolerance(100);
  QuicInteger maxAckDelay(100000); // 100 ms
  auto ackFrequencyFrame = createAckFrequencyFrame(
      sequenceNumber, packetTolerance, maxAckDelay, none);
  ASSERT_NE(ackFrequencyFrame, nullptr);

  folly::io::Cursor cursor(ackFrequencyFrame.get());
  EXPECT_THROW(decodeAckFrequencyFrame(cursor), QuicTransportException);
}

TEST_F(DecodeTest, RstStreamFrame) {
  auto buf = createRstStreamFrame(0, 0, 10);
  BufQueue queue(std::move(buf));
  auto frame = parseFrame(
      queue,
      makeHeader(),
      CodecParameters(kDefaultAckDelayExponent, QuicVersion::MVFST));
  auto rstStreamFrame = frame.asRstStreamFrame();
  EXPECT_EQ(rstStreamFrame->streamId, 0);
  EXPECT_EQ(rstStreamFrame->errorCode, 0);
  EXPECT_EQ(rstStreamFrame->finalSize, 10);
  EXPECT_FALSE(rstStreamFrame->reliableSize.hasValue());
}

TEST_F(DecodeTest, RstStreamAtFrame) {
  auto buf = createRstStreamFrame(0, 0, 10, 9);
  BufQueue queue(std::move(buf));
  auto frame = parseFrame(
      queue,
      makeHeader(),
      CodecParameters(kDefaultAckDelayExponent, QuicVersion::MVFST));
  auto rstStreamFrame = frame.asRstStreamFrame();
  EXPECT_EQ(rstStreamFrame->streamId, 0);
  EXPECT_EQ(rstStreamFrame->errorCode, 0);
  EXPECT_EQ(rstStreamFrame->finalSize, 10);
  EXPECT_EQ(*rstStreamFrame->reliableSize, 9);
}

TEST_F(DecodeTest, RstStreamAtFrameRelSizeGreaterThanOffset) {
  auto buf = createRstStreamFrame(0, 0, 10, 11);
  BufQueue queue(std::move(buf));
  EXPECT_THROW(
      parseFrame(
          queue,
          makeHeader(),
          CodecParameters(kDefaultAckDelayExponent, QuicVersion::MVFST)),
      QuicTransportException);
}

TEST_F(DecodeTest, RstStreamAtTruncated) {
  auto buf = createRstStreamFrame(0, 0, 10, 9);
  buf->coalesce();
  buf->trimEnd(1);
  BufQueue queue(std::move(buf));
  EXPECT_THROW(
      parseFrame(
          queue,
          makeHeader(),
          CodecParameters(kDefaultAckDelayExponent, QuicVersion::MVFST)),
      QuicTransportException);
}

} // namespace quic::test
