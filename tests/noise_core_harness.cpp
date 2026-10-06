/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include <xplat/noise/core/PsaCryptoBackend.h>
#include <xplat/noise/core/Transport.h>

namespace tn = musegadgets::noise::core;

namespace {

int CheckRepeatedAesGcmMessages(tn::PsaCryptoBackend& backend) {
  std::array<uint8_t, tn::CryptoBackend::kAes256GcmKeySize> key{};
  std::array<uint8_t, tn::CryptoBackend::kAes256GcmNonceSize> nonce{};
  for (size_t i = 0; i < key.size(); ++i) {
    key[i] = static_cast<uint8_t>(0x70 + i);
  }

  for (size_t message = 0; message < 4; ++message) {
    nonce.fill(0);
    nonce.back() = static_cast<uint8_t>(message);
    std::vector<uint8_t> plaintext(257 + message);
    for (size_t i = 0; i < plaintext.size(); ++i) {
      plaintext[i] = static_cast<uint8_t>((i + message) & 0xffu);
    }
    std::vector<uint8_t> ciphertextAndTag(
        plaintext.size() + tn::CryptoBackend::kAes256GcmTagSize);

    if (!backend
             .Aes256GcmSeal(
                 tn::ConstByteSpan(key),
                 tn::ConstByteSpan(nonce),
                 tn::ConstByteSpan(),
                 tn::ConstByteSpan(plaintext.data(), plaintext.size()),
                 tn::ByteSpan(
                     ciphertextAndTag.data(), ciphertextAndTag.size()))
             .ok()) {
      return 10;
    }

    std::vector<uint8_t> opened(plaintext.size());
    if (!backend
             .Aes256GcmOpen(
                 tn::ConstByteSpan(key),
                 tn::ConstByteSpan(nonce),
                 tn::ConstByteSpan(),
                 tn::ConstByteSpan(
                     ciphertextAndTag.data(), ciphertextAndTag.size()),
                 tn::ByteSpan(opened.data(), opened.size()))
             .ok()) {
      return 11;
    }
    if (opened != plaintext) {
      return 12;
    }
  }
  return 0;
}

int CheckLargeExactInPlaceAesGcm(tn::PsaCryptoBackend& backend) {
  std::array<uint8_t, tn::CryptoBackend::kAes256GcmKeySize> key{};
  std::array<uint8_t, tn::CryptoBackend::kAes256GcmNonceSize> nonce{};
  for (size_t i = 0; i < key.size(); ++i) {
    key[i] = static_cast<uint8_t>(0xe0 + i);
  }
  for (size_t i = 0; i < nonce.size(); ++i) {
    nonce[i] = static_cast<uint8_t>(0x50 + i);
  }

  std::vector<uint8_t> plaintext(8u * 1024u);
  for (size_t i = 0; i < plaintext.size(); ++i) {
    plaintext[i] = static_cast<uint8_t>((i * 31u) & 0xffu);
  }
  std::vector<uint8_t> buffer(
      plaintext.size() + tn::CryptoBackend::kAes256GcmTagSize);
  std::copy(plaintext.begin(), plaintext.end(), buffer.begin());

  if (!backend
           .Aes256GcmSeal(
               tn::ConstByteSpan(key),
               tn::ConstByteSpan(nonce),
               tn::ConstByteSpan(),
               tn::ConstByteSpan(buffer.data(), plaintext.size()),
               tn::ByteSpan(buffer.data(), buffer.size()))
           .ok()) {
    return 13;
  }

  std::vector<uint8_t> opened(plaintext.size());
  if (!backend
           .Aes256GcmOpen(
               tn::ConstByteSpan(key),
               tn::ConstByteSpan(nonce),
               tn::ConstByteSpan(),
               tn::ConstByteSpan(buffer.data(), buffer.size()),
               tn::ByteSpan(opened.data(), opened.size()))
           .ok()) {
    return 14;
  }
  if (opened != plaintext) {
    return 15;
  }
  return 0;
}

}  // namespace

int main() {
  tn::PsaCryptoBackend backend;

  const int repeatedAesGcmResult = CheckRepeatedAesGcmMessages(backend);
  if (repeatedAesGcmResult != 0) {
    return repeatedAesGcmResult;
  }
  const int inPlaceAesGcmResult = CheckLargeExactInPlaceAesGcm(backend);
  if (inPlaceAesGcmResult != 0) {
    return inPlaceAesGcmResult;
  }

  std::array<uint8_t, tn::CryptoBackend::kX25519KeySize> clientPrivate{};
  std::array<uint8_t, tn::CryptoBackend::kX25519KeySize> clientPublic{};
  std::array<uint8_t, tn::CryptoBackend::kX25519KeySize> serverPrivate{};
  std::array<uint8_t, tn::CryptoBackend::kX25519KeySize> serverPublic{};
  std::array<uint8_t, tn::CryptoBackend::kX25519KeySize> clientShared{};
  std::array<uint8_t, tn::CryptoBackend::kX25519KeySize> serverShared{};

  if (!backend
           .X25519GenerateKeypair(tn::ByteSpan(clientPrivate),
                                  tn::ByteSpan(clientPublic))
           .ok()) {
    return 1;
  }
  if (!backend
           .X25519GenerateKeypair(tn::ByteSpan(serverPrivate),
                                  tn::ByteSpan(serverPublic))
           .ok()) {
    return 2;
  }
  if (!backend
           .X25519Dh(tn::ConstByteSpan(clientPrivate),
                     tn::ConstByteSpan(serverPublic),
                     tn::ByteSpan(clientShared))
           .ok()) {
    return 3;
  }
  if (!backend
           .X25519Dh(tn::ConstByteSpan(serverPrivate),
                     tn::ConstByteSpan(clientPublic),
                     tn::ByteSpan(serverShared))
           .ok()) {
    return 4;
  }
  if (!std::equal(clientShared.begin(), clientShared.end(),
                  serverShared.begin())) {
    return 5;
  }

  std::array<uint8_t, tn::Transport::kKeySize> sendKey{};
  std::array<uint8_t, tn::Transport::kKeySize> recvKey{};
  for (size_t i = 0; i < sendKey.size(); ++i) {
    sendKey[i] = static_cast<uint8_t>(i);
    recvKey[i] = static_cast<uint8_t>(0xa0 + i);
  }

  tn::Transport outbound(backend, tn::ConstByteSpan(sendKey),
                         tn::ConstByteSpan(recvKey));
  tn::Transport inbound(backend, tn::ConstByteSpan(recvKey),
                        tn::ConstByteSpan(sendKey));
  if (!outbound.status().ok() || !inbound.status().ok()) {
    return 6;
  }

  constexpr std::array<uint8_t, 16> plaintext{'t', 'i', 'g', 'o', 'n', '-',
                                              'n', 'o', 'i', 's', 'e', '-',
                                              'c', 'o', 'r', 'e'};
  std::array<uint8_t, plaintext.size() + tn::Transport::kTagSize> ciphertext{};
  const tn::StatusWithSize sealed =
      outbound.Seal(tn::ConstByteSpan(plaintext), tn::ByteSpan(ciphertext));
  if (!sealed.ok() || sealed.size() != ciphertext.size()) {
    return 7;
  }

  std::array<uint8_t, plaintext.size()> opened{};
  const tn::StatusWithSize openedSize =
      inbound.Open(tn::ConstByteSpan(ciphertext), tn::ByteSpan(opened));
  if (!openedSize.ok() || openedSize.size() != opened.size()) {
    return 8;
  }
  if (!std::equal(opened.begin(), opened.end(), plaintext.begin())) {
    return 9;
  }

  return 0;
}
