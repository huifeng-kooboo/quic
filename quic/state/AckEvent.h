/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <quic/codec/Types.h>
#include <quic/common/Optional.h>
#include <quic/common/SmallCollections.h>
#include <quic/congestion_control/CongestionController.h>
#include <quic/state/OutstandingPacket.h>

namespace quic {

struct AckEvent {
  struct AckPacket;

  /**
   * Returns the AckPacket associated with the largestAckedPacket.
   *
   * The largestAckedPacket is included in the AckFrame received from sender.
   *
   * Can be used to get packet metadata, including send time, app limited state,
   * and other aspects.
   *
   * If the OutstandingPacketWrapper with the largestAckedPacket packet number
   * had already been acked or removed from the list of list of
   * OutstandingPackets, either due to being marked lost or acked by an earlier
   * AckEvent, then this information will be unavailable.
   */
  [[nodiscard]] const AckPacket* FOLLY_NULLABLE getLargestAckedPacket() const {
    for (const auto& packet : ackedPackets) {
      if (packet.packetNum == largestAckedPacket) {
        return &packet;
      }
    }
    return nullptr;
  }

  /**
   * Returns the AckPacket associated with the largestNewlyAckedPacket.
   *
   * Can be used to get packet metadata, including send time, app limited state,
   * and other aspects.
   */
  [[nodiscard]] const AckPacket* FOLLY_NULLABLE
  getLargestNewlyAckedPacket() const {
    if (!largestNewlyAckedPacket.has_value()) {
      return nullptr;
    }
    for (const auto& packet : ackedPackets) {
      if (packet.packetNum == largestNewlyAckedPacket) {
        return &packet;
      }
    }
    return nullptr;
  }

  // ack receive time
  const TimePoint ackTime;

  // ack receive time minus ack delay.
  const TimePoint adjustedAckTime;

  // ack delay
  //
  // the ack delay is the amount of time between the remote receiving
  // largestAckedPacket and the remote generating the AckFrame associated with
  // this AckEvent.
  //
  // different AckFrame can have the same largestAckedPacket with different ack
  // blocks (ranges) in the case of reordering; under such circumstances, you
  // cannot use the ack delay if the largestAckedPacket was already acknowledged
  // by a previous AckFrame.
  const std::chrono::microseconds ackDelay;

  // packet number space that acked packets are in.
  const PacketNumberSpace packetNumberSpace;

  // the largest acked packet included in the AckFrame received from sender.
  //
  // this may not be the same as largestNewlyAckedPacket (below) if the
  // OutstandingPacketWrapper with this packet number had already been removed
  // from the list of OutstandingPackets, either due to being marked lost or
  // acked.
  const PacketNum largestAckedPacket;

  // for all packets (newly) acked during this event, sum of encoded sizes
  // encoded size includes header and body
  //
  // this value does not directly translate to the number of stream bytes newly
  // acked; see the DetailsPerStream structure in each of the AckedPackets to
  // determine information about stream bytes.
  uint64_t ackedBytes{0};

  // total number of bytes acked on this connection after ACK processed.
  //
  // this value is the same as lossState.totalBytesAcked and does not
  // include bytea acked via implicit ACKs.
  uint64_t totalBytesAcked{0};

  // ECN mark counts reported by the peer up to and including this ACK.
  uint32_t ecnECT0Count{0};
  uint32_t ecnECT1Count{0};
  uint32_t ecnCECount{0};

  // the highest packet number newly acked during processing of this event.
  //
  // this may not be the same as the largestAckedPacket if the
  // OutstandingPacketWrapper with that packet number had already been acked or
  // removed from the list of list of OutstandingPackets, either due to being
  // marked lost or acked.
  //
  // the reason that this is an optional type is that we construct an
  // AckEvent first, then go through the acked packets that are still
  // outstanding and figure out the largest newly acked packet along the way.
  Optional<PacketNum> largestNewlyAckedPacket;

  // when largestNewlyAckedPacket was sent
  TimePoint largestNewlyAckedPacketSentTime;

  // RTT sample with ack delay included.
  //
  // not available if largestAckedPacket already acked or declared lost
  OptionalMicros rttSample;

  // RTT sample with ack delay removed.
  //
  // not available if largestAckedPacket already acked or declared lost
  OptionalMicros rttSampleNoAckDelay;

  // Congestion controller state after processing of AckEvent.
  //
  // Optional to handle cases where congestion controller not used.
  Optional<CongestionController::State> ccState;

  /**
   * Booleans grouped together to avoid padding.
   */

  // if this AckEvent came from an implicit ACK rather than a real one
  bool implicit{false};

  // whether the transport was app limited when largestNewlyAckedPacket was sent
  bool largestNewlyAckedPacketAppLimited{false};

  /**
   * Container to store information about ACKed packets
   */
  struct AckPacket {
    // Sequence number of previously outstanding (now acked) packet
    quic::PacketNum packetNum;
    uint64_t nonDsrPacketSequenceNumber{0};

    // Metadata of the previously outstanding (now acked) packet
    OutstandingPacketMetadata outstandingPacketMetadata;

    struct StreamDetails {
      Optional<uint64_t> streamPacketIdx;

      // definition for DupAckedStreamIntervalSet
      // we expect this to be rare, any thus only allocate a single position
      template <class T>
      using DupAckedStreamIntervalSetVec = SmallVec<T, 1 /* stack size */>;
      using DupAckedStreamIntervals =
          IntervalSet<uint64_t, 1, DupAckedStreamIntervalSetVec>;

      // Intervals that had already been ACKed.
      //
      // Requires ACK processing for packets spuriously marked lost is enabled
      DupAckedStreamIntervals dupAckedStreamIntervals;
    };

    // Structure with information about each stream with frames in ACKed packet
    using MapType = InlineMap<StreamId, StreamDetails, 5>;
    class DetailsPerStream : private MapType {
     public:
      /**
       * Record that a frame contained in ACKed packet was marked as delivered.
       *
       * Specifically, during processing of this ACK, we were able to fill in a
       * hole in the stream IntervalSet. This means that the intervals covered
       * by said frame had not been delivered by another packet.
       *
       * If said frame had previously been sent in some previous packet before
       * being sent in the packet that we are processing the ACK for now, then
       * we can conclude that a retransmission enabled this frame to be
       * delivered.
       *
       * See recordFrameAlreadyDelivered for the case where a frame contained in
       * an ACKed packet had already been marked as delivered.
       *
       * @param frame          The frame that is being processed.
       */
      void recordFrameDelivered(const WriteStreamFrame& frame);

      /**
       * Record that a frame had already been marked as delivered.
       *
       * This can occur if said frame was sent multiple times (e.g., in multiple
       * packets) and an ACK for a different packet containing the frame was
       * already processed. More specifically,, the hole in the stream
       * IntervalSet associated with this frame was marked as delivered when
       * some other packet's ACK was processed.
       *
       * Note that packet(s) carrying the frame may have been acknowledged at
       * the same time by the remote (e.g., in the same ACK block / message), in
       * which case we cannot discern "which" packet arrived first — we can only
       * state that multiple packets(s) carrying the same frame successfully
       * reached the remote.
       *
       * @param frame          The frame that is being processed and that was
       *                       marked as delivered by some previous packet.
       */
      void recordFrameAlreadyDelivered(const WriteStreamFrame& frame);

      [[nodiscard]] auto at(StreamId id) const {
        return MapType::at(id);
      }

      [[nodiscard]] auto begin() const {
        return cbegin();
      }

      [[nodiscard]] auto end() const {
        return cend();
      }

      using MapType::cbegin;
      using MapType::cend;
      using MapType::const_iterator;
      using MapType::empty;
      using MapType::find;
      using MapType::mapped_type;
      using MapType::size;
      using MapType::value_type;
    };

    // Details for each active stream that was impacted by an ACKed frame
    DetailsPerStream detailsPerStream;

    // LastAckedPacketInfo from this acked packet'r original sent
    // OutstandingPacketWrapper structure.
    Optional<OutstandingPacketWrapper::LastAckedPacketInfo> lastAckedPacketInfo;
    // Delta RX Timestamp of the current packet relative to the previous packet
    // (or connection start time).
    OptionalMicros receiveRelativeTimeStampUsec;

    // Whether this packet was sent when CongestionController is in
    // app-limited state.
    bool isAppLimited;

    struct Builder {
      Builder&& setPacketNum(quic::PacketNum packetNumIn);
      Builder&& setNonDsrPacketSequenceNumber(
          uint64_t nonDsrPacketSequenceNumberIn);
      Builder&& setOutstandingPacketMetadata(
          OutstandingPacketMetadata& outstandingPacketMetadataIn);
      Builder&& setDetailsPerStream(DetailsPerStream&& detailsPerStreamIn);
      Builder&& setLastAckedPacketInfo(
          OutstandingPacketWrapper::LastAckedPacketInfo* lastAckedPacketInfoIn);
      Builder&& setAppLimited(bool appLimitedIn);
      Builder&& setReceiveDeltaTimeStamp(
          OptionalMicros&& receiveRelativeTimeStampUsec);
      AckPacket build() &&;
      void buildInto(std::vector<AckPacket>& ackedPacketsVec) &&;
      explicit Builder() = default;

     private:
      Optional<quic::PacketNum> packetNum;
      Optional<uint64_t> nonDsrPacketSequenceNumber;
      OutstandingPacketMetadata* outstandingPacketMetadata{nullptr};
      Optional<DetailsPerStream> detailsPerStream;
      OutstandingPacketWrapper::LastAckedPacketInfo* lastAckedPacketInfo{
          nullptr};
      OptionalMicros receiveRelativeTimeStampUsec;
      bool isAppLimited{false};
    };

    // Better to use the Builder to construct. The reason we're not marking this
    // as private is because we need to be able to construct this in-place in
    // the AckEvent::Builder::buildInto, where we use emplace_back to emplace it
    // into the back of a vector.
    explicit AckPacket(
        quic::PacketNum packetNumIn,
        uint64_t nonDsrPacketSequenceNumberIn,
        const OutstandingPacketMetadata& outstandingPacketMetadataIn, // NOLINT
        const DetailsPerStream& detailsPerStreamIn, // NOLINT
        Optional<OutstandingPacketWrapper::LastAckedPacketInfo>
            lastAckedPacketInfoIn,
        bool isAppLimitedIn,
        OptionalMicros&& receiveRelativeTimeStampUsec);
  };

  // Information about each packet ACKed during this event
  std::vector<AckPacket> ackedPackets;

  struct BuilderFields {
    Optional<TimePoint> maybeAckTime;
    Optional<TimePoint> maybeAdjustedAckTime;
    OptionalMicros maybeAckDelay;
    Optional<PacketNumberSpace> maybePacketNumberSpace;
    Optional<PacketNum> maybeLargestAckedPacket;
    bool isImplicitAck{false};
    uint32_t ecnECT0Count{0};
    uint32_t ecnECT1Count{0};
    uint32_t ecnCECount{0};
    explicit BuilderFields() = default;
  };

  struct Builder : public BuilderFields {
    Builder&& setAckTime(TimePoint ackTimeIn);
    Builder&& setAdjustedAckTime(TimePoint adjustedAckTimeIn);
    Builder&& setAckDelay(std::chrono::microseconds ackDelay);
    Builder&& setPacketNumberSpace(PacketNumberSpace packetNumberSpaceIn);
    Builder&& setLargestAckedPacket(PacketNum largestAckedPacketIn);
    Builder&& setIsImplicitAck(bool isImplicitAckIn);
    Builder&& setEcnCounts(
        uint32_t ecnECT0CountIn,
        uint32_t ecnECT1CountIn,
        uint32_t ecnCECountIn);
    AckEvent build() &&;
    explicit Builder() = default;
  };

  // Use builder to construct.
  explicit AckEvent(BuilderFields&& fields);
};

} // namespace quic
