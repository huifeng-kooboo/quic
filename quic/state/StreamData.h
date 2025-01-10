/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <folly/container/F14Map.h>
#include <folly/container/F14Set.h>
#include <quic/QuicConstants.h>
#include <quic/codec/Types.h>
#include <quic/common/SmallCollections.h>
#include <quic/dsr/DSRPacketizationRequestSender.h>
#include <quic/state/QuicPriorityQueue.h>

namespace quic {

/**
 * A buffer representation without the actual data. This is part of the public
 * facing interface.
 *
 * This is experimental.
 */
struct BufferMeta {
  size_t length;

  explicit BufferMeta(size_t lengthIn) : length(lengthIn) {}
};

/**
 * A write buffer representation without the actual data. This is used for
 * write buffer management in a stream.
 *
 * This is experimental.
 */
struct WriteBufferMeta {
  size_t length{0};
  size_t offset{0};
  bool eof{false};

  WriteBufferMeta() = default;

  struct Builder {
    Builder& setLength(size_t lengthIn) {
      length_ = lengthIn;
      return *this;
    }

    Builder& setOffset(size_t offsetIn) {
      offset_ = offsetIn;
      return *this;
    }

    Builder& setEOF(bool val) {
      eof_ = val;
      return *this;
    }

    WriteBufferMeta build() {
      return WriteBufferMeta(length_, offset_, eof_);
    }

   private:
    size_t length_{0};
    size_t offset_{0};
    bool eof_{false};
  };

  WriteBufferMeta split(size_t splitLen) {
    CHECK_GE(length, splitLen);
    auto splitEof = splitLen == length && eof;
    WriteBufferMeta splitOf(splitLen, offset, splitEof);
    offset += splitLen;
    length -= splitLen;
    return splitOf;
  }

 private:
  explicit WriteBufferMeta(size_t lengthIn, size_t offsetIn, bool eofIn)
      : length(lengthIn), offset(offsetIn), eof(eofIn) {}
};

struct StreamBuffer {
  BufQueue data;
  uint64_t offset;
  bool eof{false};

  StreamBuffer(Buf dataIn, uint64_t offsetIn, bool eofIn = false) noexcept
      : data(std::move(dataIn)), offset(offsetIn), eof(eofIn) {}

  StreamBuffer(StreamBuffer&& other) = default;
  StreamBuffer& operator=(StreamBuffer&& other) = default;
};

struct WriteStreamBuffer {
  ChainedByteRangeHead data;
  uint64_t offset;
  bool eof{false};

  WriteStreamBuffer(
      ChainedByteRangeHead&& dataIn,
      uint64_t offsetIn,
      bool eofIn = false) noexcept
      : data(std::move(dataIn)), offset(offsetIn), eof(eofIn) {}

  WriteStreamBuffer(WriteStreamBuffer&& other)
      : data(std::move(other.data)), offset(other.offset), eof(other.eof) {}

  WriteStreamBuffer& operator=(WriteStreamBuffer&& other) noexcept {
    data = std::move(other.data);
    offset = other.offset;
    eof = other.eof;
    return *this;
  }
};

struct QuicStreamLike {
  QuicStreamLike() = default;

  QuicStreamLike(QuicStreamLike&&) = default;

  virtual ~QuicStreamLike() = default;

  // List of bytes that have been read and buffered. We need to buffer
  // bytes in case we get bytes out of order.
  CircularDeque<StreamBuffer> readBuffer;

  // List of bytes that have been written to the QUIC layer.
  uint64_t writeBufferStartOffset{0};
  BufQueue writeBuffer{};
  ChainedByteRangeHead pendingWrites{};

  // Stores a map of offset:buffers which have been written to the socket and
  // are currently un-acked. Each one represents one StreamFrame that was
  // written. We need to buffer these because these might be retransmitted in
  // the future. These are associated with the starting offset of the buffer.
  folly::F14FastMap<uint64_t, std::unique_ptr<WriteStreamBuffer>>
      retransmissionBuffer;

  // Tracks intervals which we have received ACKs for. E.g. in the case of all
  // data being acked this would contain one internval from 0 -> the largest
  // offset ACKed. This allows us to track which delivery callbacks can be
  // called.
  template <class T>
  using IntervalSetVec = SmallVec<T, 3>;
  using AckedIntervals = IntervalSet<uint64_t, 1, IntervalSetVec>;
  AckedIntervals ackedIntervals;

  // Stores a list of buffers which have been marked as loss by loss detector.
  // Each one represents one StreamFrame that was written.
  CircularDeque<WriteStreamBuffer> lossBuffer;

  // Current offset of the start bytes in the pending writes chain.
  // This changes when we pop stuff off the pendingWrites chain.
  // In a non-DSR stream, when we are finished writing out all the bytes until
  // FIN, this will be one greater than finalWriteOffset.
  // When DSR is used, this still points to the starting bytes in the write
  // buffer. Its value won't change with WriteBufferMetas are appended and sent
  // for a stream.
  uint64_t currentWriteOffset{0};

  // the minimum offset requires retransmit
  uint64_t minimumRetransmittableOffset{0};

  // Offset of the next expected bytes that we need to read from
  // the read buffer.
  uint64_t currentReadOffset{0};

  // the smallest data offset that we expect the peer to send.
  uint64_t currentReceiveOffset{0};

  // Maximum byte offset observed on the stream.
  uint64_t maxOffsetObserved{0};

  // If an EOF is observed on the stream, the position of the EOF. It could be
  // either from FIN or RST. Right now we use one value to represent both FIN
  // and RST. We may split write EOF into two values in the future.
  // Read side eof offset.
  Optional<uint64_t> finalReadOffset;

  // This is set if we send a RELIABLE_RESET_STREAM frame to the peer. If we
  // subsequently send a RESET_STREAM frame, we reset this value to none.
  Optional<uint64_t> reliableSizeToPeer;

  // This is set if we send a RESET_STREAM or RELIABLE_RESET_STREAM frame to
  // the peer. We store this in order to ensure that we use the same value of
  // the application error code for all resets we send to the peer, as mandated
  // by the spec.
  Optional<ApplicationErrorCode> appErrorCodeToPeer;

  // Current cumulative number of packets sent for this stream. It only counts
  // egress packets that contains a *new* STREAM frame for this stream.
  uint64_t numPacketsTxWithNewData{0};

  void updateAckedIntervals(uint64_t offset, uint64_t len, bool eof) {
    // When there's an EOF we count the byte of 1 past the end as having been
    // ACKed, since this is useful for delivery APIs.
    int lenAdjustment = [eof]() {
      if (eof) {
        return 0;
      } else {
        return 1;
      }
    }();
    if (lenAdjustment && len == 0) {
      LOG(FATAL) << "ACK for empty stream frame with no fin.";
    }
    ackedIntervals.insert(offset, offset + len - lenAdjustment);
  }

  /*
   * Either insert a new entry into the loss buffer, or merge the buffer with
   * an existing entry.
   */
  void insertIntoLossBuffer(std::unique_ptr<WriteStreamBuffer> buf) {
    // We assume here that we won't try to insert an overlapping buffer, as
    // that should never happen in the loss buffer.
    auto lossItr = std::upper_bound(
        lossBuffer.begin(),
        lossBuffer.end(),
        buf->offset,
        [](auto offset, const auto& buffer) { return offset < buffer.offset; });
    if (!lossBuffer.empty() && lossItr != lossBuffer.begin() &&
        std::prev(lossItr)->offset + std::prev(lossItr)->data.chainLength() ==
            buf->offset) {
      std::prev(lossItr)->data.append(std::move(buf->data));
      std::prev(lossItr)->eof = buf->eof;
    } else {
      lossBuffer.emplace(lossItr, std::move(*buf));
    }
  }

  void removeFromLossBuffer(uint64_t offset, size_t len, bool eof) {
    if (lossBuffer.empty() || len == 0) {
      // Nothing to do.
      return;
    }
    auto lossItr = lossBuffer.begin();
    for (; lossItr != lossBuffer.end(); lossItr++) {
      uint64_t lossStartOffset = lossItr->offset;
      uint64_t lossEndOffset = lossItr->offset + lossItr->data.chainLength();
      uint64_t removedStartOffset = offset;
      uint64_t removedEndOffset = offset + len;
      if (lossStartOffset > removedEndOffset) {
        return;
      }
      // There's two cases. If the removed offset lies within the existing
      // StreamBuffer then we need to potentially split it and remove that
      // section. The other case is that the existing StreamBuffer is completely
      // accounted for by the removed section, in which case it will be removed.
      // Note that this split/trim logic relies on the fact that insertion into
      // the loss buffer will merge contiguous elements, thus allowing us to
      // make these assumptions.
      if ((removedStartOffset >= lossStartOffset &&
           removedEndOffset <= lossEndOffset) ||
          (lossStartOffset >= removedStartOffset &&
           lossEndOffset <= removedEndOffset)) {
        size_t amountToSplit = removedStartOffset > lossStartOffset
            ? static_cast<size_t>(removedStartOffset - lossStartOffset)
            : 0;
        ChainedByteRangeHead splitBuf;
        if (amountToSplit > 0) {
          splitBuf = lossItr->data.splitAtMost(amountToSplit);
          lossItr->offset += amountToSplit;
        }
        lossItr->offset += lossItr->data.trimStartAtMost(len);
        if (lossItr->data.empty() && lossItr->eof == eof) {
          lossBuffer.erase(lossItr);
        }
        if (!splitBuf.empty()) {
          insertIntoLossBuffer(std::make_unique<WriteStreamBuffer>(
              std::move(splitBuf), lossStartOffset, false));
        }
        return;
      }
    }
  }

  void removeFromLossBufStartingAtOffset(uint64_t startingOffset) {
    while (!lossBuffer.empty()) {
      auto& lastElement = lossBuffer.back();
      if (lastElement.offset >= startingOffset) {
        lossBuffer.pop_back();
      } else if (
          lastElement.offset + lastElement.data.chainLength() >=
          startingOffset) {
        lastElement.data = lastElement.data.splitAtMost(
            size_t(startingOffset - lastElement.offset));
        return;
      } else {
        return;
      }
    }
  }

  void removeFromRetransmissionBufStartingAtOffset(uint64_t startingOffset) {
    folly::F14FastSet<uint64_t> offsetsToRemove;

    for (auto& [offset, buf] : retransmissionBuffer) {
      if (offset >= startingOffset) {
        offsetsToRemove.insert(offset);
      } else if (offset + buf->data.chainLength() >= startingOffset) {
        buf->data = buf->data.splitAtMost(size_t(startingOffset - offset));
      }
    }

    for (auto offset : offsetsToRemove) {
      retransmissionBuffer.erase(offset);
    }
  }

  void removeFromWriteBufStartingAtOffset(uint64_t startingOffset) {
    if (writeBuffer.empty() ||
        writeBufferStartOffset + writeBuffer.chainLength() <= startingOffset) {
      return;
    }

    if (startingOffset > writeBufferStartOffset) {
      writeBuffer = writeBuffer.splitAtMost(
          size_t(startingOffset - writeBufferStartOffset));
    } else {
      // Equivalent to clearing out the writeBuffer
      writeBuffer.splitAtMost(writeBuffer.chainLength());
    }
  }

  void removeFromPendingWritesStartingAtOffset(uint64_t startingOffset) {
    if (pendingWrites.empty() ||
        currentWriteOffset + pendingWrites.chainLength() <= startingOffset) {
      return;
    }

    if (startingOffset > currentWriteOffset) {
      pendingWrites = pendingWrites.splitAtMost(
          size_t(startingOffset - currentWriteOffset));
    } else {
      // Equivalent to clearing out the pendingWrites
      pendingWrites.splitAtMost(pendingWrites.chainLength());
    }
  }
};

struct QuicConnectionStateBase;

enum class StreamSendState : uint8_t { Open, ResetSent, Closed, Invalid };

enum class StreamRecvState : uint8_t { Open, Closed, Invalid };

inline folly::StringPiece streamStateToString(StreamSendState state) {
  switch (state) {
    case StreamSendState::Open:
      return "Open";
    case StreamSendState::ResetSent:
      return "ResetSent";
    case StreamSendState::Closed:
      return "Closed";
    case StreamSendState::Invalid:
      return "Invalid";
  }
  return "Unknown";
}

inline folly::StringPiece streamStateToString(StreamRecvState state) {
  switch (state) {
    case StreamRecvState::Open:
      return "Open";
    case StreamRecvState::Closed:
      return "Closed";
    case StreamRecvState::Invalid:
      return "Invalid";
  }
  return "Unknown";
}

struct QuicStreamState : public QuicStreamLike {
  virtual ~QuicStreamState() override = default;

  QuicStreamState(StreamId id, QuicConnectionStateBase& conn);

  QuicStreamState(
      StreamId idIn,
      const OptionalIntegral<StreamGroupId>& groupIdIn,
      QuicConnectionStateBase& connIn);

  QuicStreamState(QuicStreamState&&) = default;

  /**
   * Constructor to migrate QuicStreamState to another
   * QuicConnectionStateBase.
   */
  QuicStreamState(QuicConnectionStateBase& connIn, QuicStreamState&& other)
      : QuicStreamLike(std::move(other)),
        conn(connIn),
        id(other.id),
        groupId(other.groupId) {
    // QuicStreamState fields
    finalWriteOffset = other.finalWriteOffset;
    flowControlState = other.flowControlState;
    streamReadError = other.streamReadError;
    streamWriteError = other.streamWriteError;
    sendState = other.sendState;
    recvState = other.recvState;
    isControl = other.isControl;
    lastHolbTime = other.lastHolbTime;
    totalHolbTime = other.totalHolbTime;
    holbCount = other.holbCount;
    priority = other.priority;
    dsrSender = std::move(other.dsrSender);
    writeBufMeta = other.writeBufMeta;
    retransmissionBufMetas = std::move(other.retransmissionBufMetas);
    lossBufMetas = std::move(other.lossBufMetas);
    streamLossCount = other.streamLossCount;
  }

  // Connection that this stream is associated with.
  QuicConnectionStateBase& conn;

  // Stream id of the connection.
  StreamId id;

  // ID of the group the stream belongs to.
  OptionalIntegral<StreamGroupId> groupId;

  // Write side eof offset. This represents only the final FIN offset.
  Optional<uint64_t> finalWriteOffset;

  struct StreamFlowControlState {
    uint64_t windowSize{0};
    uint64_t advertisedMaxOffset{0};
    uint64_t peerAdvertisedMaxOffset{0};
    // Time at which the last flow control update was sent by the transport.
    Optional<TimePoint> timeOfLastFlowControlUpdate;
    // A flag indicating if the stream has a pending blocked frame to the peer
    // (blocked frame sent, but a stream flow control update has not been
    // received yet). Set when we write a blocked data frame on the stream;
    // cleared when we receive a flow control update for the stream.
    bool pendingBlockedFrame{false};
  };

  StreamFlowControlState flowControlState;

  // Stream level read error occurred.
  Optional<QuicErrorCode> streamReadError;
  // Stream level write error occurred.
  Optional<QuicErrorCode> streamWriteError;

  // State machine data
  StreamSendState sendState{StreamSendState::Open};

  // State machine data
  StreamRecvState recvState{StreamRecvState::Open};

  // Tells whether this stream is a control stream.
  // It is set by the app via setControlStream and the transport can use this
  // knowledge for optimizations e.g. for setting the app limited state on
  // congestion control with control streams still active.
  bool isControl{false};

  // The last time we detected we were head of line blocked on the stream.
  Optional<Clock::time_point> lastHolbTime;

  // The total amount of time we are head line blocked on the stream.
  std::chrono::microseconds totalHolbTime{0us};

  // Number of times the stream has entered the HOLB state
  // lastHolbTime indicates whether the stream is HOL blocked at the moment.
  uint32_t holbCount{0};

  Priority priority{kDefaultPriority};

  // This monotonically increases by 1 this stream is written to packets. Note
  // that this is only used for DSR and facilitates loss detection.
  uint64_t streamPacketIdx{0};

  // Returns true if both send and receive state machines are in a terminal
  // state
  bool inTerminalStates() const {
    bool sendInTerminalState = sendState == StreamSendState::Closed ||
        sendState == StreamSendState::Invalid;

    bool recvInTerminalState = recvState == StreamRecvState::Closed ||
        recvState == StreamRecvState::Invalid;

    return sendInTerminalState && recvInTerminalState;
  }

  // If the stream is still writable.
  bool writable() const {
    return sendState == StreamSendState::Open && !finalWriteOffset.has_value();
  }

  bool shouldSendFlowControl() const {
    return recvState == StreamRecvState::Open;
  }

  // If the stream has writable data that's not backed by DSR. That is, in a
  // regular stream write, it will be able to write something. So it either
  // needs to have data in the pendingWrites chain, or it has EOF to send.
  bool hasWritableData() const {
    if (!pendingWrites.empty()) {
      CHECK_GE(flowControlState.peerAdvertisedMaxOffset, currentWriteOffset);
      return flowControlState.peerAdvertisedMaxOffset - currentWriteOffset > 0;
    }
    if (finalWriteOffset) {
      // We can only write a FIN with a non-DSR stream frame if there's no
      // DSR data.
      return writeBufMeta.offset == 0 &&
          currentWriteOffset <= *finalWriteOffset;
    }
    return false;
  }

  // Whether this stream has non-DSR data in the write buffer or loss buffer.
  FOLLY_NODISCARD bool hasSchedulableData() const {
    return hasWritableData() || !lossBuffer.empty();
  }

  FOLLY_NODISCARD bool hasSchedulableDsr() const {
    return hasWritableBufMeta() || !lossBufMetas.empty();
  }

  FOLLY_NODISCARD bool hasWritableBufMeta() const {
    if (writeBufMeta.offset == 0) {
      return false;
    }
    if (writeBufMeta.length > 0) {
      CHECK_GE(flowControlState.peerAdvertisedMaxOffset, writeBufMeta.offset);
      return flowControlState.peerAdvertisedMaxOffset - writeBufMeta.offset > 0;
    }
    if (finalWriteOffset) {
      return writeBufMeta.offset <= *finalWriteOffset;
    }
    return false;
  }

  FOLLY_NODISCARD bool hasSentFIN() const {
    if (!finalWriteOffset) {
      return false;
    }
    return currentWriteOffset > *finalWriteOffset ||
        writeBufMeta.offset > *finalWriteOffset;
  }

  FOLLY_NODISCARD bool hasLoss() const {
    return !lossBuffer.empty() || !lossBufMetas.empty();
  }

  FOLLY_NODISCARD uint64_t nextOffsetToWrite() const {
    // The stream has never had WriteBufferMetas. Then currentWriteOffset
    // always points to the next offset we send. This of course relies on the
    // current contract of DSR: Real data always comes first. This code (and a
    // lot other code) breaks when that contract is breached.
    if (writeBufMeta.offset == 0) {
      return currentWriteOffset;
    }
    if (!pendingWrites.empty()) {
      return currentWriteOffset;
    }
    return writeBufMeta.offset;
  }

  bool hasReadableData() const {
    return (readBuffer.size() > 0 &&
            currentReadOffset == readBuffer.front().offset) ||
        (finalReadOffset && currentReadOffset == *finalReadOffset);
  }

  bool hasPeekableData() const {
    return readBuffer.size() > 0;
  }

  void removeFromWriteBufMetaStartingAtOffset(uint64_t startingOffset) {
    if (startingOffset <= writeBufMeta.offset) {
      writeBufMeta.length = 0;
      return;
    }

    if (startingOffset > writeBufMeta.offset &&
        startingOffset <= writeBufMeta.offset + writeBufMeta.length) {
      writeBufMeta.length = uint32_t(startingOffset - writeBufMeta.offset);
    }
  }

  void removeFromRetransmissionBufMetasStartingAtOffset(
      uint64_t startingOffset) {
    folly::F14FastSet<uint64_t> offsetsToRemove;

    for (auto& [offset, buf] : retransmissionBufMetas) {
      if (offset >= startingOffset) {
        offsetsToRemove.insert(offset);
      } else if (offset + buf.length >= startingOffset) {
        buf.length = size_t(startingOffset - offset);
      }
    }

    for (auto offset : offsetsToRemove) {
      retransmissionBufMetas.erase(offset);
    }
  }

  void removeFromLossBufMetasStartingAtOffset(uint64_t startingOffset) {
    if (lossBufMetas.empty()) {
      // Nothing to do.
      return;
    }

    while (!lossBufMetas.empty()) {
      auto& lastElement = lossBufMetas.back();
      if (lastElement.offset >= startingOffset) {
        lossBufMetas.pop_back();
      } else if (lastElement.offset + lastElement.length >= startingOffset) {
        lastElement.length = uint32_t(startingOffset - lastElement.offset);
        return;
      } else {
        return;
      }
    }
  }

  std::unique_ptr<DSRPacketizationRequestSender> dsrSender;

  // BufferMeta that has been written to the QUIC layer.
  // When offset is 0, nothing has been written to it. On first write, its
  // starting offset will be currentWriteOffset + pendingWrites.chainLength().
  WriteBufferMeta writeBufMeta;

  // A map to store sent WriteBufferMetas for potential retransmission.
  folly::F14FastMap<uint64_t, WriteBufferMeta> retransmissionBufMetas;

  // WriteBufferMetas that's already marked lost. They will be retransmitted.
  CircularDeque<WriteBufferMeta> lossBufMetas;

  uint64_t streamLossCount{0};

  /**
   * Insert a new WriteBufferMeta into lossBufMetas. If the new WriteBufferMeta
   * can be append to an existing WriteBufferMeta, it will be appended. Note
   * it won't be prepended to an existing WriteBufferMeta. And it will also not
   * merge 3 WriteBufferMetas together if the new one happens to fill up a hole
   * between 2 existing WriteBufferMetas.
   */
  void insertIntoLossBufMeta(WriteBufferMeta bufMeta) {
    auto lossItr = std::upper_bound(
        lossBufMetas.begin(),
        lossBufMetas.end(),
        bufMeta.offset,
        [](auto offset, const auto& wBufMeta) {
          return offset < wBufMeta.offset;
        });
    if (!lossBufMetas.empty() && lossItr != lossBufMetas.begin() &&
        std::prev(lossItr)->offset + std::prev(lossItr)->length ==
            bufMeta.offset) {
      std::prev(lossItr)->length += bufMeta.length;
      std::prev(lossItr)->eof = bufMeta.eof;
    } else {
      lossBufMetas.insert(lossItr, bufMeta);
    }
  }
};
} // namespace quic
