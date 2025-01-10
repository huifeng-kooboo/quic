/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <folly/ExceptionWrapper.h>
#include <folly/io/IOBufQueue.h>
#include <folly/io/async/DelayedDestruction.h>

#include <quic/QuicConstants.h>
#include <quic/QuicException.h>
#include <quic/handshake/Aead.h>
#include <quic/handshake/HandshakeLayer.h>

namespace quic {

class CryptoFactory;
struct CachedServerTransportParameters;
struct ClientTransportParametersExtension;
struct QuicClientConnectionState;
struct ServerTransportParameters;

class ClientHandshake : public Handshake {
 public:
  enum class Phase { Initial, Handshake, OneRttKeysDerived, Established };

  explicit ClientHandshake(QuicClientConnectionState* conn);

  /**
   * Initiate the handshake with the supplied parameters.
   */
  void connect(
      Optional<std::string> hostname,
      std::shared_ptr<ClientTransportParametersExtension> transportParams);

  /**
   * Takes input bytes from the network and processes then in the handshake.
   * This can change the state of the transport which may result in ciphers
   * being initialized, bytes written out, or the write phase changing.
   */
  virtual void doHandshake(
      std::unique_ptr<folly::IOBuf> data,
      EncryptionLevel encryptionLevel);

  /**
   * Provides facilities to get, put and remove a PSK from the cache in case the
   * handshake supports a PSK cache.
   */
  virtual void removePsk(const Optional<std::string>& /* hostname */) {}

  /**
   * Returns a reference to the CryptoFactory used internally.
   */
  virtual const CryptoFactory& getCryptoFactory() const = 0;

  /**
   * An API to get oneRttWriteCiphers on key rotation. Each call will return a
   * one rtt write cipher using the current traffic secret and advance the
   * traffic secret.
   */
  std::unique_ptr<Aead> getNextOneRttWriteCipher() override;

  /**
   * An API to get oneRttReadCiphers on key rotation. Each call will return a
   * one rtt read cipher using the current traffic secret and advance the
   * traffic secret.
   */
  std::unique_ptr<Aead> getNextOneRttReadCipher() override;

  /**
   * Triggered when we have received a handshake done frame from the server.
   */
  void handshakeConfirmed() override;

  Phase getPhase() const;

  /**
   * Was the TLS connection resumed or not.
   */
  virtual bool isTLSResumed() const = 0;

  /**
   * Edge triggered api to obtain whether or not zero rtt data was rejected.
   * If zero rtt was never attempted, then this will return none. Once
   * the result is obtained, the result is cleared out.
   */
  Optional<bool> getZeroRttRejected();

  /**
   * If zero-rtt is rejected, this will indicate whether zero-rtt data can be
   * resent on the connection or the connection has to be closed.
   */
  Optional<bool> getCanResendZeroRtt() const;

  /**
   * API used to verify that the integrity token present in the retry packet
   * matches what we would expect
   */
  virtual bool verifyRetryIntegrityTag(
      const ConnectionId& originalDstConnId,
      const RetryPacket& retryPacket) = 0;

  /**
   * Returns the negotiated transport parameters chosen by the server
   */
  virtual const Optional<ServerTransportParameters>& getServerTransportParams();

  virtual double getCertificateVerifyStartTimeMS() const {
    return 0.0;
  }

  virtual double getCertificateVerifyEndTimeMS() const {
    return 0.0;
  }

  ~ClientHandshake() override = default;

 protected:
  enum class CipherKind {
    HandshakeWrite,
    HandshakeRead,
    OneRttWrite,
    OneRttRead,
    ZeroRttWrite,
  };

  void computeCiphers(CipherKind kind, folly::ByteRange secret);

  /**
   * Various utilities for concrete implementations to use.
   */
  void raiseError(folly::exception_wrapper error);
  void throwOnError();
  void waitForData();
  void writeDataToStream(EncryptionLevel encryptionLevel, Buf data);
  void handshakeInitiated();
  void computeZeroRttCipher();
  void computeOneRttCipher(bool earlyDataAccepted);

  /**
   * Accessor for the concrete implementation, so they can access data without
   * being able to rebind it.
   */
  QuicClientConnectionState* getClientConn();
  const QuicClientConnectionState* getClientConn() const;
  const std::shared_ptr<ClientTransportParametersExtension>&
  getClientTransportParameters() const;

  /**
   * Setters for the concrete implementation so that it can be tested.
   */
  void setZeroRttRejectedForTest(bool rejected);
  void setCanResendZeroRttForTest(bool canResendZeroRtt);

  /**
   * Given secret_n, returns secret_n+1 to be used for generating the next Aead
   * on key updates.
   */
  virtual Buf getNextTrafficSecret(folly::ByteRange secret) const = 0;

  Buf readTrafficSecret_;
  Buf writeTrafficSecret_;

  Optional<bool> zeroRttRejected_;
  Optional<bool> canResendZeroRtt_;

 private:
  virtual Optional<CachedServerTransportParameters> connectImpl(
      Optional<std::string> hostname) = 0;

  virtual EncryptionLevel getReadRecordLayerEncryptionLevel() = 0;
  virtual void processSocketData(folly::IOBufQueue& queue) = 0;
  virtual bool matchEarlyParameters() = 0;
  virtual std::unique_ptr<Aead> buildAead(
      CipherKind kind,
      folly::ByteRange secret) = 0;
  virtual std::unique_ptr<PacketNumberCipher> buildHeaderCipher(
      folly::ByteRange secret) = 0;

  // Represents the packet type that should be used to write the data currently
  // in the stream.
  Phase phase_{Phase::Initial};

  QuicClientConnectionState* conn_;
  std::shared_ptr<ClientTransportParametersExtension> transportParams_;

  bool waitForData_{false};
  bool earlyDataAttempted_{false};

  folly::IOBufQueue initialReadBuf_{folly::IOBufQueue::cacheChainLength()};
  folly::IOBufQueue handshakeReadBuf_{folly::IOBufQueue::cacheChainLength()};
  folly::IOBufQueue appDataReadBuf_{folly::IOBufQueue::cacheChainLength()};

  // This variable is incremented every time a read traffic secret is rotated,
  // and decremented for the write secret. Its value should be
  // between -1 and 1. A value outside of this range indicates that the
  // transport's read and write ciphers are likely out of sync.
  int8_t trafficSecretSync_{0};

  folly::exception_wrapper error_;
};

} // namespace quic
