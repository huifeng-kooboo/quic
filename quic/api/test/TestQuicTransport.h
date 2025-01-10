/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <quic/api/QuicTransportBase.h>
#include <quic/api/QuicTransportFunctions.h>
#include <quic/common/test/TestUtils.h>
#include <quic/dsr/frontend/WriteFunctions.h>
#include <quic/fizz/server/handshake/FizzServerQuicHandshakeContext.h>

namespace quic {

class TestQuicTransport
    : public QuicTransportBase,
      public std::enable_shared_from_this<TestQuicTransport> {
 public:
  TestQuicTransport(
      std::shared_ptr<QuicEventBase> evb,
      std::unique_ptr<QuicAsyncUDPSocket> socket,
      ConnectionSetupCallback* connSetupCb,
      ConnectionCallback* connCb)
      : QuicTransportBaseLite(evb, std::move(socket)), QuicTransportBase(evb, nullptr /* Initialized through the QuicTransportBaseLite constructor */), observerContainer_(std::make_shared<SocketObserverContainer>(this)) {
    conn_.reset(new QuicServerConnectionState(
        FizzServerQuicHandshakeContext::Builder().build()));
    conn_->clientConnectionId = ConnectionId({9, 8, 7, 6});
    conn_->serverConnectionId = ConnectionId({1, 2, 3, 4});
    conn_->version = QuicVersion::MVFST;
    conn_->observerContainer = observerContainer_;
    aead = test::createNoOpAead();
    headerCipher = test::createNoOpHeaderCipher();
    setConnectionSetupCallback(connSetupCb);
    setConnectionCallbackFromCtor(connCb);
  }

  ~TestQuicTransport() override {
    // we need to call close in the derived class.
    resetConnectionCallbacks();
    closeImpl(
        QuicError(
            QuicErrorCode(LocalErrorCode::SHUTTING_DOWN),
            std::string("shutdown")),
        false /* drainConnection */);
    // closeImpl may have been called earlier with drain = true, so force close.
    closeUdpSocket();
  }

  WriteResult writeBufMeta(
      StreamId /* id */,
      const BufferMeta& /* data */,
      bool /* eof */,
      ByteEventCallback* /* cb */) override {
    return folly::makeUnexpected(LocalErrorCode::INVALID_OPERATION);
  }

  WriteResult setDSRPacketizationRequestSender(
      StreamId /* id */,
      std::unique_ptr<DSRPacketizationRequestSender> /* sender */) override {
    return folly::makeUnexpected(LocalErrorCode::INVALID_OPERATION);
  }

  Optional<std::vector<TransportParameter>> getPeerTransportParams()
      const override {
    return none;
  }

  QuicVersion getVersion() {
    auto& conn = getConnectionState();
    return conn.version.value_or(*conn.originalVersion);
  }

  void updateWriteLooper(bool thisIteration, bool /* runInline */ = false) {
    QuicTransportBase::updateWriteLooper(thisIteration);
  }

  void pacedWrite() {
    pacedWriteDataToSocket();
  }

  bool isPacingScheduled() {
    return writeLooper_->isPacingScheduled();
  }

  void onReadData(
      const folly::SocketAddress& /* peer */,
      ReceivedUdpPacket&& /* udpPacket */) noexcept override {}

  void writeData() override {
    if (closed) {
      return;
    }
    writeQuicDataToSocket(
        *socket_,
        *conn_,
        *conn_->clientConnectionId,
        *conn_->serverConnectionId,
        *aead,
        *headerCipher,
        getVersion(),
        (isConnectionPaced(*conn_)
             ? conn_->pacer->updateAndGetWriteBatchSize(Clock::now())
             : conn_->transportSettings.writeConnectionDataPacketsLimit));
    writePacketizationRequest(
        *dynamic_cast<QuicServerConnectionState*>(conn_.get()),
        *conn_->clientConnectionId,
        (isConnectionPaced(*conn_)
             ? conn_->pacer->updateAndGetWriteBatchSize(Clock::now())
             : conn_->transportSettings.writeConnectionDataPacketsLimit),
        *aead,
        Clock::now());
  }

  void closeTransport() override {
    closed = true;
  }

  bool hasWriteCipher() const override {
    return true;
  }

  std::shared_ptr<QuicTransportBaseLite> sharedGuard() override {
    return shared_from_this();
  }

  void unbindConnection() override {}

  QuicServerConnectionState& getConnectionState() {
    return *dynamic_cast<QuicServerConnectionState*>(conn_.get());
  }

  auto getAckTimeout() {
    return &ackTimeout_;
  }

  auto& getPathValidationTimeout() {
    return pathValidationTimeout_;
  }

  auto& lossTimeout() {
    return lossTimeout_;
  }

  auto& idleTimeout() {
    return idleTimeout_;
  }

  auto& keepalivetimeout() {
    return keepaliveTimeout_;
  }

  CloseState closeState() {
    return closeState_;
  }

  void drainImmediately() {
    drainTimeoutExpired();
  }

  void setIdleTimerNow() {
    setIdleTimer();
  }

  void invokeNotifyStartWritingFromAppRateLimited() {
    notifyStartWritingFromAppRateLimited();
  }

  void invokeNotifyPacketsWritten(
      const uint64_t numPacketsWritten,
      const uint64_t numAckElicitingPacketsWritten,
      const uint64_t numBytesWritten) {
    notifyPacketsWritten(
        numPacketsWritten, numAckElicitingPacketsWritten, numBytesWritten);
  }

  void invokeNotifyAppRateLimited() {
    notifyAppRateLimited();
  }

  void invokeHandleKnobCallbacks() {
    handleKnobCallbacks();
  }

  void setTransportReadyNotified(bool transportReadyNotified) {
    transportReadyNotified_ = transportReadyNotified;
  }

  SocketObserverContainer* getSocketObserverContainer() const override {
    return observerContainer_.get();
  }

  Optional<std::vector<uint8_t>> getExportedKeyingMaterial(
      const std::string&,
      const Optional<folly::ByteRange>&,
      uint16_t) const override {
    return none;
  }

  void validateECN() {
    QuicTransportBase::validateECNState();
  }

  std::unique_ptr<Aead> aead;
  std::unique_ptr<PacketNumberCipher> headerCipher;
  bool closed{false};

  // Container of observers for the socket / transport.
  //
  // This member MUST be last in the list of members to ensure it is destroyed
  // first, before any other members are destroyed. This ensures that observers
  // can inspect any socket / transport state available through public methods
  // when destruction of the transport begins.
  const std::shared_ptr<SocketObserverContainer> observerContainer_;
};

} // namespace quic
