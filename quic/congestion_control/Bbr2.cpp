/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <quic/congestion_control/Bbr2.h>

#include <quic/congestion_control/CongestionControlFunctions.h>
#include <sys/types.h>
#include <chrono>
#include <cstdint>
#include <limits>

namespace quic {

constexpr uint64_t kMaxBwFilterLen = 2; // Measured in number of ProbeBW cycles
constexpr std::chrono::microseconds kMinRttFilterLen = 10s;
constexpr std::chrono::microseconds kProbeRTTInterval = 5s;
constexpr std::chrono::microseconds kProbeRttDuration = 200ms;
constexpr uint64_t kMaxExtraAckedFilterLen =
    10; // Measured in packet-timed round trips

constexpr float kStartupPacingGain = 2.89; // 2 / ln(2)
constexpr float kDrainPacingGain = 0.5;
constexpr float kProbeBwDownPacingGain = 0.9;
constexpr float kProbeBwCruiseRefillPacingGain = 1.0;
constexpr float kProbeBwUpPacingGain = 1.25;
constexpr float kProbeRttPacingGain = 1.0;

constexpr float kStartupCwndGain = 2.89;
constexpr float kProbeBwCruiseRefillCwndGain = 2.0;
constexpr float kProbeBwDownCwndGain = 2.0;
constexpr float kProbeBwUpCwndGain = 2.25;
constexpr float kProbeRttCwndGain = 0.5;

constexpr float kBeta = 0.7;

constexpr float kLossThreshold = 0.02;
constexpr float kHeadroomFactor = 0.15;

// The experimental pacer currently achieves ~99% of the target rate
// we should not reduce the target by adding an extra margin.
// TODO: add the margin back if the pacer performance improves further.
constexpr uint8_t kPacingMarginPercent = 0;

Bbr2CongestionController::Bbr2CongestionController(
    QuicConnectionStateBase& conn)
    : conn_(conn),
      // WindowedFilter window_length is expiry time which inflates the window
      // length by 1
      maxBwFilter_(kMaxBwFilterLen - 1, Bandwidth(), 0),
      probeRttMinTimestamp_(Clock::now()),
      maxExtraAckedFilter_(kMaxExtraAckedFilterLen, 0, 0),
      cwndBytes_(
          conn_.udpSendPacketLen * conn_.transportSettings.initCwndInMss) {
  resetCongestionSignals();
  resetFullBw();
  resetLowerBounds();
  // If we explicitly don't want to pace the init cwnd, reset the pacing rate.
  // Otherwise, leave it to the pacer's initial state.
  if (!conn_.transportSettings.ccaConfig.paceInitCwnd) {
    if (conn_.pacer) {
      conn_.pacer->refreshPacingRate(cwndBytes_, 0us);
    } else {
      LOG(WARNING) << "BBR2 was initialized on a connection without a pacer";
    }
  }
  enterStartup();
}

// Congestion Controller Interface

void Bbr2CongestionController::onRemoveBytesFromInflight(
    uint64_t bytesToRemove) {
  subtractAndCheckUnderflow(conn_.lossState.inflightBytes, bytesToRemove);
}

void Bbr2CongestionController::onPacketSent(
    const OutstandingPacketWrapper& packet) {
  // Handle restart from idle
  if (conn_.lossState.inflightBytes == 0 && isAppLimited()) {
    idleRestart_ = true;
    extraAckedStartTimestamp_ = Clock::now();
    extraAckedDelivered_ = 0;

    if (isProbeBwState(state_)) {
      setPacing();
    } else if (state_ == State::ProbeRTT) {
      checkProbeRttDone();
    }
  }

  addAndCheckOverflow(
      conn_.lossState.inflightBytes, packet.metadata.encodedSize);

  // Maintain cwndLimited flag. We consider the transport being cwnd limited if
  // we are using > 90% of the cwnd.
  if (conn_.lossState.inflightBytes > cwndBytes_ * 9 / 10) {
    cwndLimitedInRound_ |= true;
  }
}

void Bbr2CongestionController::onPacketAckOrLoss(
    const AckEvent* FOLLY_NULLABLE ackEvent,
    const LossEvent* FOLLY_NULLABLE lossEvent) {
  if (conn_.qLogger) {
    conn_.qLogger->addCongestionMetricUpdate(
        conn_.lossState.inflightBytes,
        getCongestionWindow(),
        kCongestionPacketAck,
        bbr2StateToString(state_));
    conn_.qLogger->addNetworkPathModelUpdate(
        inflightHi_.value_or(0),
        inflightLo_.value_or(0),
        0, // bandwidthHi_ no longer available.
        std::chrono::microseconds(1), // bandwidthHi_ no longer available.
        bandwidthLo_.has_value() ? bandwidthLo_->units : 0,
        bandwidthLo_.has_value() ? bandwidthLo_->interval
                                 : std::chrono::microseconds(1));
  }
  if (ackEvent) {
    subtractAndCheckUnderflow(
        conn_.lossState.inflightBytes, ackEvent->ackedBytes);
  }
  if (lossEvent) {
    subtractAndCheckUnderflow(
        conn_.lossState.inflightBytes, lossEvent->lostBytes);
  }
  SCOPE_EXIT {
    VLOG(6) << "State=" << bbr2StateToString(state_)
            << " inflight=" << conn_.lossState.inflightBytes
            << " cwnd=" << getCongestionWindow() << "(gain=" << cwndGain_
            << ")";
  };

  if (lossEvent && lossEvent->lostPackets > 0 &&
      conn_.transportSettings.ccaConfig.conservativeRecovery) {
    // The pseudo code in BBRHandleLostPacket is included in
    // updateProbeBwCyclePhase. No need to repeat it here.

    // We don't expose the loss type here, so always use fast recovery for
    // non-persistent congestion
    saveCwnd();
    inRecovery_ = true;
    // Mark the connection as app-limited so bw samples during recovery are not
    // taken into account.
    setAppLimited();
    recoveryStartTime_ = Clock::now();
    if (lossEvent->persistentCongestion) {
      cwndBytes_ = kMinCwndInMssForBbr * conn_.udpSendPacketLen;
    } else {
      auto newlyAcked = ackEvent ? ackEvent->ackedBytes : 0;
      cwndBytes_ = conn_.lossState.inflightBytes +
          std::max(newlyAcked, conn_.udpSendPacketLen);
    }
  }

  if (ackEvent) {
    if (ackEvent->implicit) {
      // Implicit acks should not be used for bandwidth or rtt estimation
      setCwnd(ackEvent->ackedBytes, 0);
      return;
    }

    if (appLimited_ &&
        appLimitedLastSendTime_ <= ackEvent->largestNewlyAckedPacketSentTime) {
      appLimited_ = false;
      if (conn_.qLogger) {
        conn_.qLogger->addAppUnlimitedUpdate();
      }
    }

    if (inRecovery_ &&
        recoveryStartTime_ <= ackEvent->largestNewlyAckedPacketSentTime) {
      inRecovery_ = false;
      restoreCwnd();
    }

    // UpdateModelAndState
    updateLatestDeliverySignals(*ackEvent);
    updateRound(*ackEvent);

    // Update cwndLimited state
    if (roundStart_) {
      cwndLimitedInRound_ = false;
    }

    updateCongestionSignals(lossEvent);
    updateAckAggregation(*ackEvent);
    checkFullBwReached();
    checkStartupDone();
    checkDrain();

    auto inflightBytesAtLargestAckedPacket =
        ackEvent->getLargestNewlyAckedPacket()
        ? ackEvent->getLargestNewlyAckedPacket()
              ->outstandingPacketMetadata.inflightBytes
        : conn_.lossState.inflightBytes;
    auto lostBytes = lossEvent ? lossEvent->lostBytes : 0;
    updateProbeBwCyclePhase(
        ackEvent->ackedBytes, inflightBytesAtLargestAckedPacket, lostBytes);
    updateMinRtt();
    checkProbeRtt(ackEvent->ackedBytes);
    advanceLatestDeliverySignals(*ackEvent);
    boundBwForModel();

    // UpdateControlParameters
    setPacing();
    setSendQuantum();
    setCwnd(ackEvent->ackedBytes, lostBytes);
  }
}

uint64_t Bbr2CongestionController::getWritableBytes() const noexcept {
  return getCongestionWindow() > conn_.lossState.inflightBytes
      ? getCongestionWindow() - conn_.lossState.inflightBytes
      : 0;
}

uint64_t Bbr2CongestionController::getCongestionWindow() const noexcept {
  return cwndBytes_;
}

CongestionControlType Bbr2CongestionController::type() const noexcept {
  return CongestionControlType::BBR2;
}

bool Bbr2CongestionController::isInBackgroundMode() const {
  return false;
}

Optional<Bandwidth> Bbr2CongestionController::getBandwidth() const {
  return bandwidth_;
}

bool Bbr2CongestionController::isAppLimited() const {
  return appLimited_;
}

void Bbr2CongestionController::setAppLimited() noexcept {
  appLimited_ = true;
  appLimitedLastSendTime_ = Clock::now();
  if (conn_.qLogger) {
    conn_.qLogger->addAppLimitedUpdate();
  }
}

// Internals
void Bbr2CongestionController::resetCongestionSignals() {
  lossBytesInRound_ = 0;
  lossEventsInRound_ = 0;
  bandwidthLatest_ = Bandwidth();
  inflightLatest_ = 0;
}

void Bbr2CongestionController::resetLowerBounds() {
  bandwidthLo_.reset();
  inflightLo_.reset();
}
void Bbr2CongestionController::enterStartup() {
  state_ = State::Startup;
  updatePacingAndCwndGain();
}

void Bbr2CongestionController::setPacing() {
  if (!conn_.transportSettings.ccaConfig.paceInitCwnd &&
      conn_.lossState.totalBytesSent <
          conn_.transportSettings.initCwndInMss * conn_.udpSendPacketLen) {
    return;
  }
  uint64_t pacingWindow =
      bandwidth_ * minRtt_ * pacingGain_ * (100 - kPacingMarginPercent) / 100;
  VLOG(6) << "Setting pacing to "
          << Bandwidth(pacingWindow, minRtt_).normalizedDescribe()
          << " from bandwidth_=" << bandwidth_.normalizedDescribe()
          << " pacingGain_=" << pacingGain_
          << " kPacingMarginPercent=" << kPacingMarginPercent
          << " units=" << pacingWindow << " interval=" << minRtt_.count();

  if (state_ == State::Startup && !fullBwReached_) {
    pacingWindow = std::max(
        pacingWindow,
        conn_.udpSendPacketLen * conn_.transportSettings.initCwndInMss);
  }
  conn_.pacer->refreshPacingRate(pacingWindow, minRtt_);
}

void Bbr2CongestionController::setSendQuantum() {
  auto rate = bandwidth_ * pacingGain_ * (100 - kPacingMarginPercent) / 100;
  auto burstInPacerTick = rate * conn_.transportSettings.pacingTickInterval;
  sendQuantum_ =
      std::min(burstInPacerTick, decltype(burstInPacerTick)(64 * 1024));
  sendQuantum_ = std::max(sendQuantum_, 2 * conn_.udpSendPacketLen);
}

void Bbr2CongestionController::setCwnd(
    uint64_t ackedBytes,
    uint64_t /*lostBytes*/) {
  // BBRUpdateMaxInflight()
  auto inflightMax = addQuantizationBudget(
      getBDPWithGain(cwndGain_) + maxExtraAckedFilter_.GetBest());

  if (fullBwReached_) {
    cwndBytes_ = std::min(cwndBytes_ + ackedBytes, inflightMax);
  } else if (
      cwndBytes_ < inflightMax ||
      conn_.lossState.totalBytesAcked <
          conn_.transportSettings.initCwndInMss * conn_.udpSendPacketLen) {
    cwndBytes_ += ackedBytes;
  }
  cwndBytes_ =
      std::max(cwndBytes_, kMinCwndInMssForBbr * conn_.udpSendPacketLen);

  // BBRBoundCwndForProbeRTT()
  if (state_ == State::ProbeRTT) {
    cwndBytes_ = std::min(cwndBytes_, getProbeRTTCwnd());
  }

  // BBRBoundCwndForModel()
  auto cap = std::numeric_limits<uint64_t>::max();
  if (inflightHi_.has_value() &&
      !conn_.transportSettings.ccaConfig.ignoreInflightHi) {
    if (isProbeBwState(state_) && state_ != State::ProbeBw_Cruise) {
      cap = *inflightHi_;
    } else if (state_ == State::ProbeRTT || state_ == State::ProbeBw_Cruise) {
      cap = getTargetInflightWithHeadroom();
    }
  }
  if (inflightLo_.has_value() &&
      !conn_.transportSettings.ccaConfig.ignoreLoss) {
    cap = std::min(cap, *inflightLo_);
  }
  cap = std::max(cap, kMinCwndInMssForBbr * conn_.udpSendPacketLen);
  cwndBytes_ = std::min(cwndBytes_, cap);
}

void Bbr2CongestionController::checkProbeRttDone() {
  auto timeNow = Clock::now();
  if ((probeRttDoneTimestamp_ && timeNow > *probeRttDoneTimestamp_) ||
      conn_.lossState.inflightBytes == 0) {
    // Schedule the next ProbeRTT
    probeRttMinTimestamp_ = timeNow;
    restoreCwnd();
    exitProbeRtt();
  }
}

void Bbr2CongestionController::restoreCwnd() {
  cwndBytes_ = std::max(cwndBytes_, previousCwndBytes_);
  VLOG(6) << "Restored cwnd: " << cwndBytes_;
}
void Bbr2CongestionController::exitProbeRtt() {
  resetLowerBounds();
  if (fullBwReached_) {
    startProbeBwDown();
    startProbeBwCruise();
  } else {
    enterStartup();
  }
}

void Bbr2CongestionController::updateLatestDeliverySignals(
    const AckEvent& ackEvent) {
  lossRoundStart_ = false;

  bandwidthLatest_ =
      std::max(bandwidthLatest_, getBandwidthSampleFromAck(ackEvent));
  VLOG(6) << "Bandwidth latest=" << bandwidthLatest_.normalizedDescribe()
          << "  AppLimited=" << bandwidthLatest_.isAppLimited;
  inflightLatest_ = std::max(inflightLatest_, bandwidthLatest_.units);

  auto pkt = ackEvent.getLargestNewlyAckedPacket();
  if (pkt &&
      pkt->outstandingPacketMetadata.totalBytesSent > lossRoundEndBytesSent_) {
    // Uses bytes sent instead of ACKed in the spec. This doesn't affect the
    // round counting
    lossPctInLastRound_ = static_cast<float>(lossBytesInRound_) /
        static_cast<float>(conn_.lossState.totalBytesSent -
                           lossRoundEndBytesSent_);
    lossEventsInLastRound_ = lossEventsInRound_;
    lossRoundEndBytesSent_ = conn_.lossState.totalBytesSent;
    lossRoundStart_ = true;
  }
}

void Bbr2CongestionController::updateCongestionSignals(
    const LossEvent* FOLLY_NULLABLE lossEvent) {
  // Update max bandwidth
  if (bandwidthLatest_ > maxBwFilter_.GetBest() ||
      !bandwidthLatest_.isAppLimited) {
    VLOG(6) << "Updating bandwidth filter with sample: "
            << bandwidthLatest_.normalizedDescribe();
    maxBwFilter_.Update(bandwidthLatest_, cycleCount_);
  }

  // Update loss signal
  if (lossEvent && lossEvent->lostBytes > 0) {
    lossBytesInRound_ += lossEvent->lostBytes;
    lossEventsInRound_ += 1;
  }

  if (!lossRoundStart_) {
    return; // we're still within the same round
  }
  // AdaptLowerBoundsFromCongestion - once per round-trip
  if (state_ == State::ProbeBw_Up) {
    return;
  }
  if (lossBytesInRound_ > 0) {
    // InitLowerBounds
    if (!bandwidthLo_.has_value()) {
      bandwidthLo_ = maxBwFilter_.GetBest();
    }
    if (!inflightLo_.has_value()) {
      inflightLo_ = cwndBytes_;
    }

    // LossLowerBounds
    bandwidthLo_ = std::max(bandwidthLatest_, *bandwidthLo_ * kBeta);
    inflightLo_ =
        std::max(inflightLatest_, static_cast<uint64_t>(*inflightLo_ * kBeta));
  }

  lossBytesInRound_ = 0;
  lossEventsInRound_ = 0;
}

void Bbr2CongestionController::updateAckAggregation(const AckEvent& ackEvent) {
  /* Find excess ACKed beyond expected amount over this interval */
  auto interval =
      Clock::now() - extraAckedStartTimestamp_.value_or(TimePoint());
  auto expectedDelivered = bandwidth_ *
      std::chrono::duration_cast<std::chrono::microseconds>(interval);
  /* Reset interval if ACK rate is below expected rate: */
  if (extraAckedDelivered_ < expectedDelivered) {
    extraAckedDelivered_ = 0;
    extraAckedStartTimestamp_ = Clock::now();
    expectedDelivered = 0;
  }
  extraAckedDelivered_ += ackEvent.ackedBytes;
  auto extra = extraAckedDelivered_ - expectedDelivered;
  extra = std::min(extra, cwndBytes_);
  maxExtraAckedFilter_.Update(extra, roundCount_);
}
void Bbr2CongestionController::checkStartupDone() {
  checkStartupHighLoss();

  if (state_ == State::Startup && fullBwReached_) {
    enterDrain();
  }
}

void Bbr2CongestionController::checkStartupHighLoss() {
  /*
  Our implementation differs from the spec a bit here. The conditions in the
  spec are:
  1. The connection has been in fast recovery for at least one full packet-timed
  round trip.
  2. The loss rate over the time scale of a single full round trip exceeds
  BBRLossThresh (2%).
  3. There are at least BBRStartupFullLossCnt=6
  discontiguous sequence ranges lost in that round trip.

  For 1,2 we use the loss pct from the last loss round which means we could exit
  before a full RTT. For 3, we check we received three separate loss events
  which servers a similar purpose to discontiguous ranges but it's not exactly
  the same.
  */
  if (fullBwReached_ || !roundStart_ || isAppLimited() ||
      conn_.transportSettings.ccaConfig.ignoreLoss) {
    // TODO: the appLimited condition means we could tolerate losses in startup
    // if we haven't found the full bandwidth. This may need to be revisited.

    return; /* no need to check for a the loss exit condition now */
  }
  if (lossPctInLastRound_ > kLossThreshold && lossEventsInLastRound_ >= 6) {
    fullBwReached_ = true;
    inflightHi_ = std::max(getBDPWithGain(), inflightLatest_);
  }
}

void Bbr2CongestionController::checkFullBwReached() {
  if (fullBwNow_ || isAppLimited()) {
    return; /* no need to check for a full pipe now */
  }
  if (maxBwFilter_.GetBest() >= fullBw_ * 1.25) {
    resetFullBw(); // bw still growing, reset tracking
    fullBw_ = maxBwFilter_.GetBest(); /* record new baseline level */
    return;
  }
  if (!roundStart_) {
    return;
  }
  fullBwCount_++; /* another round w/o much growth */
  fullBwNow_ = (fullBwCount_ >= 3);
  if (fullBwNow_) {
    fullBwReached_ = true;
  }
}

void Bbr2CongestionController::resetFullBw() {
  fullBw_ = Bandwidth();
  fullBwNow_ = false;
  fullBwCount_ = 0;
}

void Bbr2CongestionController::enterDrain() {
  state_ = State::Drain;
  updatePacingAndCwndGain();
}

void Bbr2CongestionController::checkDrain() {
  if (state_ == State::Drain) {
    VLOG(6) << "Current inflight" << conn_.lossState.inflightBytes
            << " target inflight " << getTargetInflightWithGain(1.0);
  }
  if (state_ == State::Drain &&
      conn_.lossState.inflightBytes <= getTargetInflightWithGain(1.0)) {
    enterProbeBW(); /* BBR estimates the queue was drained */
  }
}
void Bbr2CongestionController::updateProbeBwCyclePhase(
    uint64_t ackedBytes,
    uint64_t inflightBytesAtLargestAckedPacket,
    uint64_t lostBytes) {
  /* The core state machine logic for ProbeBW: */
  if (!fullBwReached_) {
    return; /* only handling steady-state behavior here */
  }
  adaptUpperBounds(ackedBytes, inflightBytesAtLargestAckedPacket, lostBytes);
  if (!isProbeBwState(state_)) {
    return; /* only handling ProbeBW states here: */
  }
  switch (state_) {
    case State::ProbeBw_Down:
      if (checkTimeToProbeBW()) {
        return; /* already decided state transition */
      }
      if (checkTimeToCruise()) {
        startProbeBwCruise();
      }
      break;
    case State::ProbeBw_Cruise:
      if (checkTimeToProbeBW()) {
        return; /* already decided state transition */
      }
      break;
    case State::ProbeBw_Refill:
      /* After one round of REFILL, start UP */
      if (roundStart_) {
        // Enable one reaction to loss per probe bw cycle.
        bwProbeShouldHandleLoss_ = true;
        startProbeBwUp();
      }
      break;
    case State::ProbeBw_Up:
      if (checkTimeToGoDown()) {
        startProbeBwDown();
      }
      break;
    default:
      throw QuicInternalException(
          "BBR2: Unexpected state in ProbeBW phase: " +
              bbr2StateToString(state_),
          LocalErrorCode::CONGESTION_CONTROL_ERROR);
  }
}

void Bbr2CongestionController::adaptUpperBounds(
    uint64_t ackedBytes,
    uint64_t inflightBytesAtLargestAckedPacket,
    uint64_t lostBytes) {
  /* Update BBR.inflight_hi and BBR.bw_hi. */

  if (!checkInflightTooHigh(inflightBytesAtLargestAckedPacket, lostBytes)) {
    if (!inflightHi_.has_value()) {
      // No loss has occurred yet so these values are not set and do not need to
      // be raised.
      return;
    }
    /* There is loss but it's at safe levels. The limits are populated so we
     * update them */
    if (inflightBytesAtLargestAckedPacket > *inflightHi_) {
      inflightHi_ = inflightBytesAtLargestAckedPacket;
    }
    if (state_ == State::ProbeBw_Up) {
      probeInflightHiUpward(ackedBytes);
    }
  }
}

bool Bbr2CongestionController::checkTimeToProbeBW() {
  if (hasElapsedInPhase(bwProbeWait_) || isRenoCoexistenceProbeTime()) {
    startProbeBwRefill();
    return true;
  } else {
    return false;
  }
}

bool Bbr2CongestionController::checkTimeToCruise() {
  if (conn_.lossState.inflightBytes > getTargetInflightWithHeadroom()) {
    return false; /* not enough headroom */
  } else if (conn_.lossState.inflightBytes <= getTargetInflightWithGain()) {
    return true; /* inflight <= estimated BDP */
  }
  // Neither conditions met. Do not cruise yet.
  return false;
}

bool Bbr2CongestionController::checkTimeToGoDown() {
  if (cwndLimitedInRound_ && inflightHi_.has_value() &&
      cwndBytes_ >= inflightHi_.value()) {
    resetFullBw();
    fullBw_ = maxBwFilter_.GetBest();
  } else if (fullBwNow_) {
    return true;
  }
  return false;
}

bool Bbr2CongestionController::hasElapsedInPhase(
    std::chrono::microseconds interval) {
  return Clock::now() > probeBWCycleStart_ + interval;
}

// Was the loss percent too high for the last ack received?
bool Bbr2CongestionController::checkInflightTooHigh(
    uint64_t inflightBytesAtLargestAckedPacket,
    uint64_t lostBytes) {
  if (isInflightTooHigh(inflightBytesAtLargestAckedPacket, lostBytes)) {
    if (bwProbeShouldHandleLoss_) {
      handleInFlightTooHigh(inflightBytesAtLargestAckedPacket);
    }
    return true;
  } else {
    return false;
  }
}

bool Bbr2CongestionController::isInflightTooHigh(
    uint64_t inflightBytesAtLargestAckedPacket,
    uint64_t lostBytes) {
  return static_cast<float>(lostBytes) >
      static_cast<float>(inflightBytesAtLargestAckedPacket) * kLossThreshold;
}

void Bbr2CongestionController::handleInFlightTooHigh(
    uint64_t inflightBytesAtLargestAckedPacket) {
  bwProbeShouldHandleLoss_ = false;
  // TODO: Should this be the app limited state of the largest acknowledged
  // packet?
  if (!isAppLimited()) {
    inflightHi_ = std::max(
        inflightBytesAtLargestAckedPacket,
        static_cast<uint64_t>(
            static_cast<float>(getTargetInflightWithGain()) * kBeta));
  }
  if (state_ == State::ProbeBw_Up) {
    startProbeBwDown();
  }
}

uint64_t Bbr2CongestionController::getTargetInflightWithHeadroom() const {
  /* Return a volume of data that tries to leave free
   * headroom in the bottleneck buffer or link for
   * other flows, for fairness convergence and lower
   * RTTs and loss */
  if (!inflightHi_.has_value()) {
    return std::numeric_limits<uint64_t>::max();
  } else {
    auto headroom = static_cast<uint64_t>(
        std::max(1.0f, kHeadroomFactor * static_cast<float>(*inflightHi_)));
    return std::max(
        *inflightHi_ - headroom,
        quic::kMinCwndInMssForBbr * conn_.udpSendPacketLen);
  }
}

void Bbr2CongestionController::probeInflightHiUpward(uint64_t ackedBytes) {
  if (!inflightHi_.has_value() || !cwndLimitedInRound_ ||
      cwndBytes_ < *inflightHi_) {
    return; /* no inflight_hi set or not fully using inflight_hi, so don't grow
               it */
  }
  probeUpAcks_ += ackedBytes;
  if (probeUpAcks_ >= probeUpCount_) {
    auto delta = probeUpAcks_ / probeUpCount_;
    probeUpAcks_ -= delta * probeUpCount_;
    addAndCheckOverflow(*inflightHi_, delta);
  }
  if (roundStart_) {
    raiseInflightHiSlope();
  }
}

void Bbr2CongestionController::updateMinRtt() {
  if (idleRestart_) {
    probeRttMinTimestamp_ = Clock::now();
    probeRttMinValue_ = kDefaultMinRtt;
  }
  probeRttExpired_ = probeRttMinTimestamp_
      ? Clock::now() > (probeRttMinTimestamp_.value() + kProbeRTTInterval)
      : true;
  auto& lrtt = conn_.lossState.lrtt;
  if (lrtt > 0us && (lrtt < probeRttMinValue_ || probeRttExpired_)) {
    probeRttMinValue_ = lrtt;
    probeRttMinTimestamp_ = Clock::now();
  }

  auto minRttExpired = minRttTimestamp_
      ? Clock::now() > (minRttTimestamp_.value() + kMinRttFilterLen)
      : true;
  if (probeRttMinValue_ < minRtt_ || minRttExpired) {
    minRtt_ = probeRttMinValue_;
    minRttTimestamp_ = probeRttMinTimestamp_;
  }
}

void Bbr2CongestionController::checkProbeRtt(uint64_t ackedBytes) {
  if (state_ != State::ProbeRTT && probeRttExpired_ && !idleRestart_) {
    enterProbeRtt();
    saveCwnd();
    probeRttDoneTimestamp_.reset();
    startRound();
  }
  if (state_ == State::ProbeRTT) {
    handleProbeRtt();
  }
  if (ackedBytes > 0) {
    idleRestart_ = false;
  }
}

void Bbr2CongestionController::enterProbeRtt() {
  state_ = State::ProbeRTT;
  updatePacingAndCwndGain();
}

void Bbr2CongestionController::handleProbeRtt() {
  /* Ignore low rate samples during ProbeRTT: */
  // TODO: I don't understand the logic in the spec in
  // MarkConnectionAppLimited() but just setting app limited is reasonable
  setAppLimited();

  if (!probeRttDoneTimestamp_ &&
      conn_.lossState.inflightBytes <= getProbeRTTCwnd()) {
    /* Wait for at least ProbeRTTDuration to elapse: */
    probeRttDoneTimestamp_ = Clock::now() + kProbeRttDuration;
    /* Wait for at least one round to elapse: */
    // Is this needed? BBR.probe_rtt_round_done = false
    startRound();
  } else if (probeRttDoneTimestamp_) {
    if (roundStart_) {
      checkProbeRttDone();
    }
  }
}

void Bbr2CongestionController::advanceLatestDeliverySignals(
    const AckEvent& ackEvent) {
  if (lossRoundStart_) {
    bandwidthLatest_ = getBandwidthSampleFromAck(ackEvent);
    inflightLatest_ = bandwidthLatest_.units;
  }
}

uint64_t Bbr2CongestionController::getProbeRTTCwnd() {
  return std::max(
      getBDPWithGain(kProbeRttCwndGain),
      quic::kMinCwndInMssForBbr * conn_.udpSendPacketLen);
}
void Bbr2CongestionController::boundCwndForProbeRTT() {
  if (state_ == State::ProbeRTT) {
    cwndBytes_ = std::min(cwndBytes_, getProbeRTTCwnd());
  }
}

void Bbr2CongestionController::boundBwForModel() {
  Bandwidth previousBw = bandwidth_;
  bandwidth_ = maxBwFilter_.GetBest();
  if (state_ != State::Startup) {
    if (bandwidthLo_.has_value() &&
        !conn_.transportSettings.ccaConfig.ignoreLoss) {
      bandwidth_ = std::min(bandwidth_, *bandwidthLo_);
    }
  }
  if (conn_.qLogger && previousBw != bandwidth_) {
    conn_.qLogger->addBandwidthEstUpdate(bandwidth_.units, bandwidth_.interval);
  }
}

uint64_t Bbr2CongestionController::addQuantizationBudget(uint64_t input) const {
  // BBRUpdateOffloadBudget()
  auto offloadBudget = 3 * sendQuantum_;
  input = std::max(input, offloadBudget);
  input = std::max(input, quic::kMinCwndInMssForBbr * conn_.udpSendPacketLen);
  if (state_ == State::ProbeBw_Up) {
    // This number is arbitrary from the spec. It's probably to guarantee that
    // probing up is more aggressive (?)
    input += 2 * conn_.udpSendPacketLen;
  }
  return input;
}

void Bbr2CongestionController::saveCwnd() {
  if (!inLossRecovery_ && state_ != State::ProbeRTT) {
    previousCwndBytes_ = cwndBytes_;
  } else {
    previousCwndBytes_ = std::max(cwndBytes_, previousCwndBytes_);
  }
  VLOG(6) << "Saved cwnd: " << previousCwndBytes_;
}

uint64_t Bbr2CongestionController::getTargetInflightWithGain(float gain) const {
  return addQuantizationBudget(getBDPWithGain(gain));
}

uint64_t Bbr2CongestionController::getBDPWithGain(float gain) const {
  if (minRtt_ == kDefaultMinRtt) {
    return uint64_t(
        gain * conn_.transportSettings.initCwndInMss * conn_.udpSendPacketLen);
  } else {
    return uint64_t(gain * (minRtt_ * bandwidth_));
  }
}

void Bbr2CongestionController::enterProbeBW() {
  startProbeBwDown();
}

void Bbr2CongestionController::startRound() {
  nextRoundDelivered_ = conn_.lossState.totalBytesAcked;
}
void Bbr2CongestionController::updateRound(const AckEvent& ackEvent) {
  auto pkt = ackEvent.getLargestNewlyAckedPacket();
  if (pkt && pkt->lastAckedPacketInfo &&
      pkt->lastAckedPacketInfo->totalBytesAcked >= nextRoundDelivered_) {
    startRound();
    roundCount_++;
    roundsSinceBwProbe_++;
    roundStart_ = true;
  } else {
    roundStart_ = false;
  }
}

void Bbr2CongestionController::startProbeBwDown() {
  resetCongestionSignals();
  probeUpCount_ =
      std::numeric_limits<uint64_t>::max(); /* not growing inflight_hi */
  /* Decide random round-trip bound for wait: */
  roundsSinceBwProbe_ = folly::Random::rand32() % 2;
  /* Decide the random wall clock bound for wait: between 2-3 seconds */
  bwProbeWait_ =
      std::chrono::milliseconds(2000 + (folly::Random::rand32() % 1000));

  probeBWCycleStart_ = Clock::now();
  state_ = State::ProbeBw_Down;
  updatePacingAndCwndGain();
  startRound();

  // This is a new ProbeBW cycle. Advance the max bw filter if we're not app
  // limited
  if (!isAppLimited()) {
    cycleCount_++;
  }
}
void Bbr2CongestionController::startProbeBwCruise() {
  state_ = State::ProbeBw_Cruise;
  updatePacingAndCwndGain();
}

void Bbr2CongestionController::startProbeBwRefill() {
  resetLowerBounds();
  probeUpRounds_ = 0;
  probeUpAcks_ = 0;
  state_ = State::ProbeBw_Refill;
  updatePacingAndCwndGain();
  startRound();
}
void Bbr2CongestionController::startProbeBwUp() {
  probeBWCycleStart_ = Clock::now();
  state_ = State::ProbeBw_Up;
  updatePacingAndCwndGain();
  startRound();
  resetFullBw();
  raiseInflightHiSlope();
}

void Bbr2CongestionController::raiseInflightHiSlope() {
  auto growthThisRound = conn_.udpSendPacketLen << probeUpRounds_;
  probeUpRounds_ = std::min(probeUpRounds_ + 1, decltype(probeUpRounds_)(30));
  probeUpCount_ =
      std::max(cwndBytes_ / growthThisRound, decltype(cwndBytes_)(1));
}

// Utilities
bool Bbr2CongestionController::isProbeBwState(
    const Bbr2CongestionController::State state) {
  return (
      state == Bbr2CongestionController::State::ProbeBw_Down ||
      state == Bbr2CongestionController::State::ProbeBw_Cruise ||
      state == Bbr2CongestionController::State::ProbeBw_Refill ||
      state == Bbr2CongestionController::State::ProbeBw_Up);
}

Bandwidth Bbr2CongestionController::getBandwidthSampleFromAck(
    const AckEvent& ackEvent) {
  auto ackTime = ackEvent.adjustedAckTime;
  auto bwSample = Bandwidth();
  for (auto const& ackedPacket : ackEvent.ackedPackets) {
    auto pkt = &ackedPacket;
    if (ackedPacket.outstandingPacketMetadata.encodedSize == 0) {
      continue;
    }
    auto& lastAckedPacket = pkt->lastAckedPacketInfo;
    auto lastSentTime =
        lastAckedPacket ? lastAckedPacket->sentTime : conn_.connectionTime;

    auto sendElapsed = pkt->outstandingPacketMetadata.time - lastSentTime;

    auto lastAckTime = lastAckedPacket ? lastAckedPacket->adjustedAckTime
                                       : conn_.connectionTime;
    auto ackElapsed = ackTime - lastAckTime;
    auto interval = std::max(ackElapsed, sendElapsed);
    if (interval == 0us) {
      return Bandwidth();
    }
    auto lastBytesDelivered =
        lastAckedPacket ? lastAckedPacket->totalBytesAcked : 0;
    auto bytesDelivered = ackEvent.totalBytesAcked - lastBytesDelivered;
    Bandwidth bw(
        bytesDelivered,
        std::chrono::duration_cast<std::chrono::microseconds>(interval),
        pkt->isAppLimited || lastSentTime < appLimitedLastSendTime_);
    if (bw > bwSample) {
      bwSample = bw;
    }
  }
  return bwSample;
}

bool Bbr2CongestionController::isRenoCoexistenceProbeTime() {
  if (!conn_.transportSettings.ccaConfig.enableRenoCoexistence) {
    return false;
  }
  auto renoBdpInPackets = std::min(getTargetInflightWithGain(), cwndBytes_) /
      conn_.udpSendPacketLen;
  auto roundsBeforeRenoProbe =
      std::min(renoBdpInPackets, decltype(renoBdpInPackets)(63));
  return roundsSinceBwProbe_ >= roundsBeforeRenoProbe;
}

Bbr2CongestionController::State Bbr2CongestionController::getState()
    const noexcept {
  return state_;
}

void Bbr2CongestionController::getStats(
    CongestionControllerStats& stats) const {
  stats.bbr2Stats.state = uint8_t(state_);
}

void Bbr2CongestionController::updatePacingAndCwndGain() {
  switch (state_) {
    case State::Startup:
      pacingGain_ =
          conn_.transportSettings.ccaConfig.overrideStartupPacingGain > 0
          ? conn_.transportSettings.ccaConfig.overrideStartupPacingGain
          : kStartupPacingGain;
      cwndGain_ = kStartupCwndGain;
      break;
    case State::Drain:
      pacingGain_ = kDrainPacingGain;
      cwndGain_ = kStartupCwndGain;
      break;
    case State::ProbeBw_Up:
      pacingGain_ = kProbeBwUpPacingGain;
      cwndGain_ = kProbeBwUpCwndGain;
      break;
    case State::ProbeBw_Down:
      pacingGain_ = kProbeBwDownPacingGain;
      cwndGain_ = kProbeBwDownCwndGain;
      break;
    case State::ProbeBw_Cruise:
    case State::ProbeBw_Refill:
      pacingGain_ =
          conn_.transportSettings.ccaConfig.overrideCruisePacingGain > 0
          ? conn_.transportSettings.ccaConfig.overrideCruisePacingGain
          : kProbeBwCruiseRefillPacingGain;
      cwndGain_ = conn_.transportSettings.ccaConfig.overrideCruiseCwndGain > 0
          ? conn_.transportSettings.ccaConfig.overrideCruiseCwndGain
          : kProbeBwCruiseRefillCwndGain;
      break;
    case State::ProbeRTT:
      pacingGain_ = kProbeRttPacingGain;
      cwndGain_ = kProbeRttCwndGain;
      break;
  }
}

std::string bbr2StateToString(Bbr2CongestionController::State state) {
  switch (state) {
    case Bbr2CongestionController::State::Startup:
      return "Startup";
    case Bbr2CongestionController::State::Drain:
      return "Drain";
    case Bbr2CongestionController::State::ProbeBw_Down:
      return "ProbeBw_Down";
    case Bbr2CongestionController::State::ProbeBw_Cruise:
      return "ProbeBw_Cruise";
    case Bbr2CongestionController::State::ProbeBw_Refill:
      return "ProbeBw_Refill";
    case Bbr2CongestionController::State::ProbeBw_Up:
      return "ProbeBw_Up";
    case Bbr2CongestionController::State::ProbeRTT:
      return "ProbeRTT";
  }
  folly::assume_unreachable();
}

} // namespace quic
