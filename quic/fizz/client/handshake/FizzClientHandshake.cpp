/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <quic/fizz/client/handshake/FizzClientHandshake.h>

#include <fizz/protocol/Exporter.h>
#include <quic/client/state/ClientStateMachine.h>
#include <quic/codec/QuicPacketBuilder.h>
#include <quic/fizz/client/handshake/FizzClientExtensions.h>
#include <quic/fizz/client/handshake/FizzClientQuicHandshakeContext.h>
#include <quic/fizz/client/handshake/QuicPskCache.h>
#include <quic/fizz/handshake/FizzBridge.h>
#include <quic/fizz/handshake/FizzRetryIntegrityTagGenerator.h>

#include <fizz/client/EarlyDataRejectionPolicy.h>
#include <fizz/protocol/Protocol.h>

namespace quic {

FizzClientHandshake::FizzClientHandshake(
    QuicClientConnectionState* conn,
    std::shared_ptr<FizzClientQuicHandshakeContext> fizzContext,
    std::unique_ptr<FizzCryptoFactory> cryptoFactory)
    : ClientHandshake(conn),
      cryptoFactory_(std::move(cryptoFactory)),
      fizzContext_(std::move(fizzContext)) {
  CHECK(cryptoFactory_->getFizzFactory());
}

Optional<CachedServerTransportParameters> FizzClientHandshake::connectImpl(
    Optional<std::string> hostname) {
  // Look up psk
  Optional<QuicCachedPsk> quicCachedPsk = getPsk(hostname);

  Optional<fizz::client::CachedPsk> cachedPsk;
  Optional<CachedServerTransportParameters> transportParams;
  if (quicCachedPsk) {
    cachedPsk = std::move(quicCachedPsk->cachedPsk);
    transportParams = std::move(quicCachedPsk->transportParams);
  }

  // Setup context for this handshake.
  auto context = std::make_shared<fizz::client::FizzClientContext>(
      *fizzContext_->getContext());
  context->setFactory(cryptoFactory_->getFizzFactory());
  context->setSupportedCiphers({fizz::CipherSuite::TLS_AES_128_GCM_SHA256});
  context->setCompatibilityMode(false);
  // Since Draft-17, EOED should not be sent
  context->setOmitEarlyRecordLayer(true);

  Optional<std::vector<fizz::ech::ECHConfig>> echConfigs;
  if (hostname.hasValue()) {
    echConfigs = fizzContext_->getECHConfigs(*hostname);
  }

  processActions(machine_.processConnect(
      state_,
      std::move(context),
      fizzContext_->getCertificateVerifier(),
      std::move(hostname),
      std::move(cachedPsk),
      std::make_shared<FizzClientExtensions>(getClientTransportParameters()),
      std::move(echConfigs)));

  return transportParams;
}

Optional<QuicCachedPsk> FizzClientHandshake::getPsk(
    const Optional<std::string>& hostname) const {
  auto quicCachedPsk = fizzContext_->getPsk(hostname);
  if (!quicCachedPsk) {
    return none;
  }

  // TODO T32658838 better API to disable early data for current connection
  const QuicClientConnectionState* conn = getClientConn();
  if (!conn->transportSettings.attemptEarlyData) {
    quicCachedPsk->cachedPsk.maxEarlyDataSize = 0;
  } else if (
      conn->earlyDataAppParamsValidator &&
      !conn->earlyDataAppParamsValidator(
          quicCachedPsk->cachedPsk.alpn,
          folly::IOBuf::copyBuffer(quicCachedPsk->appParams))) {
    quicCachedPsk->cachedPsk.maxEarlyDataSize = 0;
    // Do not remove psk here, will let application decide
  }

  return quicCachedPsk;
}

void FizzClientHandshake::removePsk(const Optional<std::string>& hostname) {
  fizzContext_->removePsk(hostname);
}

const CryptoFactory& FizzClientHandshake::getCryptoFactory() const {
  return *cryptoFactory_;
}

const Optional<std::string>& FizzClientHandshake::getApplicationProtocol()
    const {
  auto& earlyDataParams = state_.earlyDataParams();
  if (earlyDataParams) {
    return earlyDataParams->alpn;
  } else {
    return state_.alpn();
  }
}

bool FizzClientHandshake::verifyRetryIntegrityTag(
    const ConnectionId& originalDstConnId,
    const RetryPacket& retryPacket) {
  PseudoRetryPacketBuilder pseudoRetryPacketBuilder(
      retryPacket.initialByte,
      retryPacket.header.getSourceConnId(),
      retryPacket.header.getDestinationConnId(),
      originalDstConnId,
      retryPacket.header.getVersion(),
      folly::IOBuf::copyBuffer(retryPacket.header.getToken()));

  Buf pseudoRetryPacket = std::move(pseudoRetryPacketBuilder).buildPacket();

  FizzRetryIntegrityTagGenerator retryIntegrityTagGenerator;
  auto expectedIntegrityTag = retryIntegrityTagGenerator.getRetryIntegrityTag(
      retryPacket.header.getVersion(), pseudoRetryPacket.get());

  folly::IOBuf integrityTagWrapper = folly::IOBuf::wrapBufferAsValue(
      retryPacket.integrityTag.data(), retryPacket.integrityTag.size());
  return folly::IOBufEqualTo()(*expectedIntegrityTag, integrityTagWrapper);
}

bool FizzClientHandshake::isTLSResumed() const {
  auto pskType = state_.pskType();
  return pskType && *pskType == fizz::PskType::Resumption;
}

Optional<std::vector<uint8_t>> FizzClientHandshake::getExportedKeyingMaterial(
    const std::string& label,
    const Optional<folly::ByteRange>& context,
    uint16_t keyLength) {
  const auto& ems = state_.exporterMasterSecret();
  const auto cipherSuite = state_.cipher();
  if (!ems.hasValue() || !cipherSuite.hasValue()) {
    return none;
  }

  auto ekm = fizz::Exporter::getExportedKeyingMaterial(
      *state_.context()->getFactory(),
      cipherSuite.value(),
      ems.value()->coalesce(),
      label,
      context == none ? nullptr : folly::IOBuf::wrapBuffer(*context),
      keyLength);

  std::vector<uint8_t> result(ekm->coalesce());
  return result;
}

EncryptionLevel FizzClientHandshake::getReadRecordLayerEncryptionLevel() {
  return getEncryptionLevelFromFizz(
      state_.readRecordLayer()->getEncryptionLevel());
}

void FizzClientHandshake::processSocketData(folly::IOBufQueue& queue) {
  processActions(
      machine_.processSocketData(state_, queue, fizz::Aead::AeadOptions()));
}

bool FizzClientHandshake::matchEarlyParameters() {
  return fizz::client::earlyParametersMatch(state_);
}

std::unique_ptr<Aead> FizzClientHandshake::buildAead(
    CipherKind kind,
    folly::ByteRange secret) {
  bool isEarlyTraffic = kind == CipherKind::ZeroRttWrite;
  fizz::CipherSuite cipher =
      isEarlyTraffic ? state_.earlyDataParams()->cipher : *state_.cipher();
  std::unique_ptr<fizz::KeyScheduler> keySchedulerPtr = isEarlyTraffic
      ? state_.context()->getFactory()->makeKeyScheduler(cipher)
      : nullptr;
  fizz::KeyScheduler& keyScheduler =
      isEarlyTraffic ? *keySchedulerPtr : *state_.keyScheduler();

  auto aead = FizzAead::wrap(fizz::Protocol::deriveRecordAeadWithLabel(
      *state_.context()->getFactory(),
      keyScheduler,
      cipher,
      secret,
      kQuicKeyLabel,
      kQuicIVLabel));

  return aead;
}
std::unique_ptr<PacketNumberCipher> FizzClientHandshake::buildHeaderCipher(
    folly::ByteRange secret) {
  return cryptoFactory_->makePacketNumberCipher(secret);
}

Buf FizzClientHandshake::getNextTrafficSecret(folly::ByteRange secret) const {
  auto deriver =
      state_.context()->getFactory()->makeKeyDeriver(*state_.cipher());
  auto nextSecret = deriver->expandLabel(
      secret, kQuicKULabel, folly::IOBuf::create(0), secret.size());
  return nextSecret;
}

void FizzClientHandshake::onNewCachedPsk(
    fizz::client::NewCachedPsk& newCachedPsk) noexcept {
  QuicClientConnectionState* conn = getClientConn();
  DCHECK(conn->version.has_value());
  DCHECK(conn->serverInitialParamsSet_);

  QuicCachedPsk quicCachedPsk;
  quicCachedPsk.cachedPsk = std::move(newCachedPsk.psk);
  quicCachedPsk.transportParams = getServerCachedTransportParameters(*conn);

  if (conn->earlyDataAppParamsGetter) {
    auto appParams = conn->earlyDataAppParamsGetter();
    if (appParams) {
      quicCachedPsk.appParams = appParams->to<std::string>();
    }
  }

  fizzContext_->putPsk(state_.sni(), std::move(quicCachedPsk));
}

void FizzClientHandshake::echRetryAvailable(
    fizz::client::ECHRetryAvailable& retry) {
  if (echRetryCallback_) {
    echRetryCallback_->retryAvailable(retry);
  }
}

class FizzClientHandshake::ActionMoveVisitor {
 public:
  explicit ActionMoveVisitor(FizzClientHandshake& client) : client_(client) {}

  void operator()(fizz::DeliverAppData&) {
    client_.raiseError(folly::make_exception_wrapper<QuicTransportException>(
        "Invalid app data on crypto stream",
        TransportErrorCode::PROTOCOL_VIOLATION));
  }

  void operator()(fizz::WriteToSocket& write) {
    for (auto& content : write.contents) {
      auto encryptionLevel =
          getEncryptionLevelFromFizz(content.encryptionLevel);
      client_.writeDataToStream(encryptionLevel, std::move(content.data));
    }
  }

  void operator()(fizz::client::ReportEarlyHandshakeSuccess&) {
    client_.computeZeroRttCipher();
  }

  void operator()(fizz::client::ReportHandshakeSuccess& handshakeSuccess) {
    client_.computeOneRttCipher(handshakeSuccess.earlyDataAccepted);
  }

  void operator()(fizz::client::ReportEarlyWriteFailed&) {
    LOG(DFATAL) << "QUIC TLS app data write";
  }

  void operator()(fizz::ReportError& err) {
    auto errMsg = err.error.what();
    if (errMsg.empty()) {
      errMsg = "Error during handshake";
    }

    auto fe = err.error.get_exception<fizz::FizzException>();

    if (fe && fe->getAlert()) {
      auto alertNum =
          static_cast<std::underlying_type<TransportErrorCode>::type>(
              fe->getAlert().value());
      alertNum += static_cast<std::underlying_type<TransportErrorCode>::type>(
          TransportErrorCode::CRYPTO_ERROR);
      client_.raiseError(folly::make_exception_wrapper<QuicTransportException>(
          errMsg.toStdString(), static_cast<TransportErrorCode>(alertNum)));
    } else {
      client_.raiseError(folly::make_exception_wrapper<QuicTransportException>(
          errMsg.toStdString(),
          static_cast<TransportErrorCode>(
              fizz::AlertDescription::internal_error)));
    }
  }

  void operator()(fizz::WaitForData&) {
    client_.waitForData();
  }

  void operator()(fizz::client::MutateState& mutator) {
    mutator(client_.state_);
  }

  void operator()(fizz::client::NewCachedPsk& newCachedPsk) {
    client_.onNewCachedPsk(newCachedPsk);
  }

  void operator()(fizz::EndOfData&) {
    client_.raiseError(folly::make_exception_wrapper<QuicTransportException>(
        "unexpected close notify", TransportErrorCode::INTERNAL_ERROR));
  }

  void operator()(fizz::SecretAvailable& secretAvailable) {
    switch (secretAvailable.secret.type.type()) {
      case fizz::SecretType::Type::EarlySecrets_E:
        switch (*secretAvailable.secret.type.asEarlySecrets()) {
          case fizz::EarlySecrets::ClientEarlyTraffic:
            client_.computeCiphers(
                CipherKind::ZeroRttWrite,
                folly::range(secretAvailable.secret.secret));
            break;
          default:
            break;
        }
        break;
      case fizz::SecretType::Type::HandshakeSecrets_E:
        switch (*secretAvailable.secret.type.asHandshakeSecrets()) {
          case fizz::HandshakeSecrets::ClientHandshakeTraffic:
            client_.computeCiphers(
                CipherKind::HandshakeWrite,
                folly::range(secretAvailable.secret.secret));
            break;
          case fizz::HandshakeSecrets::ServerHandshakeTraffic:
            client_.computeCiphers(
                CipherKind::HandshakeRead,
                folly::range(secretAvailable.secret.secret));
            break;
          case fizz::HandshakeSecrets::ECHAcceptConfirmation:
            break;
        }
        break;
      case fizz::SecretType::Type::AppTrafficSecrets_E:
        switch (*secretAvailable.secret.type.asAppTrafficSecrets()) {
          case fizz::AppTrafficSecrets::ClientAppTraffic:
            client_.computeCiphers(
                CipherKind::OneRttWrite,
                folly::range(secretAvailable.secret.secret));
            break;
          case fizz::AppTrafficSecrets::ServerAppTraffic:
            client_.computeCiphers(
                CipherKind::OneRttRead,
                folly::range(secretAvailable.secret.secret));
            break;
        }
        break;
      case fizz::SecretType::Type::MasterSecrets_E:
        break;
    }
  }

  void operator()(fizz::client::ECHRetryAvailable& retry) {
    client_.echRetryAvailable(retry);
  }

 private:
  FizzClientHandshake& client_;
};

void FizzClientHandshake::processActions(fizz::client::Actions actions) {
  ActionMoveVisitor visitor(*this);
  for (auto& action : actions) {
    switch (action.type()) {
      case fizz::client::Action::Type::DeliverAppData_E:
        visitor(*action.asDeliverAppData());
        break;
      case fizz::client::Action::Type::WriteToSocket_E:
        visitor(*action.asWriteToSocket());
        break;
      case fizz::client::Action::Type::ReportHandshakeSuccess_E:
        visitor(*action.asReportHandshakeSuccess());
        break;
      case fizz::client::Action::Type::ReportEarlyHandshakeSuccess_E:
        visitor(*action.asReportEarlyHandshakeSuccess());
        break;
      case fizz::client::Action::Type::ReportEarlyWriteFailed_E:
        visitor(*action.asReportEarlyWriteFailed());
        break;
      case fizz::client::Action::Type::ReportError_E:
        visitor(*action.asReportError());
        break;
      case fizz::client::Action::Type::EndOfData_E:
        visitor(*action.asEndOfData());
        break;
      case fizz::client::Action::Type::MutateState_E:
        visitor(*action.asMutateState());
        break;
      case fizz::client::Action::Type::WaitForData_E:
        visitor(*action.asWaitForData());
        break;
      case fizz::client::Action::Type::NewCachedPsk_E:
        visitor(*action.asNewCachedPsk());
        break;
      case fizz::client::Action::Type::SecretAvailable_E:
        visitor(*action.asSecretAvailable());
        break;
      case fizz::client::Action::Type::ECHRetryAvailable_E:
        visitor(*action.asECHRetryAvailable());
        break;
    }
  }
}

} // namespace quic
