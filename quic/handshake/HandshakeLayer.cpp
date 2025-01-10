/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <quic/handshake/HandshakeLayer.h>

namespace quic {

EncryptionLevel protectionTypeToEncryptionLevel(ProtectionType type) {
  switch (type) {
    case ProtectionType::Initial:
      return EncryptionLevel::Initial;
    case ProtectionType::Handshake:
      return EncryptionLevel::Handshake;
    case ProtectionType::ZeroRtt:
      return EncryptionLevel::EarlyData;
    case ProtectionType::KeyPhaseZero:
    case ProtectionType::KeyPhaseOne:
      return EncryptionLevel::AppData;
  }
  folly::assume_unreachable();
}

folly::StringPiece getQuicVersionSalt(QuicVersion version) {
  switch (version) {
    case QuicVersion::MVFST_EXPERIMENTAL4:
      return kQuicExperimentalSalt;
    case QuicVersion::QUIC_V1:
      [[fallthrough]];
    case QuicVersion::QUIC_V1_ALIAS:
      [[fallthrough]];
    case QuicVersion::QUIC_V1_ALIAS2:
      return kQuicV1Salt;
    case QuicVersion::MVFST:
      [[fallthrough]];
    default:
      // Default to one arbitrarily.
      return kQuicDraft23Salt;
  }
}
} // namespace quic
