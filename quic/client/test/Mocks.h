/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <folly/portability/GMock.h>
#include <quic/client/QuicClientTransport.h>
#include <quic/client/connector/QuicConnector.h>
#include <quic/client/handshake/CachedServerTransportParameters.h>
#include <quic/client/handshake/ClientHandshake.h>
#include <quic/client/handshake/ClientHandshakeFactory.h>
#include <quic/common/events/FollyQuicEventBase.h>
#include <quic/common/udpsocket/FollyQuicAsyncUDPSocket.h>
#include <quic/handshake/CryptoFactory.h>
#include <quic/handshake/TransportParameters.h>

namespace quic::test {

class MockClientHandshakeFactory : public ClientHandshakeFactory {
 public:
  MOCK_METHOD(
      std::unique_ptr<ClientHandshake>,
      _makeClientHandshake,
      (QuicClientConnectionState*));

  std::unique_ptr<ClientHandshake>
      makeClientHandshake(QuicClientConnectionState* conn) && override {
    return _makeClientHandshake(conn);
  }
};

class MockClientHandshake : public ClientHandshake {
 public:
  MockClientHandshake(QuicClientConnectionState* conn)
      : ClientHandshake(conn) {}
  ~MockClientHandshake() override {
    destroy();
  }
  // Legacy workaround for move-only types
  void doHandshake(
      std::unique_ptr<folly::IOBuf> data,
      EncryptionLevel encryptionLevel) override {
    doHandshakeImpl(data.get(), encryptionLevel);
  }
  MOCK_METHOD(void, doHandshakeImpl, (folly::IOBuf*, EncryptionLevel));
  MOCK_METHOD(
      bool,
      verifyRetryIntegrityTag,
      (const ConnectionId&, const RetryPacket&));
  MOCK_METHOD(void, removePsk, (const Optional<std::string>&));
  MOCK_METHOD(const CryptoFactory&, getCryptoFactory, (), (const));
  MOCK_METHOD(bool, isTLSResumed, (), (const));
  MOCK_METHOD(
      Optional<std::vector<uint8_t>>,
      getExportedKeyingMaterial,
      (const std::string& label,
       const Optional<folly::ByteRange>& context,
       uint16_t keyLength),
      ());
  MOCK_METHOD(Optional<bool>, getZeroRttRejected, ());
  MOCK_METHOD(
      const Optional<ServerTransportParameters>&,
      getServerTransportParams,
      ());
  MOCK_METHOD(void, destroy, ());

  MOCK_METHOD(
      Optional<CachedServerTransportParameters>,
      connectImpl,
      (Optional<std::string>));
  MOCK_METHOD(EncryptionLevel, getReadRecordLayerEncryptionLevel, ());
  MOCK_METHOD(void, processSocketData, (folly::IOBufQueue & queue));
  MOCK_METHOD(bool, matchEarlyParameters, ());
  MOCK_METHOD(
      std::unique_ptr<Aead>,
      buildAead,
      (ClientHandshake::CipherKind kind, folly::ByteRange secret));
  MOCK_METHOD(
      std::unique_ptr<PacketNumberCipher>,
      buildHeaderCipher,
      (folly::ByteRange secret));
  MOCK_METHOD(Buf, getNextTrafficSecret, (folly::ByteRange secret), (const));
  MOCK_METHOD(
      const Optional<std::string>&,
      getApplicationProtocol,
      (),
      (const));
};

class MockQuicConnectorCallback : public quic::QuicConnector::Callback {
 public:
  MOCK_METHOD(void, onConnectError, (QuicError));
  MOCK_METHOD(void, onConnectSuccess, ());
};

class MockQuicClientTransport : public quic::QuicClientTransport {
 public:
  enum class TestType : uint8_t { Success = 0, Failure, Timeout };

  explicit MockQuicClientTransport(
      TestType testType,
      std::shared_ptr<QuicEventBase> evb,
      std::unique_ptr<QuicAsyncUDPSocket> socket,
      std::shared_ptr<ClientHandshakeFactory> handshakeFactory)
      : QuicTransportBaseLite(evb, std::move(socket)),
        QuicClientTransport(
            evb,
            nullptr /* Initialized through the QuicTransportBaseLite constructor
                     */
            ,
            std::move(handshakeFactory)),
        testType_(testType) {}

  void start(ConnectionSetupCallback* connSetupCb, ConnectionCallback*)
      override {
    auto cancelCode = QuicError(
        QuicErrorCode(LocalErrorCode::NO_ERROR),
        toString(LocalErrorCode::NO_ERROR).str());

    switch (testType_) {
      case TestType::Success:
        connSetupCb->onReplaySafe();
        break;
      case TestType::Failure:
        connSetupCb->onConnectionSetupError(std::move(cancelCode));
        break;
      case TestType::Timeout:
        // Do nothing and let it timeout.
        break;
    }
  }

 private:
  TestType testType_;
};

} // namespace quic::test
